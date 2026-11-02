#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""No target access: qualify the emulator runtime package and evidence decisions."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/integration'))
spec=importlib.util.spec_from_file_location('runtime_sdk',ROOT/'tests/integration/sdk_emulator_sdk.py')
runtime=importlib.util.module_from_spec(spec);spec.loader.exec_module(runtime)

def sha(data): return hashlib.sha256(data).hexdigest()

def elf(build_id):
    note=struct.pack('<III',4,len(build_id),3)+b'GNU\0'+build_id
    note+=b'\0'*((-len(note))%4)
    header=bytearray(64)
    header[:16]=b'\x7fELF\x02\x01\x01'+b'\0'*9
    struct.pack_into('<HHIQQQIHHHHHH',header,16,3,183,1,0,64,0,0,64,56,1,0,0,0)
    return bytes(header)+struct.pack('<IIQQQQQQ',4,4,120,0,0,len(note),len(note),4)+note

def binding_elf(build_id, functions=(), full=False, binding=None, code=b'\xc0\x03\x5f\xd6'):
    """Small real ELF layout; separate RX ranges retain exact historical RVAs."""
    note=struct.pack('<III',4,len(build_id),3)+b'GNU\0'+build_id
    note+=b'\0'*((-len(note))%4)
    count=1+len(functions)
    data=bytearray(64+56*count);programs=[];sections=[(0,0,0,0,0,0,0,0,0,0)]
    offset=len(data);data.extend(note);programs.append((4,4,offset,0,0,len(note),len(note),4))
    for name,rva,info in functions:
        offset=len(data);data.extend(code);programs.append((1,5,offset,rva,rva,len(code),len(code),4))
    names=bytearray(b'\0');section_names={}
    def section(name,kind,payload,flags=0,link=0,entsize=0):
        section_names[name]=len(names);names.extend(name.encode()+b'\0')
        offset=len(data);data.extend(payload)
        sections.append((section_names[name],kind,flags,0,offset,len(payload),link,0,1,entsize))
    def symbols(label,kind,selected):
        strings=bytearray(b'\0');rows=bytearray(24)
        for name,rva,info in selected:
            start=len(strings);strings.extend(name.encode()+b'\0')
            rows.extend(struct.pack('<IBBHQQ',start,info,0,1,rva,len(code)))
        section(label+'str',3,strings);link=len(sections)-1
        section(label+'sym',kind,rows,link=link,entsize=24)
    symbols('.dyn',11,[row for row in functions if row[2]>>4 in (1,2)])
    if full:symbols('.',2,functions)
    if binding:
        delta,provider,anchor,target=binding
        packed=struct.pack('<8sIIq129s64s64s',b'AZSDKPB1',281,1,delta,provider.encode(),anchor.encode(),target.encode())
        section('.azsdk.binding',1,packed,flags=2)
    section_names['.shstrtab']=len(names);names.extend(b'.shstrtab\0')
    offset=len(data);data.extend(names);shstr=len(sections)
    sections.append((section_names['.shstrtab'],3,0,0,offset,len(names),0,0,1,0))
    shoff=len(data)
    for row in sections:data.extend(struct.pack('<IIQQQQIIQQ',*row))
    data[:16]=b'\x7fELF\x02\x01\x01'+b'\0'*9
    struct.pack_into('<HHIQQQIHHHHHH',data,16,3,183,1,0,64,shoff,0,64,56,count,64,len(sections),shstr)
    for i,row in enumerate(programs):struct.pack_into('<IIQQQQQQ',data,64+56*i,*row)
    return bytes(data)

PROVIDERS={
    'source':('azmod.so','05de88b41b03476ea51b884bd257cc8b03fe4b5a','azmod_dm_key','azsdk_sources_candidate',522360,762720),
    'loop':('azstem.so','22'*20,'azstem_loop_observation_v2','azsdk_loop_read',92936,136296)}

