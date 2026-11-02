#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Scheduler evidence tests; no VM, panel inputs or physical deck access."""
import importlib.util
from pathlib import Path
import sys
import unittest
import os
import shutil
import subprocess
import tempfile
import threading
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/integration'))
import sdk_emulator_diagnostics as diagnostic

def snapshot(uptime=10, runtime=100, wait=20, birth=42):
    # Linux stat starts at field3 after the comm, which may contain spaces/parentheses.
    tail=['S']+['0']*49
    tail[11]='7';tail[12]='3';tail[15]='20';tail[16]='0';tail[19]=str(birth)
    tail[36]='2';tail[37]='0';tail[38]='0'
    line='123 (JUCE ALSA ) worker) '+' '.join(tail)
    return ('AZSDK_SCHED 1\npid 123\nbirth '+str(birth)+'\nuptime '+str(uptime)+
            '\n/proc/123/task/123/stat '+line+'\n/proc/123/task/123/schedstat '+str(runtime)+' '+str(wait)+' 4'+
            '\n/proc/123/task/123/wchan futex_wait_queue\nEND 123 '+str(birth)+'\n')

class SchedulerDiagnosticsTests(unittest.TestCase):
    def test_stat_comm_and_fields_are_bound_to_one_pid_and_birth(self):
        result=diagnostic.parse_scheduler_snapshot(snapshot(),'123','42')
        thread=result['threads']['123']
        self.assertEqual(thread['name'],'JUCE ALSA ) worker')
        self.assertEqual((thread['state'],thread['user_ticks'],thread['system_ticks'],thread['processor']),('S',7,3,2))
        self.assertEqual((thread['runtime_ns'],thread['runqueue_ns'],thread['timeslices']),(100,20,4))
        self.assertEqual(thread['wchan'],'futex_wait_queue')
    def test_changed_identity_duplicate_partial_and_excessive_capture_are_refused(self):
        for value in (snapshot(birth=43),snapshot().replace('END 123 42\n',''),
                      snapshot()+snapshot(),snapshot().replace('/proc/123/task/123/stat','/proc/124/task/123/stat'),
                      snapshot()+'x'*524289):
            with self.subTest(value=value[:80]),self.assertRaises(ValueError):diagnostic.parse_scheduler_snapshot(value,'123','42')
    def test_delta_reports_cpu_and_scheduler_wait_without_calling_sleep_a_cpu_stall(self):
        a=diagnostic.parse_scheduler_snapshot(snapshot(),'123','42')
        b=diagnostic.parse_scheduler_snapshot(snapshot(uptime=12,runtime=1000000100,wait=500000020),'123','42')
        result=diagnostic.scheduler_delta(a,b)
        self.assertEqual(result['elapsed_s'],2)
        self.assertEqual(result['threads']['123']['runtime_ns'],1000000000)
        self.assertEqual(result['threads']['123']['runqueue_ns'],500000000)
        self.assertEqual(result['threads']['123']['runtime_fraction'],.5)
    def test_clock_reversal_counter_reset_or_thread_reuse_cannot_produce_a_delta(self):
        a=diagnostic.parse_scheduler_snapshot(snapshot(),'123','42')
        for value in (snapshot(uptime=9),snapshot(uptime=12,runtime=1),snapshot(uptime=12).replace(' 42 0 0',' 43 0 0')):
            with self.subTest(value=value),self.assertRaises(ValueError):
                b=diagnostic.parse_scheduler_snapshot(value,'123','42')
                diagnostic.scheduler_delta(a,b)
    def test_fair_profile_requires_observed_normal_batch_or_idle_policies(self):
        value=diagnostic.parse_scheduler_snapshot(snapshot(),'123','42')
        for policy in (0,3,5):
            value['threads']['123']['policy']=policy
            proof=diagnostic.require_fair_scheduler(value)
            self.assertEqual(proof['threads'],1)
            self.assertEqual(proof['policies'],{str(policy):1})
        for policy in (1,2,6,1073741824):
            value['threads']['123']['policy']=policy
            with self.subTest(policy=policy),self.assertRaises(ValueError):diagnostic.require_fair_scheduler(value)

    def test_sampler_has_both_birth_guards_count_and_output_bounds_without_target_writes(self):
        script=diagnostic.scheduler_snapshot_script('123','42')
        self.assertEqual(script.count('|| exit 93'),2)
        self.assertIn('512',script);self.assertIn('CAP=506880',script);self.assertIn('bytes > CAP',script);self.assertNotIn('| head',script)
        self.assertNotRegex(script,r'(?:printf|echo|print) [^\n]* >');self.assertNotIn('touch ',script)
        for pid,birth in [('1;reboot','42'),('123','0'),('0','42'),('123','42\n')]:
            with self.subTest(pid=pid,birth=birth),self.assertRaises(ValueError):diagnostic.scheduler_snapshot_script(pid,birth)

