#!/usr/bin/env python3
"""Public API records only; no private native reads, VM or hardware."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import unittest
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
import sdk_emulator_public as public
from sdk_emulator_controls import MissingEvidence,StaleObservation

def line(seq,deck,query=10_000_000_000,**changes):
    row=dict(v=1,seq=seq,deck=deck,returned=deck,query_ns=query,rc=0,valid=768,mt=1,mt_unit=deck,mt_rev=1,mt_ns=9_000_000_000,
             sync=0,sync_unit=deck+10,sync_rev=1,sync_ns=9_000_000_000,raw=0)
    row.update(changes)
    return 'SDK_PUBLIC_STATE '+' '.join(k+'='+str(v) for k,v in row.items())

class PublicTests(unittest.TestCase):
    def test_approval_read_requires_exact_id_and_binary_hash_without_aliasing(self):
        sha='a'*64
        self.assertFalse(public.approved('other '+sha+'\n','sdk-state-probe',sha))
        self.assertFalse(public.approved('sdk-state-probe '+'b'*64+'\n','sdk-state-probe',sha))
        self.assertTrue(public.approved('sdk-state-probe '+sha+'\n','sdk-state-probe',sha))
        self.assertFalse(public.approved('sdk-state-probe '+sha+' extra\n','sdk-state-probe',sha))
    def test_normal_stage_plan_preserves_trust_records_and_existing_enabled_list(self):
        script=public.stage_script('a'*32)
        self.assertIn('test ! -e /home/root/settings/az-usbload/community/sdk-state-probe',script)
        self.assertIn('tar -xf -',script)
        self.assertIn('grep -qx sdk-state-probe',script)
        self.assertIn('>>/home/root/settings/az-usbload/community/enabled',script)
        self.assertNotIn('approved',script);self.assertNotIn('.origin',script)
        for token in ('bad','a'*32+'; id','../anything'):
            with self.assertRaises(ValueError):public.stage_script(token)
    def test_public_read_script_checks_native_birth_before_and_after_a_bounded_read(self):
        script=public.read_script('12','100','cat /tmp/azmod-community/sdk-state-probe.log',262144)
        self.assertEqual(script.count('test "$(check_public_native)" = AZSDK_NATIVE_BIRTH_MATCH'),2)
        self.assertLess(script.index('test "$(check_public_native)"'),script.index('cat /tmp/azmod-community'))
        self.assertIn('head -c 262145',script)
        self.assertNotIn('touch ',script);self.assertNotIn('kill ',script)
        with self.assertRaises(ValueError):public.read_script('12;id','100','cat /tmp/log',100)
    def test_reload_identity_requires_a_new_pid_and_birth_not_a_receiver_or_existing_process(self):
        self.assertTrue(public.new_process({'pid':'12','birth':'100'},{'pid':'13','birth':'200'}))
        for after in ({'pid':'12','birth':'100'},{'pid':'12','birth':'200'},{'pid':'13','birth':'100'},{'pid':'0','birth':'200'}):
            with self.subTest(after=after),self.assertRaises(ValueError):public.new_process({'pid':'12','birth':'100'},after)
    def test_public_acceptance_waits_for_real_allow_then_new_native_process_before_settings(self):
        calls=[]
        before={'status':'PASS','pid':'12','birth':'100','ui_allow':True,'ui_reload':True,'reload_request':'apply','approved_sha256':'a'*64}
        after={'status':'PASS','pid':'13','birth':'200','mapped_probe_sha256':'a'*64,
               'checks':{str(d):{'mt':'PASS','sync':'PASS'} for d in range(1,5)}}
        def phase(name):calls.append(name);return copy.deepcopy(before if name=='approval' else after)
        result=public.run_public_phases(phase,lambda native:calls.append(('restart',native['pid'])),'a'*64)
        self.assertEqual(result['status'],'PASS')
        self.assertEqual(calls,['approval',('restart','12'),'settings'])
        for mutation in ('unapproved','no-reload-request','same-pid','wrong-map','missing-deck'):
            b=copy.deepcopy(before);a=copy.deepcopy(after);calls.clear()
            if mutation=='unapproved':b['status']='INCOMPLETE';b['ui_allow']=False
            if mutation=='no-reload-request':b['ui_reload']=False;b['reload_request']=''
            if mutation=='same-pid':a['pid']='12'
            if mutation=='wrong-map':a['mapped_probe_sha256']='b'*64
            if mutation=='missing-deck':del a['checks']['4']
            result=public.run_public_phases(lambda name:b if name=='approval' else a,lambda native:calls.append('restart'),'a'*64)
            self.assertNotEqual(result['status'],'PASS',mutation)
            if mutation in ('unapproved','no-reload-request'):self.assertEqual(calls,[])
    def test_wave_only_lifecycle_refuses_settings_stimuli_or_wrong_process_mapping(self):
        before=dict(status='PASS',pid='12',birth='100',ui_allow=True,ui_reload=True,reload_request='apply',approved_sha256='a'*64)
        after=dict(status='PASS',pid='13',birth='200',mapped_probe_sha256='a'*64,settings_execution='NOT_RUN',events=[],checks={})
        result=public.run_public_phases(lambda name:before if name=='approval' else after,lambda identity:None,'a'*64,require_settings=False)
        self.assertEqual(result['status'],'PASS');self.assertEqual(result['settings_execution'],'NOT_RUN')
        for change in (dict(events=[dict(action='sync',deck=1)]),dict(settings_execution='REQUESTED'),dict(pid='12'),dict(mapped_probe_sha256='b'*64)):
            value=dict(after,**change)
            result=public.run_public_phases(lambda name:before if name=='approval' else value,lambda identity:None,'a'*64,require_settings=False)
            self.assertEqual(result['status'],'FAIL',change)

    def test_probe_archive_keeps_exact_bound_normal_loader_files_and_refuses_extra_or_changed_payload(self):
        def archive(extra=False):
            stream=io.BytesIO()
            with zipfile.ZipFile(stream,'w') as z:
                z.writestr('azmod.json',json.dumps(dict(id='sdk-state-probe',version='0.1.0',api=1,hooks=False,firmware=['any'])))
                z.writestr('sdk-state-probe.so',b'bound binary');z.writestr('LICENSE',b'GPL');z.writestr('icon.png',b'icon')
                if extra:z.writestr('../approved',b'forbidden')
            return stream.getvalue()
        data=archive();binding=dict(package={'sha256':hashlib.sha256(data).hexdigest()},binary={'sha256':hashlib.sha256(b'bound binary').hexdigest()})
        files=public.probe_archive(data,binding)
        self.assertEqual(set(files),{'azmod.json','sdk-state-probe.so','LICENSE','icon.png'})
        with self.assertRaises(ValueError):public.probe_archive(data+b'changed',binding)
        extra=archive(True);binding['package']['sha256']=hashlib.sha256(extra).hexdigest()
        with self.assertRaises(ValueError):public.probe_archive(extra,binding)
    def test_raw_slip_package_keeps_exact_hook_tags_id_and_binary(self):
        for change in ({},{'hooks':False},{'firmware':['any']},{'id':'other'}):
            metadata=dict(id='sdk-slip-capture',version='0.1.0',api=1,hooks=True,firmware=list(public.SLIP_FIRMWARE));metadata.update(change)
            stream=io.BytesIO()
            with zipfile.ZipFile(stream,'w') as z:
                for name,data in {'azmod.json':json.dumps(metadata),'sdk-slip-capture.so':'binary','LICENSE':'GPL','icon.png':'icon'}.items():z.writestr(name,data)
            data=stream.getvalue();binding=dict(package={'sha256':hashlib.sha256(data).hexdigest()},binary={'sha256':hashlib.sha256(b'binary').hexdigest()})
            if change:
                with self.subTest(change=change),self.assertRaises(ValueError):public.probe_archive(data,binding,mod_id='sdk-slip-capture')
            else:self.assertIn('sdk-slip-capture.so',public.probe_archive(data,binding,mod_id='sdk-slip-capture'))
        plan=public.stage_script('a'*32,'sdk-slip-capture')
        self.assertIn('grep -qx sdk-slip-capture',plan);self.assertNotIn('sdk-state-probe',plan)
        self.assertNotIn('approved',plan)

    def setting_case(self,publish=True):
        class Controller:
            timeout=1;interval=.1
            def __init__(self):self.now=20.;self.inputs=[];self.rows={d:dict(v=1,seq=d,deck=d,returned=d,query_ns=20_000_000_000,rc=0,valid=0,
                mt=-1,mt_unit=0,mt_rev=0,mt_ns=0,sync=-1,sync_unit=0,sync_rev=0,sync_ns=0,raw=-1) for d in range(1,5)}
            def _now(self):return self.now
            def sleep(self,seconds):self.now+=seconds
            def button(self,deck,name):
                self.inputs.append((deck,name));self.now+=.01
                if publish:
                    stream='mt' if name=='mastertempo' else 'sync';r=self.rows[deck]
                    r[stream]=0 if r[stream]==1 else 1;r[stream+'_unit']=deck+(0 if stream=='mt' else 10)
                    r[stream+'_rev']+=1;r[stream+'_ns']=int(self.now*1e9)
                    r['valid']|=256 if stream=='mt' else 512
                    if stream=='sync':r['raw']=2 if r[stream] else 0
            def read(self):
                self.now+=.01;ns=int(self.now*1e9)
                for r in self.rows.values():r['query_ns']=ns
                return copy.deepcopy(self.rows),ns
        controller=Controller();checks=public.PublicSettingChecks(controller,controller.read)
        return controller,checks
    def test_genuine_setup_is_separate_then_two_known_state_inputs_roundtrip_each_public_setting(self):
        c,checks=self.setting_case()
        for deck in range(1,5):
            for stream in ('mt','sync'):
                setup=checks.establish(deck,stream)
                self.assertIsNone(setup['initial_value']);self.assertTrue(setup['input_sent'])
                result=checks.roundtrip(deck,stream)
                self.assertEqual([r[stream+'_rev'] for r in result['records']],[1,2,3])
                self.assertEqual(result['records'][0][stream],result['records'][2][stream])
        self.assertEqual(len(c.inputs),24)
        self.assertEqual(set(name for _,name in c.inputs),{'mastertempo','sync'})
    def test_failed_setup_sends_once_and_never_retries_or_guesses_a_state(self):
        c,checks=self.setting_case(False)
        with self.assertRaises(MissingEvidence):checks.establish(1,'mt')
        with self.assertRaisesRegex(MissingEvidence,'already attempted'):checks.establish(1,'mt')
        self.assertEqual(c.inputs,[(1,'mastertempo')])
    def test_known_state_roundtrip_refuses_before_input_without_public_validity(self):
        c,checks=self.setting_case(False)
        with self.assertRaises(MissingEvidence):checks.roundtrip(1,'sync')
        self.assertEqual(c.inputs,[])
    def test_four_deck_public_rows_validate_fresh_query_and_separate_setting_clocks(self):
        text='loaded 0.1.0 on ep147 (0 hooks)\n'+'\n'.join(line(d,d) for d in range(1,5))+'\n'
        rows=public.public_rows(text,10_500_000_000)
        self.assertEqual(set(rows),{1,2,3,4})
        self.assertEqual(public.setting(rows[3],'mt'),dict(value=1,unit=3,revision=1,received_ns=9_000_000_000))
        self.assertEqual(public.setting(rows[3],'sync')['unit'],13)
        # Old receipt time describes a retained setting; current API reads
        # must remain fresh. This supplies no transport/paused semantics.
        with self.assertRaises(StaleObservation):public.public_rows(text,16_000_000_000)
    def test_malformed_future_duplicate_and_wrong_public_deck_rows_refuse(self):
        base='\n'.join(line(d,d) for d in range(1,5))
        for text in (base+'\n'+line(4,4),base.replace('returned=2','returned=3'),base.replace('v=1','v=2'),
                     base.replace('mt=1','mt=2'),base.replace('raw=0','raw=2'),base.replace('mt_ns=9000000000','mt_ns=11000000000'),
                     base.replace('query_ns=10000000000','query_ns=11000000000'),base.replace('seq=1','seq=1 seq=1')):
            with self.subTest(text=text[-70:]),self.assertRaises(ValueError):public.public_rows(text,10_500_000_000)
    def test_unknown_public_settings_are_missing_evidence_and_never_false_defaults(self):
        text='\n'.join(line(d,d,valid=0,mt=-1,sync=-1,mt_unit=0,mt_rev=0,mt_ns=0,sync_unit=0,sync_rev=0,sync_ns=0,raw=-1) for d in range(1,5))
        rows=public.public_rows(text,10_500_000_000)
        for stream in ('mt','sync'):
            with self.assertRaises(MissingEvidence):public.setting(rows[1],stream)
    def test_transition_needs_same_unit_new_revision_post_input_native_time_and_expected_value(self):
        before=public.public_rows('\n'.join(line(d,d) for d in range(1,5)),10_500_000_000)[1]
        after=dict(before,seq=5,query_ns=12_000_000_000,mt=0,mt_rev=2,mt_ns=11_000_000_000)
        self.assertTrue(public.transition(before,after,'mt',10_500_000_000,0))
        for changes in (dict(mt_unit=99),dict(mt_rev=1),dict(mt_ns=10_000_000_000),dict(mt=1),dict(deck=2,returned=2)):
            value=dict(after,**changes)
            with self.subTest(changes=changes):
                try:result=public.transition(before,value,'mt',10_500_000_000,0)
                except ValueError:result=False
                self.assertFalse(result)

if __name__=='__main__':unittest.main()