def bound_package(root, source_delta=240360):
    out,source,manifest=legacy_package(root)
    manifest['schema']=2
    manifest['binding_proofs']={}
    for family,(library,identity,anchor,target,arva,trva) in PROVIDERS.items():
        functions=[(anchor,arva,34),(target,trva,2)]
        if family=='source':functions.append(('azsdk_sources_read',762360,2))
        for full in (False,True):
            (out/(library+('.full' if full else ''))).write_bytes(binding_elf(bytes.fromhex(identity),functions,full))
        delta=source_delta if family=='source' else trva-arva
        probe=family+'-probe.so'
        (out/probe).write_bytes(binding_elf(bytes.fromhex(('44' if family=='source' else '33')*20),binding=(delta,identity,anchor,target)))
        manifest['bindings'][family]={'build_id':identity,'delta':delta}
        manifest['binding_proofs'][family]={
            'build_id':identity,'delta':trva-arva,'full_sha256':sha((out/(library+'.full')).read_bytes()),
            'stripped_sha256':sha((out/library).read_bytes()),'probe_sha256':sha((out/probe).read_bytes()),
            'anchor':{'name':anchor,'rva':arva,'size':4,'code_sha256':sha(b'\xc0\x03\x5f\xd6')},
            'target':{'name':target,'rva':trva,'size':4,'code_sha256':sha(b'\xc0\x03\x5f\xd6')}}
    manifest['sha256']={name:sha((out/name).read_bytes()) for name in runtime.FILES}
    (out/'runtime.json').write_text(json.dumps(manifest))
    return out,source,manifest

def legacy_package(root):
    source=root/'source';source.mkdir();(source/'input.c').write_text('source')
    out=root/'package';out.mkdir()
    ids={'azmod.so':'11'*20,'azstem.so':'22'*20,'loop-probe.so':'33'*20,'source-probe.so':'44'*20}
    hashes={}
    for name,identity in ids.items():
        data=elf(bytes.fromhex(identity));(out/name).write_bytes(data);hashes[name]=sha(data)
    manifest={'schema':1,'sha256':hashes,'source_sha256':{'input.c':sha(b'source')},
              'bindings':{'loop':{'build_id':ids['azstem.so'],'delta':43360},
                          'source':{'build_id':ids['azmod.so'],'delta':240000}},
              'loop_operator':True}
    (out/'runtime.json').write_text(json.dumps(manifest))
    return out,source,manifest

def package(root):return bound_package(root)

def evidence(manifest):
    rows=['operator_mode=1 stop_file=/tmp/az-sdk-loop-probe.123.100.stop',
          'pid=123 exports=1,1','live_exports=1,1','private_load_export=1','diagnostic_available=1',
          'registration_mask=15 tick=10','live_registration_mask=15 tick=10']
    for deck in range(4):
        rows.append('deck=%d unit=%d revision=1 received_ns=100 engaged=0 bounds=0 in_s=0 out_s=0 mask=15 diagnostic_reason=1'%(deck,deck+1))
        for stream in ['play','quantize','key_lock']:
            rows.append('live_deck=%d unit=%d stream=%s revision=1 received_ns=100 age_ns=10 valid=1 mode_is_2=0 play_pause=0 play_aux=0 quantize=0 key_lock=0 mask=15'%(deck,deck+11,stream))
        rows.append('load_deck=%d availability=unavailable read_rc=-1 loaded=unknown'%deck)
    rows.append('complete seen=15 submitted=15 errors=0 live_seen=15 live_submitted=15 load_seen=0 load_available=0')
    source='pid=123 configured=1 nice_requested=10 nice_result=0 batch=256 interval_ms=100 cap=65536 history_incomplete=1 loaded_source_valid=0\n'
    source+='resolver_available=1 module=azmod.so build_id='+manifest['bindings']['source']['build_id']+'\n'
    source+='scan pass=1 queried=65535 current=0 ambiguous=0 unknown=65535 busy=0 invalid=0 history_limit=0\n'
    source+='complete status=time_limit ticks=3000 passes=1 next_media=1 current=0 ambiguous=0 unknown=0 busy=0 invalid=0 history_limit=0\n'
    health=[{'pid':'123','service':{'NRestarts':'0','ActiveState':'active','SubState':'running'},'local_monotonic':i} for i in [1,121]]
    identity={'pid':'123','firmware_sha256':'a'*64,'mapped':dict(manifest['sha256']),
              'flags':list(runtime.FLAGS),'deck_mask':'EEEE','deck_heartbeat_fresh':True}
    return '\n'.join(rows)+'\n',source,health,identity

