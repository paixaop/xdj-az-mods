"""Actual read-only executor for the source-owned raw Slip array."""
from pathlib import Path
import copy
import hashlib
import importlib.util
import json
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
import sdk_emulator_slip_source as source
if os.environ.get('AZ_SLIP_READER'):
    _spec=importlib.util.spec_from_file_location('retained_slip_reader',os.environ['AZ_SLIP_READER'])
    source=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(source)
from sdk_emulator_controls import MissingEvidence

NOW=20_000_000_000
CORE=Path(os.environ.get('AZ_SLIP_TEST_CORE',str(Path(__file__).resolve().parents[3]/'anchor/build/native-slip-source-batch/src/out')))
ANCHOR=Path(__file__).resolve().parents[3]/'anchor'
RETAINED_MAPS=ANCHOR/'output/sdk-emulator-goal-20261028/emulator-200-slip-getters1/controlled-runtime/loaded/getter-failed-maps.txt'


def rows(valid=True):
    return [dict(sequence=4,generation=d+1,revision=1 if valid else 0,
                 observed_ns=NOW-1000 if valid else 0,callbacks=1 if valid else 0,
                 registered=1,valid=int(valid),raw=0xffffffff if valid else 0,reserved=0)
            for d in range(4)]


def packed(values):
    return b''.join(struct.pack('<5Q4I',*(r[k] for k in source.FIELDS)) for r in values)


def getter_rows():
    return [dict(sequence=4,generation=d+1,observed_ns=NOW-1000,
                 target_slot4=0x480000,target_slot13=0x480040,target_slot14=0x480080,
                 seen_mask=7,conflict_mask=0) for d in range(4)]


def packed_getters(values):
    fields=('sequence','generation','observed_ns','target_slot4','target_slot13','target_slot14','seen_mask','conflict_mask')
    return b''.join(struct.pack('<6Q2I',*(r[k] for k in fields)) for r in values)


