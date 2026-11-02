#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Loaded-phase contract tests; no VM, panel or physical deck access."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/integration'))
import sdk_emulator_loaded as loaded
spec=importlib.util.spec_from_file_location('loaded_fixture',ROOT/'tests/unit/sdk_emulator_sdk_test.py')
fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)

TOKEN='a'*32
TITLE='SDK clock 44100'
CHECKS=('loaded_heartbeat','loaded_title','raw_play_notifications','play_pause','manual_loop_paused_exit_reloop','key_lock','quantize_state')

def scenario(folder):
    package,root,m=fixture.package(folder)
    header=root/'src/common/azdeckinfo.h';header.parent.mkdir(parents=True);header.write_bytes(b'header')
    probe=root/'tests/integration/sdk_position_probe.c';probe.parent.mkdir(parents=True);probe.write_bytes(b'position probe')
    m['source_sha256'].update({str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [header,probe]})
    binding={'provider_build_id':'22'*20,'header_sha256':hashlib.sha256(b'header').hexdigest(),'engine_rate':44100,'abi_size':296}
    m['position_binding']=binding
    (package/'runtime.json').write_text(json.dumps(m))
    loop,source,health,identity=fixture.evidence(m)
    loop=loop.replace('received_ns=100','received_ns=1000000000').replace('engaged=0 bounds=0 in_s=0 out_s=0','engaged=1 bounds=1 in_s=1 out_s=2').replace(' diagnostic_reason=1','')
    identity['deck_mask']='LLLL'
    evidence=folder/'selected.ppm';evidence.write_bytes(b'UI evidence')
    controller={'status':'PASS','errors':[],'missing':[],'owner_nonce':TOKEN,'pid':'123',
                'panel_source':{'sha256':'d'*64},'position_binding':copy.deepcopy(binding),
                'decks':{str(d):{name:'PASS' for name in CHECKS} for d in range(1,5)},
                'selection':{'status':'PASS','selected_title':TITLE,'evidence':str(evidence)},
                'events':[{'action':action,'deck':d} for d in range(1,5) for action in ['load','play','loopin','loopout','reloop','mastertempo']]+[{'action':'quantize'},{'action':'quantize'}]}
    controller['load_selections']={str(d):dict(controller['selection'],evidence=str(evidence)+'.deck'+str(d)) for d in range(1,5)}
    controller['load_selections']['1']=copy.deepcopy(controller['selection'])
    controller['events']=[row for d in range(1,5) for row in (
        {'action':'selection_observed','deck':d,'evidence':controller['load_selections'][str(d)]['evidence'],'local_monotonic':100+d},
        {'action':'load','deck':d})]+[row for row in controller['events'] if row['action']!='load']
    positions=dict(binding,schema=1,pid=123,status='ok',sequence=5,native_monotonic_ns=2000000000,native_boottime_ns=2000000000)
    positions['samples']=[{'sequence':i+1,'native_monotonic_ns':1600000000+i*100000000,
                          'decks':[{'deck':d,'read_rc':0,'loaded':1,'playhead':44100,'pos_s':1.,'title':TITLE} for d in range(4)]} for i in range(5)]
    context={'token':TOKEN,'driver_sha256':'d'*64,'position_binding':binding,'expected_titles':[TITLE],
             'max_source_rate':4,'rate_bound_source':'independently established fixture source-rate bound'}
    args=[loop,source,health,identity,m,'a'*64,controller,positions,900000000,context,2000000000]
    return package,root,args