class SchedulerCollectorLifecycleTests(unittest.TestCase):
    def fixture(self,root):
        proc=Path(root)/'proc';base=proc/'123';(base/'task').mkdir(parents=True)
        (proc/'uptime').write_text('10.0 0.0\n')
        for tid in ('123','124'):
            task=base/'task'/tid;task.mkdir()
            stat=snapshot().splitlines()[4].split('/stat ',1)[1].replace('123 (',tid+' (',1)
            (task/'stat').write_text(stat+'\n');(task/'schedstat').write_text('100 20 4\n');(task/'wchan').write_text('futex_wait_queue')
        (base/'stat').write_text((base/'task/123/stat').read_text())
        return proc,base
    def run_capture(self,proc):
        script=diagnostic.scheduler_snapshot_script('123','42').replace('/proc',str(proc))
        result=subprocess.run(['sh','-c',script],capture_output=True,text=True,timeout=10)
        return result,result.stdout.replace(str(proc),'/proc')
    def test_actual_shell_collects_all_three_fields_without_newlines(self):
        with tempfile.TemporaryDirectory() as root:
            proc,base=self.fixture(root);r,raw=self.run_capture(proc)
            self.assertEqual(r.returncode,0,r.stderr)
            self.assertEqual(len(diagnostic.parse_scheduler_snapshot(raw,'123','42')['threads']),2)
    def test_live_missing_file_is_not_hidden_by_pipeline_success(self):
        with tempfile.TemporaryDirectory() as root:
            proc,base=self.fixture(root);(base/'task/124/wchan').unlink();r,raw=self.run_capture(proc)
            self.assertNotEqual(r.returncode,0,'sampler masked a live unreadable thread with END')
            self.assertNotIn('END 123 42',raw)
    def transition(self,change):
        with tempfile.TemporaryDirectory() as root:
            proc,base=self.fixture(root);fifo=base/'task/124/wchan';fifo.unlink();os.mkfifo(fifo)
            errors=[]
            def writer():
                try:
                    with fifo.open('w') as stream:
                        change(base)
                        stream.write('futex_wait_queue')
                except Exception as error:errors.append(error)
            worker=threading.Thread(target=writer,daemon=True);worker.start()
            r,raw=self.run_capture(proc);worker.join(2);self.assertFalse(worker.is_alive());self.assertEqual(errors,[])
            return r,raw
    def test_confirmed_thread_retirement_preserves_remaining_complete_records(self):
        r,raw=self.transition(lambda base:shutil.rmtree(base/'task/124'))
        self.assertEqual(r.returncode,0,r.stderr)
        value=diagnostic.parse_scheduler_snapshot(raw,'123','42')
        self.assertEqual(set(value['threads']),{'123'});self.assertEqual(value['retired'],['124'])
        self.assertEqual(diagnostic.require_fair_scheduler(value)['threads'],1)
    def test_thread_reuse_or_policy_change_is_not_a_complete_fair_sample(self):
        for field,value in ((19,'43'),(38,'1')):
            def change(base):
                p=base/'task/124/stat';fields=p.read_text().rsplit(') ',1);tail=fields[1].split();tail[field]=value;p.write_text(fields[0]+') '+' '.join(tail)+'\n')
            r,raw=self.transition(change)
            if r.returncode==0:
                with self.subTest(field=field),self.assertRaises(ValueError):diagnostic.parse_scheduler_snapshot(raw,'123','42')
    def test_new_live_thread_in_final_census_requires_a_fresh_complete_capture(self):
        def change(base):
            task=base/'task/125';task.mkdir();(task/'stat').write_text((base/'task/124/stat').read_text().replace('124 (','125 (',1))
        r,raw=self.transition(change)
        if r.returncode==0:
            with self.assertRaises(ValueError):diagnostic.parse_scheduler_snapshot(raw,'123','42')
    def test_process_birth_change_stays_closed(self):
        def change(base):
            p=base/'stat';p.write_text(p.read_text().replace(' 42 0 0',' 43 0 0'))
        r,raw=self.transition(change);self.assertEqual(r.returncode,93);self.assertNotIn('END 123 42',raw)

    def test_actual_shell_enforces_byte_and_thread_bounds(self):
        with tempfile.TemporaryDirectory() as root:
            proc,base=self.fixture(root);(base/'task/124/schedstat').write_text('9'*diagnostic.BYTE_CAP+'\n')
            r,raw=self.run_capture(proc);self.assertNotEqual(r.returncode,0);self.assertNotIn('END 123 42',raw)
            self.assertLessEqual(len(raw.encode()),diagnostic.BYTE_CAP)
        with tempfile.TemporaryDirectory() as root:
            proc,base=self.fixture(root)
            for tid in range(1000,1511):
                task=base/'task'/str(tid);task.mkdir();(task/'stat').write_text('0')
            r,raw=self.run_capture(proc);self.assertEqual(r.returncode,94);self.assertEqual(raw,'')
    def test_schema2_cannot_claim_retirement_for_the_live_principal_or_omit_records(self):
        with tempfile.TemporaryDirectory() as root:
            proc,base=self.fixture(root);r,raw=self.run_capture(proc)
            for changed in (raw.replace('census 124\n','census 125\n'),raw.replace('enumerated 124\n',''),raw.replace('census 123\n',''),raw.replace('census 124\n','retired 124\ncensus 124\n')):
                with self.subTest(changed=changed[-100:]),self.assertRaises(ValueError):diagnostic.parse_scheduler_snapshot(changed,'123','42')

if __name__=='__main__':unittest.main()