class SourceTests(unittest.TestCase):
    @unittest.skipUnless((CORE/'azmod.so.full').is_file(),'frozen source-owned Slip artifact is not present')
    def test_exact_ready_artifact_derives_object_and_collector_without_numeric_binding(self):
        proof=source.binding(CORE)
        self.assertEqual(proof['build_id'],os.environ.get('AZ_SLIP_TEST_BUILD_ID','0666d22e4ed2d4ad80704bc2729a63b1ff26361b'))
        self.assertEqual(proof['symbol']['name'],'azsdk_slip_observation_v1')
        self.assertEqual(proof['symbol']['size'],224)
        self.assertNotEqual(proof['symbol']['rva'],proof['collector']['rva'])
        self.assertEqual(proof['collector']['name'],'azsdk_slip_collect_v1')
        with tempfile.TemporaryDirectory() as t:
            other=Path(t)
            (other/'azmod.so.full').write_bytes((CORE/'azmod.so.full').read_bytes())
            (other/'azmod.so').write_bytes((CORE/'azmod.so').read_bytes())
            data=bytearray((other/'azmod.so').read_bytes());needle=source.binding(other)['collector']
            from sdk_probe_binding import Elf
            elf=Elf(bytes(data));program=next(p for p in elf.programs if p[0]==1 and p[3]<=needle['rva']<p[3]+p[5])
            data[program[2]+needle['rva']-program[3]]^=1;(other/'azmod.so').write_bytes(data)
            with self.assertRaisesRegex(ValueError,'collector'):source.binding(other)

    def test_registered_without_callback_never_supplies_initial_zero_semantics(self):
        value=source.decode(packed(rows(False)),packed(rows(False)),NOW)
        self.assertEqual([r['registered'] for r in value],[1]*4)
        self.assertEqual([r['valid'] for r in value],[0]*4)
        self.assertTrue(all('enabled' not in r for r in value))
        good=source.decode(packed(rows()),packed(rows()),NOW)
        self.assertEqual([r['raw'] for r in good],[2**32-1]*4)

    def test_torn_odd_short_future_invalid_and_aliased_rows_refuse(self):
        for change in (dict(sequence=5),dict(sequence=0),dict(reserved=1),dict(registered=2),dict(valid=2),
                       dict(generation=0),dict(generation=2),dict(revision=0),dict(callbacks=0),dict(observed_ns=NOW+1),
                       dict(registered=0),dict(valid=0)):
            bad=rows();bad[0].update(change)
            with self.subTest(change=change),self.assertRaises((ValueError,MissingEvidence)):
                source.decode(packed(bad),packed(bad),NOW)
        with self.assertRaises(MissingEvidence):source.decode(packed(rows())[:-1],packed(rows()),NOW)
        bad=rows();bad[0]['sequence']+=2
        with self.assertRaises(MissingEvidence):source.decode(packed(rows()),packed(bad),NOW)

    def capture_case(self,mutation='none',previous=None,values=None,getters=None,hooks=7):
        proof=dict(build_id='b'*40,sha256='a'*64,symbol=dict(name='azsdk_slip_observation_v1',rva=0x1200,size=224),
                   collector=dict(name='azsdk_slip_collect_v1',rva=0x80,size=16,code_sha256='c'*64))
        path='/tmp/az-sdk-live/azmod.so'
        mapping='40000-41000 r-xp 00000000 00:01 1 '+path+'\n41000-42000 rw-p 00000000 00:00 0\n'
        if mutation=='sysv-deleted':mapping+='ffffa92a6000-ffffa92a7000 rw-s 00000000 00:01 0                          /SYSV1402004f (deleted)\n'
        if mutation=='deleted-core':mapping=mapping.replace(path,path+' (deleted)')
        memory=tempfile.TemporaryDirectory();self.addCleanup(memory.cleanup)
        file=Path(memory.name)/'memory'
        with file.open('wb') as out:
            out.seek(0x41200);out.write(packed(rows(False) if values is None else values))
            if getters is not None:
                proof.update(getter_symbol=dict(name='azsdk_slip_getter_observation_v1',rva=0x1300,size=224),
                             getter_hooks=dict(name='azsdk_slip_getter_hooks_v1',rva=0x13e0,size=4))
                out.seek(0x41300);out.write(packed_getters(getters));out.write(struct.pack('<I',hooks))
        calls=[];clock=[10.0];alive=[True]
        class Controller:
            def _owner(self):
                if mutation=='owner':raise ValueError('owner changed')
            def _now(self):return clock[0]
        def verify(pid,birth):
            self.assertEqual((pid,birth),('123','456'))
            return alive[0]
        def read(body,cap):
            calls.append(body);self.assertLessEqual(cap,524288)
            self.assertNotIn('call ',body);self.assertNotIn('kill ',body)
            if mutation=='expiry':clock[0]=100
            if mutation=='pid':alive[0]=False
            if body=='cat /proc/123/maps':
                changed=mutation=='mapping' or (mutation=='late-map' and calls.count(body)>1)
                return mapping.replace('41000-42000 rw-p','41000-42000 r--p') if changed else mapping
            if body=='sha256sum '+path:return ('d'*64 if mutation=='hash' else 'a'*64)+'  '+path+'\n'
            if mutation=='short':return '0000000 0000\n'
            # Execute the actual generated memory-read shell, with the exact
            # legacy BusyBox option set (no od -A/-t) against a fixture memfile.
            shell='od() { for arg in "$@"; do case "$arg" in -A*|-t*) return 2;; esac; done; command od "$@"; };\n'+body.replace('/proc/123/mem',str(file))
            return subprocess.run(['sh','-c',shell],universal_newlines=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=2).stdout
        result=source.capture(Controller(),read,'123','456',proof,lambda:NOW,verify,20,previous=previous)
        return result,calls

    def test_actual_executor_reads_anonymous_bss_and_checks_process_module_and_bounds(self):
        result,calls=self.capture_case()
        self.assertEqual(result['pid'],'123');self.assertEqual(result['birth'],'456')
        self.assertEqual(len(result['rows']),4)
        self.assertEqual(result['scope'],'source-owned raw Slip observations; no enabled-state semantics')
        self.assertTrue(any('if=/proc/123/mem' in call for call in calls))
        self.assertFalse(any('sdk-slip-capture' in call for call in calls))
        memory_reads=[call for call in calls if 'if=/proc/123/mem' in call]
        self.assertEqual(len(memory_reads),1,'both seqlock samples must use one bounded remote call')
        self.assertEqual(memory_reads[0].count('dd if=/proc/123/mem'),2)

    def test_actual_executor_changed_pid_module_maps_or_expired_budget_refuses(self):
        for mutation in ('owner','pid','hash','mapping','late-map','short','expiry','deleted-core'):
            with self.subTest(mutation=mutation),self.assertRaises((ValueError,MissingEvidence)):
                self.capture_case(mutation)

    def test_actual_executor_accepts_retained_unrelated_sysv_deleted_mapping(self):
        value,_=self.capture_case('sysv-deleted')
        self.assertEqual(value['pid'],'123');self.assertEqual(len(value['rows']),4)

    def retained_case(self,mutation='none',exercise=None,timed=False):
        # The full unedited guest maps, real full/stripped symbols/BSS and exact
        # firmware bytes are used. Only source/target row values are fixtures;
        # these addresses are NOT observed runtime getter results.
        original_maps=RETAINED_MAPS.read_text()
        proof=source.binding(ANCHOR/'build/native-slip-getter-batch/src/out',getters=True)
        lines=[line.split(maxsplit=5) for line in original_maps.splitlines()]
        core_rows=[f for f in lines if len(f)==6 and f[-1].endswith('/azmod.so')]
        core_path=core_rows[0][-1]
        base=next(int(f[0].split('-')[0],16)-int(f[2],16) for f in core_rows if f[1]=='r-xp')
        native_path=next(f[-1] for f in lines if len(f)==6 and f[-1].endswith('/EP147') and f[1]=='r-xp')
        spec=importlib.util.spec_from_file_location('retained_exact_image',Path(__file__).resolve().parents[2]/'tools/ep147-addrgen.py')
        tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
        image=tool.Image(ANCHOR/'firmware/EP147-2.00',json.loads((ANCHOR/'build/emulator-runtime-slip-getter-inputs/tools/ep147-addresses.json').read_text())['builds']['2.00'])
        source_rows=rows(False);getters=getter_rows()
        for row in getters:row.update(target_slot4=0x2126ba8,target_slot13=0x2126bc8,target_slot14=0x2126be8)
        if mutation=='replaced-generation':getters[0]['generation']+=10
        memory={base+proof['symbol']['rva']:packed(source_rows),base+proof['getter_symbol']['rva']:packed_getters(getters),
                base+proof['getter_hooks']['rva']:struct.pack('<I',7)}
        for address in (0x2126ba8,0x2126bc8,0x2126be8):memory[address]=image.code(address,address+16)
        calls=[];alive=[True];clock=[10.0]
        class Controller:
            interval=.2
            events=[]
            def _owner(self):
                if timed:clock[0]+=.046
            def _now(self):return clock[0]
            def sleep(self,seconds):clock[0]+=seconds
        with tempfile.TemporaryDirectory() as t:
            clauses=[];memory_paths={}
            for index,(address,value) in enumerate(memory.items()):
                path=Path(t)/('mem'+str(index));path.write_bytes(value)
                memory_paths[address]=path
                selector='if=/proc/7585/mem bs=1 skip='+str(address)+' count='+str(len(value))
                clauses.append(shlex.quote(selector)+') command dd if='+shlex.quote(str(path))+' bs=1 count='+str(len(value))+';;')
            shell_prefix='dd() { case "$*" in '+'\n'.join(clauses)+'\n*) return 97;; esac; };\n'+\
                'od() { for arg in "$@"; do case "$arg" in -A*|-t*) return 2;; esac; done; command od "$@"; };\n'
            def read(body,cap):
                calls.append(body)
                if timed:clock[0]+=2.885 if 'AZSLIP_' in body else 1.94 if '/maps' in body else 2.36
                if mutation=='identity':alive[0]=False
                if body=='cat /proc/7585/maps':
                    if mutation=='final-map' and calls.count(body)>=3:return original_maps.replace(core_path,core_path+' (deleted)')
                    return original_maps.replace(core_path,core_path+' (deleted)') if mutation=='deleted-core' else original_maps
                if body=='sha256sum '+core_path:
                    return ('d'*64 if mutation=='final-hash' and calls.count(body)>1 else proof['sha256'])+'  '+core_path+'\n'
                if body=='sha256sum '+native_path:return hashlib.sha256(image.data).hexdigest()+'  '+native_path+'\n'
                self.assertIn('if=/proc/7585/mem',body)
                result=subprocess.run(['sh','-c',shell_prefix+body],universal_newlines=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=3).stdout
                if exercise is not None:
                    for name,values,pack in (('symbol',source_rows,packed),('getter_symbol',getters,packed_getters)):
                        for value in values:value['sequence']+=2
                        memory_paths[base+proof[name]['rva']].write_bytes(pack(values))
                if mutation=='final-identity' and 'AZSLIP_0_code' in body:alive[0]=False
                if mutation=='expire-memory' and 'AZSLIP_' in body:clock[0]=1000
                return result
            def verify(pid,birth):
                if timed:clock[0]+=1.2
                return alive[0] and (pid,birth)==('7585','58532')
            if exercise is not None:
                return exercise(Controller(),read,proof,image,native_path,verify,clock)
            observed=source.capture(Controller(),read,'7585','58532',proof,lambda:NOW,verify,20)
            self.assertTrue(all(r['registered']==1 and r['valid']==0 and r['callbacks']==0 for r in observed['rows']))
            result=source.identify_targets(Controller(),read,observed,{native_path:image},verify,20)
            return observed,result,calls

    @unittest.skipUnless(RETAINED_MAPS.is_file(),'retained guest maps are not present')
    def test_entire_retained_maps_actual_source_bss_and_code_executor_without_raw_callback(self):
        observed,result,calls=self.retained_case()
        self.assertEqual(len(observed['rows']),4)
        self.assertEqual(len(result['targets']),3)
        self.assertTrue(all(r['status']=='BOUND' for r in result['targets']))
        self.assertEqual(sum('if=/proc/7585/mem' in call for call in calls),2)

    @unittest.skipUnless(RETAINED_MAPS.is_file(),'retained guest maps are not present')
    def test_retained_full_input_still_refuses_deleted_core_changed_identity_or_generation(self):
        for mutation in ('deleted-core','identity','replaced-generation'):
            with self.subTest(mutation=mutation),self.assertRaises((ValueError,MissingEvidence)):self.retained_case(mutation)

    def test_actual_executor_requires_a_new_collection_and_per_deck_source_continuity(self):
        before=self.capture_case()[0]
        with self.assertRaises(MissingEvidence):self.capture_case(previous=before)
        values=rows(False)
        for row in values:row['sequence']+=2
        after=self.capture_case(previous=before,values=values)[0]
        self.assertEqual(source.registered_sources(before,after),(1,2,3,4))
        for change in (dict(registered=0,generation=0),dict(generation=5)):
            bad=copy.deepcopy(after);bad['rows'][0].update(change)
            with self.subTest(change=change),self.assertRaises(MissingEvidence):source.registered_sources(before,bad)

    def test_retirement_and_invalid_callback_are_unknown_not_counter_corruption(self):
        before=self.capture_case(values=rows())[0]
        invalid=rows(False)
        for row in invalid:row.update(sequence=6,callbacks=1)
        result=self.capture_case(previous=before,values=invalid)[0]
        self.assertEqual([r['valid'] for r in result['rows']],[0]*4)
        for row in invalid:row.update(generation=0,registered=0,callbacks=0)
        result=self.capture_case(previous=before,values=invalid)[0]
        with self.assertRaises(MissingEvidence):source.registered_sources(before,result)

    def test_fresh_callback_needs_same_current_generation_and_post_stimulus_receipt(self):
        before=self.capture_case()[0];before['native_now_ns']=NOW-4000;after=copy.deepcopy(before);after['native_now_ns']=NOW
        after['rows']=source.decode(packed(rows()),packed(rows()),NOW)
        after['rows'][0]['sequence']=6
        self.assertEqual(source.fresh_callback(before,after,1,NOW-2000)['raw'],2**32-1)
        for change in (dict(generation=5),dict(sequence=4),dict(registered=0),dict(valid=0),dict(observed_ns=NOW-3000),dict(callbacks=0)):
            bad=copy.deepcopy(after);bad['rows'][0].update(change)
            with self.subTest(change=change),self.assertRaises((ValueError,MissingEvidence)):
                source.fresh_callback(before,bad,1,NOW-2000)
        bad=copy.deepcopy(after);bad['birth']='789'
        with self.assertRaises(ValueError):source.fresh_callback(before,bad,1,NOW-2000)

    def test_actual_getter_executor_requires_all_three_checked_sites_and_current_source(self):
        value,calls=self.capture_case(getters=getter_rows())
        self.assertEqual(source.getter_sites(value,1),{4:0x480000,13:0x480040,14:0x480080})
        self.assertEqual(sum('if=/proc/123/mem' in call for call in calls),1)
        self.assertTrue(all('enabled' not in row for row in value['getters']))
        for mask in (0,1,3):
            with self.subTest(mask=mask),self.assertRaises(MissingEvidence):self.capture_case(getters=getter_rows(),hooks=mask)
        changed=getter_rows();changed[0]['generation']=5
        with self.assertRaises(MissingEvidence):self.capture_case(getters=changed)

    def test_unseen_and_conflicted_getter_sites_are_not_code_identity(self):
        values=getter_rows();values[0].update(conflict_mask=1)
        result=self.capture_case(getters=values)[0]
        self.assertEqual(source.getter_sites(result,1),{13:0x480040,14:0x480080})
        for row in values:row.update(seen_mask=0,conflict_mask=0,observed_ns=0,target_slot4=0,target_slot13=0,target_slot14=0)
        result=self.capture_case(getters=values)[0]
        self.assertEqual(source.getter_sites(result,1),{})

    def test_getter_decoder_refuses_torn_future_bad_masks_and_unknown_source(self):
        for change in (dict(sequence=3),dict(observed_ns=NOW+1),dict(seen_mask=8),dict(conflict_mask=8),
                       dict(seen_mask=1),dict(target_slot4=0),dict(target_slot13=0x480041)):
            bad=getter_rows();bad[0].update(change)
            with self.subTest(change=change),self.assertRaises((ValueError,MissingEvidence)):
                source.decode_getters(packed_getters(bad),packed_getters(bad),rows(False),NOW)
        with self.assertRaises(MissingEvidence):source.decode_getters(packed_getters(getter_rows()),b'',rows(False),NOW)

    def identify_case(self,mutation='none'):
        observation=self.capture_case(getters=getter_rows())[0]
        for row in observation['getters']:
            row.update(target_slot4=0x400080,target_slot13=0x4000a0,target_slot14=0x4000c0)
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);path=root/'EP147'
            data=bytearray(256);data[:7]=b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HHIQQQIHHHHHH',data,16,2,183,1,0x400080,64,0,0,64,56,1,0,0,0)
            struct.pack_into('<IIQQQQQQ',data,64,1,5,0,0x400000,0x400000,256,256,4096)
            data[128:]=struct.pack('<I',0xd503201f)*32;path.write_bytes(data)
            spec=importlib.util.spec_from_file_location('slip_exact_image',Path(__file__).resolve().parents[2]/'tools/ep147-addrgen.py')
            tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool)
            image=tool.Image(path,dict(size=len(data),sha256=hashlib.sha256(data).hexdigest(),md5=hashlib.md5(data).hexdigest()))
            native='/opt/native/EP147' if mutation!='unknown-library' else '/usr/lib/libnative.so'
            mapping='40000-41000 r-xp 00000000 00:01 1 '+observation['module_path']+'\n41000-42000 rw-p 00000000 00:00 0\n400000-401000 r-xp 00000000 00:01 2 '+native+'\n'
            memory=root/'memory'
            with memory.open('wb') as out:out.seek(0x400000);out.write(data)
            calls=[];alive=[True];clock=[10]
            class Controller:
                def _owner(self):pass
                def _now(self):return clock[0]
            def read(body,cap):
                calls.append(body)
                if mutation=='pid':alive[0]=False
                if mutation=='expiry':clock[0]=100
                if body=='cat /proc/123/maps':
                    if mutation=='late-map' and calls.count(body)>1:return mapping.replace('400000-401000 r-xp','400000-401000 rw-p')
                    return mapping
                if body=='sha256sum '+observation['module_path']:return 'a'*64+'  '+observation['module_path']+'\n'
                if body=='sha256sum '+native:return ('d'*64 if mutation=='hash' else hashlib.sha256(data).hexdigest())+'  '+native+'\n'
                if mutation=='code':
                    with memory.open('r+b') as out:out.seek(0x400080);out.write(b'bad!')
                shell='od() { for arg in "$@"; do case "$arg" in -A*|-t*) return 2;; esac; done; command od "$@"; };\n'+body.replace('/proc/123/mem',str(memory))
                return subprocess.run(['sh','-c',shell],universal_newlines=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=2).stdout
            images={} if mutation=='unknown-library' else {native:image}
            value=source.identify_targets(Controller(),read,observation,images,lambda pid,birth:alive[0],20)
            return value,calls

    def test_actual_code_identity_executor_binds_entry_words_without_invocation(self):
        value,calls=self.identify_case()
        self.assertEqual(len(value['targets']),3)
        self.assertTrue(all(row['status']=='BOUND' for row in value['targets']))
        self.assertTrue(all(row['load_bias']==0 for row in value['targets']))
        self.assertTrue(all('enabled' not in row for row in value['targets']))
        self.assertTrue(all(not any(term in call for term in ('gdb','dlopen','call ')) for call in calls))

    def test_unknown_library_target_preserves_address_map_and_words_as_unresolved(self):
        value,_=self.identify_case('unknown-library')
        self.assertTrue(all(row['status']=='UNRESOLVED' and row['target'] and row['words'] and row['mapped_sha256'] for row in value['targets']))
        self.assertTrue(all(row['mapping'][-1]=='/usr/lib/libnative.so' for row in value['targets']))

    def test_code_identity_executor_rejects_changed_pid_expiry_image_code_and_maps(self):
        for mutation in ('pid','expiry','hash','code','late-map'):
            with self.subTest(mutation=mutation),self.assertRaises((ValueError,MissingEvidence)):self.identify_case(mutation)


if __name__=='__main__':unittest.main()