class LoadedTests(unittest.TestCase):
    def test_explicit_stopped_identity_allows_new_runtime_without_a_live_intermediate_baseline(self):
        self.executor_case(mode='stopped-activation')
    def test_stopped_activation_refuses_an_existing_native_process(self):
        with self.assertRaisesRegex(ValueError,'stopped activation'):
            self.executor_case(mode='stopped-but-live')
    def test_observed_runtime_injects_separate_public_action_and_validator_without_loaded_claims(self):
        report,_=self.executor_case(mode='custom-controls')
        self.assertEqual(report['scope'],'public unit settings; empty decks')
        self.assertNotIn('loaded_title',report)
    def test_retained_observer_allows_independent_command_validation_without_scalar_completion(self):
        report,_=self.executor_case(mode='custom-controls',retain_operator=True)
        self.assertEqual(report['scope'],'public unit settings; empty decks')
    def test_retained_observer_does_not_claim_scalar_completion(self):
        report,_=self.executor_case(retain_operator=True,expected_status='INCOMPLETE')
        self.assertTrue(any('completion' in item for item in report['missing']),report)
    def test_independent_birth_check_reads_bounded_stat_with_parenthesized_comm_and_exact_u64_birth(self):
        with tempfile.TemporaryDirectory() as t:
            stat=Path(t)/'stat'
            birth='18446744073709551614'
            script=loaded.native_birth_check_script('123',birth).replace('/proc/123/stat',str(stat))
            def check(text):
                stat.write_text(text)
                return subprocess.run(['sh','-c',script],capture_output=True,timeout=2,check=True).stdout
            fields=['S']+['0']*18+[birth]+['0']*3
            self.assertEqual(check('123 (EP (odd) 147) '+' '.join(fields)+'\n'),b'AZSDK_NATIVE_BIRTH_MATCH\n')
            fields[19]='18446744073709551613'
            self.assertEqual(check('123 (EP147) '+' '.join(fields)+'\n'),b'AZSDK_NATIVE_BIRTH_REFUSED\n')
            fields[19]=birth
            for state in ('Z','X','x'):
                fields[0]=state
                self.assertEqual(check('123 (EP147) '+' '.join(fields)+'\n'),b'AZSDK_NATIVE_BIRTH_REFUSED\n')
            fields[0]='S'
            self.assertEqual(check('124 (EP147) '+' '.join(fields)+'\n'),b'AZSDK_NATIVE_BIRTH_REFUSED\n')
            self.assertEqual(check('123 ('+'a'*8192+') '+' '.join(fields)+'\n'),b'AZSDK_NATIVE_BIRTH_REFUSED\n')
    def test_expired_receiver_with_native_identity_refusal_blocks_stateful_send(self):
        report,_=self.executor_case(mode='dead-check',expected_status='FAIL')
        self.assertIn('identity refused',report.get('controller_error',''))
    def test_unavailable_independent_birth_check_does_not_claim_native_exit(self):
        report,_=self.executor_case(mode='unavailable-check',expected_status='INCOMPLETE')
        self.assertIn('check unavailable',report.get('controller_error',''))
    def test_expired_receiver_check_allows_independent_birth_diagnostics_without_reopening_sends(self):
        report,elapsed=self.executor_case(mode='expired-check',expected_status='FAIL')
        self.assertEqual(report['status'],'FAIL')
        self.assertNotIn('pinned emulator process changed',report.get('controller_error',''))
    def test_owned_runtime_forwards_explicit_one_shot_observation_setup(self):
        report,elapsed=self.executor_case(mode='observation-setup',expected_status='PASS')
        self.assertEqual(report['status'],'PASS')
    def test_owned_runtime_forwards_the_bounded_per_deck_selection_provider(self):
        report,elapsed=self.executor_case(mode='per-deck-preparation',expected_status='PASS')
        self.assertEqual(report['status'],'PASS')

    def test_post_control_public_sample_runs_before_cleanup_and_confirms_all_four_master_rows(self):
        report,_=self.executor_case(mode='post-controls',ready_after=0)
        value=report['public_native_after_controls']
        self.assertEqual(value['status'],'PASS')
        self.assertEqual(set(value['rows']),set(range(1,5)))
        self.assertEqual(value['master'],dict(status='PASS',deck=1,revision=5,observed_ns=3_000_000_000))
        self.assertTrue(all(row['query_ns']>value['after_ns'] for row in value['rows'].values()))
        self.assertEqual(value['pid'],'123');self.assertEqual(value['birth'],'42')
        self.assertEqual(value['mapped_probe_sha256'],'c'*64)

    def test_post_control_sample_never_promotes_an_earlier_failed_or_incomplete_control_result(self):
        for mode,status in [('post-controls-failed','FAIL'),('post-controls-incomplete','INCOMPLETE')]:
            with self.subTest(mode=mode):
                report,_=self.executor_case(mode=mode,ready_after=0,expected_status=status)
                self.assertEqual(report['public_native_after_controls']['master']['status'],'PASS')
                self.assertEqual(report['controls_status'],status)

    def test_post_control_sample_refuses_changed_process_module_and_owner_without_replaying_controls(self):
        for mode in ('post-controls-process','post-controls-module','post-controls-map','post-controls-owner'):
            with self.subTest(mode=mode):
                report,_=self.executor_case(mode=mode,ready_after=0,expected_status='FAIL')
                self.assertEqual(report['public_native_after_controls']['status'],'FAIL')
                self.assertNotIn('rows',report['public_native_after_controls'])

    def test_post_control_sample_requires_all_four_rows_and_a_shared_master_confirmation(self):
        for mode in ('post-controls-missing-deck','post-controls-no-master'):
            with self.subTest(mode=mode):
                report,_=self.executor_case(mode=mode,ready_after=0,expected_status='INCOMPLETE')
                value=report['public_native_after_controls']
                self.assertEqual(value['status'],'INCOMPLETE')
                self.assertNotEqual(value.get('master',{}).get('status'),'PASS')

    def test_post_control_sample_refuses_at_the_exact_global_deadline_or_when_read_crosses_it(self):
        for mode in ('post-controls-expired','post-controls-expired-read'):
            with self.subTest(mode=mode):
                report,elapsed=self.executor_case(mode=mode,ready_after=0,expected_status='INCOMPLETE')
                self.assertEqual(report['public_native_after_controls']['status'],'INCOMPLETE')
                self.assertIn('deadline',str(report['public_native_after_controls']))
                self.assertLessEqual(elapsed,601)  # Existing runtime cleanup sleeps once.

    def test_controls_wait_for_dispatch_registration_and_fresh_publisher_without_requiring_loaded_decks(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,args=scenario(Path(t))
            snapshot={'pid':'123','heartbeat':'azdecks 1\npid 123\nup 2.00\ndecks EEEE\n','uptime':'2.00',
                      'loop_log':args[0].rsplit('complete ',1)[0],'positions':args[7]}
            journal='ep122_startup: main dispatch ready; PreviewMixer released\n'
            self.assertTrue(loaded.controls_ready(snapshot,journal,args[9]['position_binding']))
            for mutation in ['dispatch','registration','stale_position','stale_heartbeat']:
                value=copy.deepcopy(snapshot);text=journal
                if mutation=='dispatch':text='ep122_startup: startup grace expired\n'
                if mutation=='registration':value['loop_log']=value['loop_log'].replace('registration_mask=15','registration_mask=7')
                if mutation=='stale_position':value['positions']['native_boottime_ns']=1
                if mutation=='stale_heartbeat':value['heartbeat']=value['heartbeat'].replace('up 2.00','up 0.00');value['uptime']='6.00'
                with self.subTest(mutation=mutation):self.assertFalse(loaded.controls_ready(value,text,args[9]['position_binding']))

    def test_loaded_package_binds_header_provider_clock_abi_and_position_source(self):
        with tempfile.TemporaryDirectory() as t:
            p,root,args=scenario(Path(t))
            self.assertEqual(loaded.validate_loaded_package(p,root,args[9]['position_binding'])['position_binding'],args[4]['position_binding'])
            for field,value in [('provider_build_id','ff'*20),('header_sha256','f'*64),('engine_rate',48000),('abi_size',128)]:
                m=copy.deepcopy(args[4]);m['position_binding'][field]=value
                (p/'runtime.json').write_text(json.dumps(m))
                with self.subTest(field=field),self.assertRaises(ValueError):loaded.validate_loaded_package(p,root,m['position_binding'])

    def test_all_four_loaded_decks_with_engaged_loop_can_pass_the_loaded_contract(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,args=scenario(Path(t));report=loaded.validate_loaded_evidence(*args)
            self.assertEqual(report['status'],'PASS',report)
            for capability in ['source_binding_acceptance','native_commands_acceptance','beatgrid_acceptance','persistence_acceptance','audio_acceptance']:
                self.assertEqual(report[capability],'INCOMPLETE')

    def test_all_deck_pass_requires_independent_selection_evidence_before_each_load(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,original=scenario(Path(t))
            for mutation in ['missing','reused','wrong_title','no_event','event_after_load']:
                args=copy.deepcopy(original)
                if mutation=='missing':args[6]['load_selections'].pop('2')
                if mutation=='reused':args[6]['load_selections']['2']['evidence']=args[6]['load_selections']['1']['evidence']
                if mutation=='wrong_title':args[6]['load_selections']['2']['selected_title']='different track'
                if mutation=='no_event':args[6]['events']=[e for e in args[6]['events'] if not (e['action']=='selection_observed' and e['deck']==2)]
                if mutation=='event_after_load':args[6]['events'][2:4]=reversed(args[6]['events'][2:4])
                with self.subTest(mutation=mutation):self.assertEqual(loaded.validate_loaded_evidence(*args)['status'],'FAIL')

    def test_engaged_loop_stationarity_requires_a_bound_and_non_aliasing_sampling_window(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,original=scenario(Path(t))
            for mutation in ['no_bound','short_loop','missing_provenance']:
                args=copy.deepcopy(original)
                if mutation=='no_bound':args[9].pop('max_source_rate')
                if mutation=='short_loop':args[0]=args[0].replace('out_s=2','out_s=1.1')
                if mutation=='missing_provenance':args[9].pop('rate_bound_source')
                with self.subTest(mutation=mutation):
                    self.assertEqual(loaded.validate_loaded_evidence(*args)['status'],'FAIL' if mutation=='missing_provenance' else 'INCOMPLETE')

    def test_changed_pid_maps_controller_nonce_or_false_controller_pass_fails(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,original=scenario(Path(t))
            for mutation in ['pid','map','nonce','controller_pid','driver','false_pass','wrong_binding']:
                args=copy.deepcopy(original)
                if mutation=='pid':args[2][-1]['pid']='124'
                if mutation=='map':args[3]['mapped']['azstem.so']='f'*64
                if mutation=='nonce':args[6]['owner_nonce']='b'*32
                if mutation=='controller_pid':args[6]['pid']='124'
                if mutation=='driver':args[6]['panel_source']['sha256']='f'*64
                if mutation=='false_pass':args[6]['decks']['3'].pop('key_lock')
                if mutation=='wrong_binding':args[7]['engine_rate']=48000
                with self.subTest(mutation=mutation):self.assertEqual(loaded.validate_loaded_evidence(*args)['status'],'FAIL')

    def test_every_loaded_stream_must_have_a_post_activation_receipt(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,original=scenario(Path(t))
            for deck in range(4):
                for stream in ['loop','play','quantize','key_lock']:
                    args=copy.deepcopy(original)
                    prefix='deck=%d '%deck if stream=='loop' else 'live_deck=%d '%deck
                    rows=[]
                    for line in args[0].splitlines():
                        if line.startswith(prefix) and (stream=='loop' or 'stream='+stream+' ' in line):line=line.replace('received_ns=1000000000','received_ns=100')
                        rows.append(line)
                    args[0]='\n'.join(rows)
                    with self.subTest(deck=deck,stream=stream):self.assertEqual(loaded.validate_loaded_evidence(*args)['status'],'INCOMPLETE')

    def test_stale_title_lag_missing_footer_wrong_loaded_mask_and_no_response_are_incomplete(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,original=scenario(Path(t))
            for mutation in ['stale','title_lag','footer','source_footer','mask','no_response','missing_selection']:
                args=copy.deepcopy(original)
                if mutation=='stale':args[7]['native_boottime_ns']=1
                if mutation=='title_lag':
                    for sample in args[7]['samples']:sample['decks'][1]['title']=''
                if mutation=='footer':args[0]=args[0].rsplit('complete ',1)[0]
                if mutation=='source_footer':args[1]=args[1].rsplit('complete ',1)[0]
                if mutation=='mask':args[3]['deck_mask']='LELL'
                if mutation=='no_response':args[6]['status']='INCOMPLETE';args[6]['decks']['2']['play_pause']='INCOMPLETE'
                if mutation=='missing_selection':args[6].pop('selection')
                with self.subTest(mutation=mutation):self.assertEqual(loaded.validate_loaded_evidence(*args)['status'],'INCOMPLETE')

    def test_probe_error_still_fails_with_missing_footer_or_controller_transport(self):
        with tempfile.TemporaryDirectory() as t:
            _,_,args=scenario(Path(t));args[0]=args[0].replace('errors=0','errors=1');args[6]['status']='INCOMPLETE'
            self.assertEqual(loaded.validate_loaded_evidence(*args)['status'],'FAIL')

    def test_wrong_owned_vm_token_causes_zero_target_writes(self):
        with tempfile.TemporaryDirectory() as t:
            folder=Path(t);p,root,args=scenario(folder);driver=folder/'driver';driver.write_bytes(b'driver')
            context=dict(args[9],container='az-sdk-automation-200-'+'a'*12,work_dir=folder,qmp_port=4461,
                         driver_path=driver,driver_sha256=hashlib.sha256(b'driver').hexdigest(),
                         prepare_track=lambda controller:None,send_control=lambda payload:self.fail('panel write'))
            calls=[]
            def command(argv,**options):calls.append(argv);return ('b'*32+'\n').encode()
            with self.assertRaisesRegex(ValueError,'ownership'):
                loaded.run_loaded_runtime(['ssh','root@127.0.0.1'],p,root,folder/'out','a'*64,480,command,context)
            self.assertTrue(calls);self.assertTrue(all(argv[:2]==['docker','inspect'] for argv in calls))

    def executor_case(self,mode='clean',ready_after=None,startup_delay=0,runtime_seconds=480,readiness_kind='ready',expected_status=None,retain_operator=False):
        with tempfile.TemporaryDirectory() as t:
            folder=Path(t);p,root,args=scenario(folder);driver=folder/'driver';driver.write_bytes(b'driver')
            active=args[0].replace('received_ns=1000000000','received_ns=3000000000').rsplit('complete ',1)[0]
            footer=args[0].split('complete ',1)[1]
            calls=[];uploads=[];panel=[];reads=[0];stopped=[False];readiness_started=[None];controlling=[False];prepared_decks=[]
            receiver_expired=[False];failed_sends=[0];public_reads=[];public_owner_changed=[False]
            post_controls=mode.startswith('post-controls')
            class Clock:
                now=0.
                def monotonic(self):return self.now
                def sleep(self,seconds):self.now+=seconds
            clock=Clock()
            context=dict(args[9],container='az-sdk-automation-200-'+'a'*12,work_dir=folder,qmp_port=4461,
                         driver_path=driver,driver_sha256=hashlib.sha256(b'driver').hexdigest(),
                         prepare_track=lambda controller:args[6]['selection'],send_control=lambda payload:panel.append(payload))
            context['retain_operator']=retain_operator
            if post_controls:context['public_probe_sha256']='c'*64
            if mode=='custom-controls':
                del context['prepare_track'];del context['expected_titles']
            if mode in ('stopped-activation','stopped-but-live'):
                context['stopped_identity']={'pid':'44','birth':'55'}
            if mode=='per-deck-preparation':
                context['prepare_deck_load']=lambda controller,deck,selected:prepared_decks.append(deck)
            if mode=='observation-setup':
                context['prepare_controls']=lambda controller:{'status':'PASS','scope':'setup only'}
            if mode in ('expired-check','dead-check','unavailable-check'):
                context['verify_process']=lambda pid,birth:not receiver_expired[0]
                def closed_send(payload):
                    failed_sends[0]+=1;raise BrokenPipeError('receiver expired')
                context['send_control']=closed_send
            if mode=='close-failure':
                class Transport:
                    def send(self,payload):panel.append(payload)
                    def close(self):raise RuntimeError('transport close failed')
                context['panel_transport_factory']=lambda pid,birth,guard:Transport()
            expected=copy.deepcopy(args[6]);expected['panel_source']['sha256']=context['driver_sha256']
            def observation_packet():
                reads[0]+=1;packet=copy.deepcopy(args[7]);offset=2000000000 if controlling[0] else 0
                packet['native_monotonic_ns']+=offset;packet['native_boottime_ns']+=offset
                for sample in packet['samples']:sample['native_monotonic_ns']+=offset
                uptime=str(2+offset/1000000000)
                return ('azdecks 1\npid 123\nup '+uptime+'\ndecks LLLL\n\n__AZSDK_LOOP__\n'+active+'\n__AZSDK_POSITION__\n'+json.dumps(packet)+'\n__AZSDK_UPTIME__\n'+uptime+' 0\n').encode()
            class FakeController:
                def __init__(self,*pos,**options):self.options=options;self.send=pos[5]
                def run_selected_track(self,prepare,titles,prepare_deck_load=None,prepare_controls=None):
                    assert mode!='custom-controls','public action must not run a loaded-track sequence'
                    controlling[0]=True
                    if mode in ('expired-check','dead-check','unavailable-check'):
                        receiver_expired[0]=True
                        try:self.send(b'\x00'*153)
                        except BrokenPipeError:
                            expected['status']='FAIL';expected['errors']=['receiver expired'];return expected
                    self.options['read_observation']();prepare(self);self.send(b'\x00'*153)
                    if prepare_deck_load is not None:
                        assert prepare_deck_load is context['prepare_deck_load']
                        prepare_deck_load(self,2,expected['selection'])
                        assert prepared_decks==[2]
                    if mode=='observation-setup':
                        assert prepare_controls is context['prepare_controls']
                    if mode=='post-controls-failed':expected.update(status='FAIL',errors=['original control failure'])
                    if mode=='post-controls-incomplete':expected.update(status='INCOMPLETE',missing=['original missing control witness'])
                    if mode=='post-controls-expired':clock.now=600
                    return expected
            def command(argv,**options):
                calls.append(argv)
                if argv[:2]==['docker','inspect']:
                    if public_owner_changed[0]:public_owner_changed[0]=False;return ('b'*32+'\n').encode()
                    return (TOKEN+'\n').encode()
                text=argv[-1]
                if text.startswith('AZSDK_PUBLIC_READ=1\n'):
                    self.assertTrue(post_controls);self.assertEqual(len(panel),1);self.assertFalse(stopped[0])
                    self.assertIn('p=123',text);self.assertIn('b=42',text)
                    self.assertEqual(text.count('test "$(check_public_native)" = AZSDK_NATIVE_BIRTH_MATCH'),2)
                    self.assertNotIn('touch ',text);self.assertNotIn('kill ',text)
                    self.assertGreater(options['timeout'],0);self.assertLessEqual(options['timeout'],min(15,600-clock.now))
                    public_reads.append(text)
                    if mode=='post-controls-process':raise subprocess.CalledProcessError(93,argv)
                    if mode=='post-controls-expired-read':clock.now=600
                    if mode=='post-controls-owner':public_owner_changed[0]=True
                    if '__AZSDK_MODULE_BEFORE__' not in text:return observation_packet()
                    module='/home/root/settings/az-usbload/community/sdk-state-probe/sdk-state-probe.so'
                    module_sha='d'*64 if mode=='post-controls-module' else 'c'*64
                    flags='r--p' if mode=='post-controls-map' else 'r-xp'
                    identity=module_sha+'  '+module+'\n7000-8000 '+flags+' 00000000 00:01 123 '+module+'\n'
                    rows=[]
                    for d in range(1,4 if mode=='post-controls-missing-deck' else 5):
                        row=dict(v=1,seq=d,deck=d,returned=d,query_ns=4_500_000_000,rc=0,valid=1024|262144|64,
                                 master_valid=1,master=int(d==1),master_rev=5,master_ns=3_000_000_000,
                                 load_valid=1,native_load=d,load_unit=d,load_epoch=6,load_ns=3_000_000_000,source_load=0,
                                 tempo_valid=1,tempo='120.500000',tempo_state=d,tempo_rev=2,tempo_ns=3_000_000_000,
                                 slider_valid=0,slider_locked=-1,slider_unit=0,slider_rev=0,slider_ns=0)
                        if mode=='post-controls-no-master':row.update(master_valid=0,master=-1,master_rev=0,master_ns=0)
                        rows.append('SDK_PUBLIC_NATIVE '+' '.join(str(k)+'='+str(v) for k,v in row.items()))
                    packet=copy.deepcopy(args[7]);packet.update(native_monotonic_ns=5_000_000_000,native_boottime_ns=5_000_000_000)
                    for sample in packet['samples']:sample['native_monotonic_ns']+=3_000_000_000
                    return ('__AZSDK_MODULE_BEFORE__\n'+identity+'\n__AZSDK_LOG__\n'+'\n'.join(rows)+
                            '\n__AZSDK_POSITION__\n'+json.dumps(packet)+'\n__AZSDK_HEARTBEAT__\nazdecks 1\npid 123\nup 5.0\ndecks LLLL\n'+
                            '\n__AZSDK_UPTIME__\n5.0 0\n\n__AZSDK_MODULE_AFTER__\n'+identity).encode()
                if text.startswith('AZSDK_NATIVE_BIRTH_CHECK=1\n'):
                    self.assertIn(mode,('expired-check','dead-check','unavailable-check','stopped-activation'))
                    self.assertNotIn('touch ',text);self.assertNotIn('kill ',text)
                    if mode=='stopped-activation':return b'AZSDK_NATIVE_BIRTH_REFUSED\n'
                    if mode=='dead-check':return b'AZSDK_NATIVE_BIRTH_REFUSED\n'
                    if mode=='unavailable-check':return b''
                    return b'AZSDK_NATIVE_BIRTH_MATCH\n'
                if text=='pidof EP147 || true':return b'' if mode=='stopped-activation' else b'44\n'
                if text=="awk '{print $22}' /proc/44/stat":return b'55\n'
                if text.startswith('sha256sum '):
                    stage=text.split()[1].rsplit('/',1)[0]
                    return ''.join(args[4]['sha256'][n]+'  '+stage+'/'+n+'\n' for n in loaded.runtime.FILES).encode()
                if text=='cat /tmp/az-sdk-baseline/start.sh':return b'#!/bin/sh\nexport LD_PRELOAD=/home/root/ep122_shim.so\nexec /home/root/pdj/EP147\n'
                if text.startswith('cat >') and 'start.sh' in text:uploads.append(options['input']);return b''
                if 'systemctl stop az-sdk-baseline.service' in text:clock.now+=startup_delay
                if text.startswith('pidof EP147 || true;'):return b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                if text=="awk '{print $22}' /proc/123/stat":return b'42\n'
                if text.startswith('if test -r /proc/'):return b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                if text.startswith('test ! -f /tmp/az-sdk-loop-probe.'):return (active+('complete '+footer if stopped[0] else '')).encode()
                if text.startswith('test ! -f /tmp/az-sdk-source-probe.'):return args[1].encode()
                if text.startswith('cat /tmp/az-decks;'):
                    return observation_packet()
                if text.startswith('touch /tmp/az-sdk-loop-probe.'):stopped[0]=True
                if text.startswith('journalctl _PID=') and ready_after is not None:
                    if readiness_started[0] is None:readiness_started[0]=clock.now
                    if clock.now-readiness_started[0]<ready_after:return b'ep122_startup: waiting for main dispatch\n'
                    return b'ep122_startup: main dispatch ready; PreviewMixer released\n' if readiness_kind=='ready' else b'ep122_startup: startup grace expired\n'
                if text.startswith('journalctl'):return b'ep122_startup: main dispatch ready; PreviewMixer released\n'
                if text.startswith('p=123; printf'):
                    return ('pid=123\nfirmware_sha256='+'a'*64+'\n'+''.join('MAP '+n+' '+args[4]['sha256'][n]+'\n' for n in loaded.runtime.FILES)+''.join('FLAG '+n+'\n' for n in loaded.runtime.FLAGS)+'deck_mask=LLLL\nheartbeat_fresh=1\n').encode()
                return b''
            monotonic=clock.monotonic if ready_after is not None else iter(range(0,100000,10)).__next__
            sleep=clock.sleep if ready_after is not None else lambda seconds:None
            with patch.object(loaded,'Controller',FakeController),patch.object(loaded.time,'monotonic',side_effect=monotonic),patch.object(loaded.time,'sleep',side_effect=sleep):
                if mode=='custom-controls':
                    def action(controller):
                        controller.options['read_observation']();controller.send(b'\0'*153)
                        return {'status':'PASS','public_setting_checks':8}
                    def validate(*evidence):
                        self.assertEqual(evidence[6]['public_setting_checks'],8)
                        return {'status':'PASS','scope':'public unit settings; empty decks'}
                    report=loaded.run_observed_runtime(['ssh','root@127.0.0.1'],p,root,folder/'out','a'*64,runtime_seconds,command,context,
                                                       execute_controls=action,validate_observed=validate)
                else:report=loaded.run_loaded_runtime(['ssh','root@127.0.0.1'],p,root,folder/'out','a'*64,runtime_seconds,command,context)
            expected_status=expected_status or ('INCOMPLETE' if mode=='close-failure' else 'PASS')
            self.assertEqual(report['status'],expected_status,report)
            if post_controls:
                self.assertEqual(len(panel),1,'post-control observation must never replay controls')
                self.assertTrue(stopped[0],'capture must finish/refuse before cleanup')
                self.assertEqual(bool(public_reads),mode!='post-controls-expired')
                self.assertIn('public_native_after_controls',report)
                return report,clock.now
            if mode in ('dead-check','unavailable-check'):
                self.assertEqual(failed_sends,[0]);self.assertEqual(panel,[])
                self.assertFalse(stopped[0]);return report,clock.now
            if mode=='expired-check':
                self.assertEqual(failed_sends,[1]);self.assertEqual(panel,[])
                self.assertTrue(stopped[0],'read-only collection must continue after receiver loss')
                for name in ('source.log','loop.log','journal.txt','mapped-identity.txt'):
                    self.assertTrue((folder/'out'/name).exists(),name)
                proof=folder/'out/process-identity-fallback.jsonl'
                self.assertTrue(proof.exists())
                self.assertTrue(all(json.loads(line)['matched'] for line in proof.read_text().splitlines()))
                return report,clock.now
            if mode=='close-failure':self.assertIn('transport close failed',report.get('transport_close_error',''))
            controls_expected=expected_status=='PASS' or mode=='close-failure' or retain_operator
            self.assertEqual(len(panel),int(controls_expected));self.assertEqual(stopped[0],controls_expected and not retain_operator)
            if retain_operator:
                lifecycle=json.loads((folder/'out/lifecycle-report.json').read_text())
                self.assertEqual(lifecycle['status'],'INCOMPLETE')
                self.assertNotIn('complete ',(folder/'out/loop.log').read_text())
            self.assertEqual(len(uploads),1);self.assertIn(b'export AZSDK_POSITION_PROBE=1\n',uploads[0])
            self.assertEqual((folder/'out/start.sh').read_bytes(),uploads[0])
            self.assertTrue((folder/'out/lifecycle-report.json').exists())
            self.assertGreaterEqual(report['observations'],3 if controls_expected else 1)
            observation_commands=[c[-1] for c in calls if c[-1].startswith('cat /tmp/az-decks;') or c[-1].startswith('cat /proc/uptime;')]
            self.assertTrue(observation_commands)
            for text in observation_commands:self.assertGreater(text.index('cat /proc/uptime'),text.rindex('.json'))
            return report,clock.now

    def test_executor_composes_actual_lifecycle_position_export_controls_and_loaded_validation(self):
        self.executor_case()

    def test_transport_close_failure_retains_all_collected_runtime_evidence(self):
        self.executor_case('close-failure')

    def test_readiness_budget_allows_real_dispatch_after_130_or_150_seconds(self):
        for delay in [130,150]:
            with self.subTest(delay=delay):self.executor_case(ready_after=delay)

    def test_readiness_expiry_marker_never_qualifies_as_dispatch(self):
        report,elapsed=self.executor_case(ready_after=130,readiness_kind='expiry',expected_status='INCOMPLETE')
        self.assertIn('readiness deadline',report.get('controller_error',''))
        self.assertLessEqual(elapsed,180.21)

    def test_readiness_budget_does_not_extend_the_existing_overall_bound(self):
        report,elapsed=self.executor_case(ready_after=130,startup_delay=150,runtime_seconds=120,expected_status='INCOMPLETE')
        self.assertIn('deadline',report.get('controller_error',''))
        self.assertLessEqual(elapsed,240.21)

if __name__=='__main__':unittest.main()