class RuntimeSDKTests(unittest.TestCase):
    def test_heartbeat_clock_follows_packet_and_uses_its_precision(self):
        script=runtime.identity_script('123','/tmp/az-sdk-runtime.'+'a'*32)
        self.assertLess(script.index("awk '($1==\"pid\")"),script.index('printf "heartbeat_now='))
        def check(up,now,pid='123'):
            return runtime._identity('pid=123\ndeck_mask=EEEE\nheartbeat_pid='+pid+'\nheartbeat_up='+up+'\nheartbeat_now='+now+'\n')['deck_heartbeat_fresh']
        self.assertTrue(check('100.00','100.00'))
        self.assertTrue(check('100.00','105.00'))
        self.assertTrue(check('100.00','99.995'))
        self.assertFalse(check('100.00','99.99'))
        self.assertFalse(check('100.00','105.01'))
        self.assertFalse(check('100.00','101.00','456'))
        self.assertFalse(check('nan','101.00'))

    def test_exact_full_and_stripped_symbols_and_compiled_constants_pass(self):
        with tempfile.TemporaryDirectory() as t:
            p,root,m=package(Path(t))
            self.assertEqual(runtime.validate_package(p,root),m)
            self.assertEqual(m['binding_proofs']['source']['anchor']['rva'],522360)
            self.assertEqual(m['binding_proofs']['source']['target']['rva'],762720)
            self.assertEqual(runtime.binding_proof(p,'source'),m['binding_proofs']['source'])

    def test_package_preparation_requires_rebuilt_probe_and_host_full_companions(self):
        for delta in (240000,240360):
            with self.subTest(delta=delta),tempfile.TemporaryDirectory() as t:
                folder=Path(t);p,root,m=bound_package(folder,source_delta=delta)
                (p/'operator-loop-probe.so').write_bytes((p/'loop-probe.so').read_bytes())
                prior={'source_sha256':m['source_sha256'],'bindings':m['bindings'],
                       'core_libraries':{name:m['sha256'][name] for name in ('azmod.so','azstem.so')},
                       'probes':{'operator-loop-probe.so':m['sha256']['loop-probe.so'],
                                 'source-probe.so':m['sha256']['source-probe.so']}}
                (p/'manifest.json').write_text(json.dumps(prior))
                production=folder/'production.json';production.write_text('{}')
                destination=folder/'prepared'
                if delta==240000:
                    with self.assertRaisesRegex(ValueError,'compiled probe binding mismatch'):
                        runtime.prepare_package(destination,p,production,p,root)
                    self.assertFalse((destination/'runtime.json').exists())
                else:
                    result=runtime.prepare_package(destination,p,production,p,root)
                    self.assertEqual(result['schema'],2)
                    self.assertEqual(result['binding_proofs'],m['binding_proofs'])
                    self.assertEqual((destination/'azmod.so.full').read_bytes(),(p/'azmod.so.full').read_bytes())

    def test_full_symbol_proof_rejects_exact_historical_reader_candidate_mixup(self):
        # GNU ID, stripped/full hashes and callable RX address all match, but
        # 240000 names sources_read; candidate is at delta 240360.
        with tempfile.TemporaryDirectory() as t:
            p,root,_=bound_package(Path(t),source_delta=240000)
            with self.assertRaisesRegex(ValueError,'binding|candidate'):
                runtime.validate_package(p,root)

    def test_manifest_correction_cannot_relabel_wrong_compiled_probe_delta(self):
        with tempfile.TemporaryDirectory() as t:
            p,root,m=bound_package(Path(t),source_delta=240000)
            m['bindings']['source']['delta']=240360
            (p/'runtime.json').write_text(json.dumps(m))
            with self.assertRaisesRegex(ValueError,'compiled|binding'):
                runtime.validate_package(p,root)

    def test_missing_legacy_or_forged_binding_evidence_is_rejected_before_activation(self):
        for mutation in ['legacy','missing_full','full_symlink','no_full_symbols','wrong_full_id',
                         'wrong_stripped_code','wrong_target_name','wrong_anchor','missing_note',
                         'writeable_note','short_note','forged_proof','wrong_probe_id','section_overflow',
                         'symbol_overflow','unterminated_name','duplicate_target']:
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as t:
                p,root,m=package(Path(t))
                library,identity,anchor,target,arva,trva=PROVIDERS['source']
                full=p/(library+'.full');probe=p/'source-probe.so'
                if mutation=='legacy':m['schema']=1
                if mutation=='missing_full':full.unlink()
                if mutation=='full_symlink':full.unlink();full.symlink_to(p/library)
                if mutation=='no_full_symbols':full.write_bytes((p/library).read_bytes())
                if mutation=='wrong_full_id':full.write_bytes(binding_elf(bytes.fromhex('55'*20),[(anchor,arva,34),(target,trva,2)],True))
                if mutation=='wrong_stripped_code':(p/library).write_bytes(binding_elf(bytes.fromhex(identity),[(anchor,arva,34),(target,trva,2)],code=b'\x1f\x20\x03\xd5'))
                if mutation in ('wrong_target_name','wrong_anchor','wrong_probe_id'):
                    probe.write_bytes(binding_elf(bytes.fromhex('44'*20),binding=(trva-arva,
                        '55'*20 if mutation=='wrong_probe_id' else identity,
                        target if mutation=='wrong_anchor' else anchor,
                        'azsdk_sources_read' if mutation=='wrong_target_name' else target)))
                if mutation=='missing_note':probe.write_bytes(binding_elf(bytes.fromhex('44'*20)))
                if mutation in ('writeable_note','short_note'):
                    data=bytearray(probe.read_bytes());shoff=struct.unpack_from('<Q',data,40)[0]
                    count=struct.unpack_from('<H',data,60)[0]
                    for index in range(count):
                        at=shoff+64*index
                        if struct.unpack_from('<Q',data,at+32)[0]==281:
                            struct.pack_into('<Q',data,at+(8 if mutation=='writeable_note' else 32),3 if mutation=='writeable_note' else 280)
                    probe.write_bytes(data)
                if mutation=='forged_proof':m['binding_proofs']['source']['target']['name']='azsdk_sources_read'
                if mutation=='section_overflow':
                    data=bytearray(full.read_bytes());struct.pack_into('<Q',data,40,2**64-1);full.write_bytes(data)
                if mutation=='symbol_overflow':
                    data=bytearray(full.read_bytes());shoff=struct.unpack_from('<Q',data,40)[0]
                    for index in range(struct.unpack_from('<H',data,60)[0]):
                        if struct.unpack_from('<I',data,shoff+64*index+4)[0]==2:
                            struct.pack_into('<Q',data,shoff+64*index+32,24*65537)
                    full.write_bytes(data)
                if mutation=='unterminated_name':
                    data=full.read_bytes().replace(target.encode()+b'\0',target.encode()+b'x');full.write_bytes(data)
                if mutation=='duplicate_target':full.write_bytes(binding_elf(bytes.fromhex(identity),[(anchor,arva,34),(target,trva,2),(target,trva,2)],True))
                # Keep outer hashes/proof hashes current, so only semantic/full
                # symbol or compiled-input validation can reject these cases.
                m['sha256']={name:sha((p/name).read_bytes()) for name in runtime.FILES}
                for family,(name,*_) in PROVIDERS.items():
                    proof=m['binding_proofs'][family]
                    if (p/(name+'.full')).exists():proof['full_sha256']=sha((p/(name+'.full')).read_bytes())
                    proof['stripped_sha256']=sha((p/name).read_bytes())
                    proof['probe_sha256']=sha((p/(family+'-probe.so')).read_bytes())
                (p/'runtime.json').write_text(json.dumps(m))
                with self.assertRaises(ValueError):runtime.validate_package(p,root)

    def test_interrupted_callback_retains_bounded_owned_raw_diagnostics(self):
        for mutation in ['clean','secondary-read','oversized','changed-birth','primary-integrity']:
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as t:
                p,root,m=package(Path(t));loop,source,_,_=evidence(m)
                stage='/tmp/az-sdk-runtime.'+'a'*32
                calls=[];callback_calls=[0]
                def command(argv,**options):
                    text=argv[-1];calls.append((text,options))
                    if text.startswith('sha256sum '):
                        return ''.join(m['sha256'][n]+'  '+stage+'/'+n+'\n' for n in runtime.FILES).encode()
                    if text=='cat /tmp/az-sdk-baseline/start.sh':return b'export LD_PRELOAD=/home/root/ep122_shim.so\n'
                    if text.startswith('pidof '):return b'123\n'
                    if text.startswith("awk '{print $22}'"):return b'42\n'
                    if text.startswith('if test -r /proc/'):
                        return b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                    if text.startswith('test ! -f /tmp/az-sdk-loop-probe.'):return loop.encode()
                    if text.startswith('test ! -f /tmp/az-sdk-source-probe.'):
                        callback_calls[0]+=1
                        if mutation=='primary-integrity':raise ValueError('primary readiness failure')
                        raise RuntimeError('primary readiness failure')
                    if text.startswith('AZSDK_RUNTIME_DIAGNOSTIC_GUARD=1\n'):
                        return b'' if mutation=='changed-birth' else b'AZSDK_RUNTIME_BIRTH_OK\n'
                    if text.startswith('AZSDK_RUNTIME_DIAGNOSTIC_READ='):
                        name=text.splitlines()[0].split('=',1)[1]
                        self.assertLessEqual(options['timeout'],5)
                        self.assertIn("/proc/123/stat",text)
                        self.assertIn('= 42',text)
                        self.assertNotIn('touch ',text)
                        if mutation=='secondary-read' and name=='source':raise OSError('secondary source read failed')
                        if mutation=='oversized' and name in ('loop','source','journal','maps'):
                            return b'x'*({'loop':262144,'source':65536,'journal':524288,'maps':524288}[name]+1)
                        if name=='loop':return loop.encode()
                        if name=='source':return source.encode()
                        if name=='journal':return b'owned sdk journal\n'
                        if name=='maps':return b'owned sdk maps\n'
                        if name=='identity':
                            return ('pid=123\nfirmware_sha256='+'a'*64+'\n'+
                                    ''.join('MAP '+n+' '+m['sha256'][n]+'\n' for n in runtime.FILES)+
                                    ''.join('FLAG '+n+'\n' for n in runtime.FLAGS)+
                                    'deck_mask=EEEE\nheartbeat_fresh=1\n').encode()
                    return b''
                with patch.object(runtime.uuid,'uuid4',return_value=SimpleNamespace(hex='a'*32)), \
                     patch.object(runtime.time,'monotonic',side_effect=range(0,10000,10)),patch.object(runtime.time,'sleep'):
                    out=Path(t)/'out'
                    report=runtime.run_runtime(['ssh','root@127.0.0.1'],p,root,out,'a'*64,120,command)
                self.assertEqual(report['controller_error'],'primary readiness failure')
                self.assertTrue(report['controller_operation'].startswith('test ! -f /tmp/az-sdk-source-probe.'))
                self.assertEqual(report['status'],'FAIL' if mutation=='primary-integrity' else 'INCOMPLETE',report)
                self.assertEqual(callback_calls,[1])
                self.assertEqual(sum('systemctl stop az-sdk-baseline.service' in text for text,_ in calls),1)
                reads=[text for text,_ in calls if text.startswith('AZSDK_RUNTIME_DIAGNOSTIC_READ=')]
                if mutation=='changed-birth':
                    self.assertFalse(reads)
                    for name in ['source.log','journal.txt','maps.txt','mapped-identity.txt']:
                        self.assertFalse((out/name).exists(),name)
                elif mutation=='oversized':
                    self.assertTrue(report.get('diagnostic_collection_errors'))
                    self.assertFalse((out/'source.log').exists())
                    self.assertFalse((out/'journal.txt').exists())
                    self.assertFalse((out/'maps.txt').exists())
                else:
                    self.assertEqual((out/'loop.log').read_text(),loop)
                    self.assertEqual((out/'journal.txt').read_text(),'owned sdk journal\n')
                    self.assertEqual((out/'maps.txt').read_text(),'owned sdk maps\n')
                    self.assertTrue((out/'mapped-identity.txt').exists())
                    if mutation=='secondary-read':
                        self.assertTrue(report.get('diagnostic_collection_errors'))
                        self.assertFalse((out/'source.log').exists())
                    else:self.assertEqual((out/'source.log').read_text(),source)

    def test_both_systemd_mask_variants_are_supported_but_other_states_refuse(self):
        prefix=runtime.setup_script('/tmp/az-sdk-runtime.'+'a'*32).split('systemctl stop az-sdk-baseline.service',1)[0]
        self.assertNotIn('touch ',prefix)
        with tempfile.TemporaryDirectory() as t:
            fake=Path(t)/'systemctl'
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$MASK_FIXTURE"\nexit 1\n')
            fake.chmod(0o755)
            for mode in ['masked','masked-runtime','enabled','disabled','static','generated','']:
                env=dict(os.environ,PATH=t+os.pathsep+os.environ.get('PATH',''),MASK_FIXTURE=mode)
                result=subprocess.run(['/bin/sh','-c',prefix],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                with self.subTest(mode=mode):
                    self.assertEqual(result.returncode==0,mode in ('masked','masked-runtime'),result.stderr)

    def test_empty_stderr_failure_is_retained_as_controller_error(self):
        with tempfile.TemporaryDirectory() as t:
            p,source,_=package(Path(t))
            calls=[]
            def command(argv,**options):
                calls.append(argv[-1]);raise RuntimeError('')
            report=runtime.run_runtime(['ssh','root@127.0.0.1'],p,source,Path(t)/'out','a'*64,120,command)
            self.assertEqual(report['status'],'INCOMPLETE')
            self.assertTrue(report.get('controller_error'),report)
            self.assertIn('EP147.service',report.get('controller_operation',''))
            self.assertEqual(report['health']['status'],'INCOMPLETE')
            self.assertFalse((Path(t)/'out/setup.sh').exists())
            self.assertEqual(len(calls),1)

    def test_package_binds_actual_arm_elf_build_ids_and_source_inputs(self):
        with tempfile.TemporaryDirectory() as t:
            p,root,manifest=package(Path(t))
            self.assertEqual(runtime.validate_package(p,root)['sha256'],manifest['sha256'])
            (root/'input.c').write_text('changed')
            with self.assertRaises(ValueError):runtime.validate_package(p,root)

    def test_changed_artifact_wrong_build_id_unsafe_path_or_symlink_is_rejected(self):
        for mutation in ['artifact','build_id','path','symlink','extra']:
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as t:
                p,root,m=package(Path(t))
                if mutation=='artifact':(p/'azmod.so').write_bytes(b'corrupt')
                if mutation=='build_id':m['bindings']['loop']['build_id']='55'*20
                if mutation=='path':m['source_sha256']={'../other':sha(b'source')}
                if mutation=='symlink':(p/'azmod.so').unlink();(p/'azmod.so').symlink_to(root/'input.c')
                if mutation=='extra':m['sha256']['unreviewed.so']='a'*64
                (p/'runtime.json').write_text(json.dumps(m))
                with self.assertRaises(ValueError):runtime.validate_package(p,root)

    def test_clean_empty_runtime_pass_is_scoped_and_never_claims_loaded_acceptance(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));args=evidence(m)
            report=runtime.validate_evidence(*args,m,'a'*64)
            self.assertEqual(report['status'],'PASS')
            self.assertEqual(report['loaded_acceptance'],'INCOMPLETE')
            self.assertEqual(report['source_binding_acceptance'],'INCOMPLETE')
            self.assertEqual(report['missing_decks'],[])

    def test_every_deck_and_stream_is_required_independently(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));loop,source,health,identity=evidence(m)
            for deck in range(4):
                for prefix in ['deck=%d '%deck,'live_deck=%d '%deck]:
                    altered='\n'.join(l for l in loop.splitlines() if not l.startswith(prefix))
                    self.assertEqual(runtime.validate_evidence(altered,source,health,identity,m,'a'*64)['status'],'INCOMPLETE')
            missing=loop.replace('stream=key_lock','stream=quantize')
            self.assertEqual(runtime.validate_evidence(missing,source,health,identity,m,'a'*64)['status'],'FAIL')

    def test_missing_footer_registration_or_experimental_abi_is_incomplete(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));loop,source,health,identity=evidence(m)
            cases=[(loop.rsplit('complete ',1)[0],source),
                   (loop,source.rsplit('complete ',1)[0]),
                   (loop.replace('submitted=15','submitted=7').replace('registration_mask=15','registration_mask=7'),source),
                   (loop.replace('live_exports=1,1','live_exports=0,0'),source),
                   (loop,source.replace('resolver_available=1','resolver_available=0').replace('status=time_limit','status=resolver_refused'))]
            for a,b in cases:self.assertEqual(runtime.validate_evidence(a,b,health,identity,m,'a'*64)['status'],'INCOMPLETE')

    def test_probe_errors_fail_even_when_footer_or_other_observation_is_missing(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));loop,source,health,identity=evidence(m)
            for a,b in [(loop.replace('errors=0','errors=1'),''),('',source.replace('invalid=0','invalid=1')),
                        (loop,source.replace('status=time_limit','status=cadence_refused')),
                        (loop,source.replace('nice_result=0','nice_result=-1').rsplit('complete ',1)[0])]:
                self.assertEqual(runtime.validate_evidence(a,b,health,identity,m,'a'*64)['status'],'FAIL')

    def test_process_change_mapped_hash_firmware_or_flag_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));loop,source,health,identity=evidence(m)
            for mutation in ['pid','mapped','firmware','flags']:
                h=copy.deepcopy(health);i=copy.deepcopy(identity)
                if mutation=='pid':h[-1]['pid']='124'
                if mutation=='mapped':i['mapped']['azmod.so']='f'*64
                if mutation=='firmware':i['firmware_sha256']='f'*64
                if mutation=='flags':i['flags']=i['flags'][:-1]
                self.assertEqual(runtime.validate_evidence(loop,source,h,i,m,'a'*64)['status'],'FAIL')

    def test_stale_receipt_missing_maps_and_loaded_decks_do_not_get_empty_pass(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));loop,source,health,identity=evidence(m)
            for mutation in ['receipt','maps','loaded','heartbeat']:
                a=loop;i=copy.deepcopy(identity)
                if mutation=='receipt':a=a.replace('age_ns=10','age_ns=20000000000')
                if mutation=='maps':i.pop('mapped')
                if mutation=='loaded':i['deck_mask']='LEEE'
                if mutation=='heartbeat':i['deck_heartbeat_fresh']=False
                self.assertEqual(runtime.validate_evidence(a,source,health,i,m,'a'*64)['status'],'INCOMPLETE')

    def test_plan_only_mutates_owned_guest_tmp_ram_units_and_whitelisted_flags(self):
        stage='/tmp/az-sdk-runtime.'+'a'*32
        text=runtime.setup_script(stage)
        self.assertIn('systemctl stop az-sdk-baseline.service',text)
        self.assertIn('Restart=no',text)
        self.assertNotIn('systemctl restart EP147',text)
        for bad in ['/tmp/unowned','/tmp/az-sdk-runtime.;bad','/home/root/settings']:
            with self.assertRaises(ValueError):runtime.setup_script(bad)

    def test_runtime_script_preserves_emulator_startup_exports_and_rejects_extra_preloads(self):
        stage='/tmp/az-sdk-runtime.'+'a'*32
        baseline='#!/bin/sh\nexport EMULATOR_STARTUP=1\nexport LD_PRELOAD=/home/root/ep122_shim.so\nexec /home/root/pdj/EP147\n'
        value=runtime.startup_script(baseline,stage)
        self.assertIn('export EMULATOR_STARTUP=1\n',value)
        self.assertIn('AZSDK_LOOP_PROBE=1',value)
        self.assertIn('/home/root/ep122_shim.so:'+stage+'/azmod.so:',value)
        self.assertLess(value.index('AZSDK_SOURCE_PROBE=1'),value.index('exec /home/root'))
        for script in [value,runtime.setup_script(stage),runtime.identity_script('123',stage)]:
            result=subprocess.run(['/bin/sh','-n'],input=script.encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            self.assertEqual(result.returncode,0,result.stderr)
        for corrupt in [baseline.replace('export LD_PRELOAD=','export OTHER='),baseline+'export LD_PRELOAD=/extra\n']:
            with self.assertRaises(ValueError):runtime.startup_script(corrupt,stage)

    def test_operator_stop_path_cannot_be_used_for_another_process_or_arbitrary_file(self):
        self.assertEqual(runtime.operator_stop('operator_mode=1 stop_file=/tmp/az-sdk-loop-probe.123.100.stop\n','123'),'/tmp/az-sdk-loop-probe.123.100.stop')
        for p in ['/home/root/settings/x','/tmp/az-sdk-loop-probe.124.100.stop','/tmp/az-sdk-loop-probe.123.100.stop;true']:
            with self.assertRaises(ValueError):runtime.operator_stop('operator_mode=1 stop_file='+p+'\n','123')

    def test_failed_baseline_cannot_call_activation(self):
        called=[]
        report=runtime.after_baseline({'status':'FAIL'},lambda:called.append(1))
        self.assertEqual(called,[])
        self.assertEqual(report['status'],'INCOMPLETE')

    def test_runtime_input_change_after_baseline_remains_a_failure(self):
        def changed():raise ValueError('compiled source changed during baseline')
        self.assertEqual(runtime.after_baseline({'status':'PASS'},changed)['status'],'FAIL')

    def test_fake_ssh_executor_upload_integrity_and_partial_transport_evidence(self):
        for mutation,expected in [('clean','PASS'),('upload','FAIL'),('lost','FAIL'),('early-exit','FAIL'),('operator-complete','PASS'),('operator-retained','INCOMPLETE')]:
            with self.subTest(mutation=mutation),tempfile.TemporaryDirectory() as t:
                p,root,m=package(Path(t));loop,source,health,identity=evidence(m)
                stage='/tmp/az-sdk-runtime.'+'a'*32
                calls=[];reads=[0];operator_stopped=[False]
                def command(argv,**options):
                    text=argv[-1];calls.append(text)
                    if text.startswith('sha256sum '):
                        values=dict(m['sha256'])
                        if mutation=='upload':values['azmod.so']='f'*64
                        return ''.join(values[n]+'  '+stage+'/'+n+'\n' for n in runtime.FILES).encode()
                    if text.startswith('pidof '):
                        return b'NRestarts=0\nActiveState=failed\nSubState=failed\n' if mutation=='early-exit' else b'123\n'
                    if text=='cat /tmp/az-sdk-baseline/start.sh':return b'#!/bin/sh\nexport LD_PRELOAD=/home/root/ep122_shim.so\nexec /home/root/pdj/EP147\n'
                    if text.startswith("awk '{print $22}'"):return b'42\n'
                    if text.startswith('if test -r /proc/'):
                        reads[0]+=1
                        return (('' if mutation=='lost' and reads[0]>1 else '123\n')+'NRestarts=0\nActiveState=active\nSubState=running\n').encode()
                    if text.startswith('test ! -f /tmp/az-sdk-loop-probe.'):
                        if mutation=='lost' and reads[0]>1:raise RuntimeError('transport lost after observed process loss')
                        if mutation.startswith('operator-') and not operator_stopped[0]:return '\n'.join(row for row in loop.splitlines() if not row.startswith('complete ')).encode()+b'\n'
                        return loop.encode()
                    if text.startswith('touch /tmp/az-sdk-loop-probe.'):
                        operator_stopped[0]=True;return b''
                    if text.startswith('test ! -f /tmp/az-sdk-source-probe.'):return source.encode()
                    if text.startswith('p=123; printf'):
                        return ('pid=123\nfirmware_sha256='+'a'*64+'\n'+
                                ''.join('MAP '+n+' '+m['sha256'][n]+'\n' for n in runtime.FILES)+
                                ''.join('FLAG '+n+'\n' for n in runtime.FLAGS)+'deck_mask=EEEE\nheartbeat_fresh=1\n').encode()
                    return b''
                with patch.object(runtime.uuid,'uuid4',return_value=SimpleNamespace(hex='a'*32)), \
                     patch.object(runtime.time,'monotonic',side_effect=range(0,10000,10)),patch.object(runtime.time,'sleep'):
                    report=runtime.run_runtime(['ssh','root@127.0.0.1'],p,root,Path(t)/'out','a'*64,360,command,retain_operator=mutation=='operator-retained')
                self.assertEqual(report['status'],expected,report)
                if mutation.startswith('operator-'):
                    self.assertEqual(operator_stopped[0],mutation=='operator-complete')
                    self.assertEqual(any(text.startswith('touch /tmp/az-sdk-loop-probe.') for text in calls),mutation=='operator-complete')
                if mutation=='upload':self.assertFalse(any(s.startswith('set -e') for s in calls))

    def test_bad_health_clock_and_missing_identity_pid_cannot_escape_validation(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,m=package(Path(t));loop,source,health,identity=evidence(m)
            identity.pop('pid')
            self.assertEqual(runtime.validate_evidence(loop,source,health,identity,m,'a'*64)['status'],'INCOMPLETE')
            health[-1]['local_monotonic']=None
            self.assertEqual(runtime.validate_evidence(loop,source,health,identity,m,'a'*64)['status'],'FAIL')

if __name__=='__main__':unittest.main()
