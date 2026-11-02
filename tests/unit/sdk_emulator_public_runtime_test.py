#!/usr/bin/env python3
"""Fake-I/O wiring of normal trust, one UI apply consumption and new PID.

No target binary, SSH, Docker, VM, approval file or panel device is accessed.
"""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
import sdk_emulator_public_runtime as public
from sdk_panel_transport import PanelTransport as RealPanel

TOKEN='a'*32
BINDING=dict(provider_build_id='b'*40,header_sha256='c'*64,engine_rate=44100,abi_size=296)
SSH=['ssh','-T','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile=/tmp/pinned',
     '-o','BatchMode=yes','-o','ConnectTimeout=5','-p','2390','root@127.0.0.1']

class PublicRuntimeTests(unittest.TestCase):
    def test_pending_ui_resume_preserves_approval_and_consumes_one_real_apply(self):
        self.case('loaded',probe_id='sdk-cue-command-probe',resume='ok')

    def test_pending_ui_resume_refuses_foreign_owner_before_any_mutation(self):
        self.case('loaded',probe_id='sdk-cue-command-probe',resume='foreign')

    def case(self,mode='ok',wave=False,probe_id='sdk-state-probe',unknown_first=False,resume=None):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);(root/'src/azmod').mkdir(parents=True)
            (root/'src/azmod/azmod-mod.h').write_bytes(b'header')
            driver=root/'driver';driver.write_bytes(b'fixture driver')
            receiver=root/'receiver';receiver.write_bytes(b'fixture receiver')
            probe=root/'probe.azmod'
            probe_path=public.PROBE.replace('sdk-state-probe',probe_id);probe_log=public.LOG.replace('sdk-state-probe',probe_id)
            files={'azmod.json':json.dumps(dict(id=probe_id,version='0.1.0',api=1,hooks=probe_id=='sdk-slip-capture',firmware=['ep147-79d43e5de779fc1b','ep147-9e9454e357389ece'] if probe_id=='sdk-slip-capture' else ['any'])).encode(),
                   probe_id+'.so':b'fixture normal module','LICENSE':b'GPL','icon.png':b'icon'}
            with zipfile.ZipFile(probe,'w') as z:
                for name,data in files.items():z.writestr(name,data)
            probe_sha=hashlib.sha256(files[probe_id+'.so']).hexdigest()
            binding=root/'probe-manifest.json';binding.write_text(json.dumps(dict(package={'sha256':public.digest(probe)},binary={'sha256':probe_sha},sha256={'include/azmod-mod.h':hashlib.sha256(b'header').hexdigest()})))
            wave_package=root/'wave.azmod'
            wave_files={'azmod.json':json.dumps(dict(azmod=1,id='sdk-wave-layer-probe',version='0.1.0',api=1,hooks=False,firmware=['any'])).encode(), 'sdk-wave-layer-probe.so':b'wave fixture','LICENSE':b'GPL','icon.png':b'icon'}
            with zipfile.ZipFile(wave_package,'w') as z:
                for name,data in wave_files.items():z.writestr(name,data)
            wave_sha=hashlib.sha256(wave_files['sdk-wave-layer-probe.so']).hexdigest()
            wave_config=dict(package=wave_package,package_sha256=public.digest(wave_package),binary_sha256=wave_sha,profile='200',core_build_id='e'*40,core_sha256='f'*64,core_source_manifest_sha256='1'*64,public_header_sha256=hashlib.sha256(b'header').hexdigest())
            state=dict(owner=TOKEN,pid='44',birth='55',phase='',approved=False,request=False,consumed=0,staged=0,ui=0,settings_inputs=0,loaded_inputs=0,now=100.,seq=0,log=[],reload_reads=0)
            rows={d:dict(v=1,deck=d,returned=d,rc=0,valid=0,mt=-1,mt_unit=0,mt_rev=0,mt_ns=0,sync=-1,sync_unit=0,sync_rev=0,sync_ns=0,raw=-1) for d in range(1,5)}
            if mode=='stale-owner':state['owner']='b'*32
            if resume:state.update(pid='100',birth='1000',approved=True,request=True)
            def command(argv,**options):
                if argv[:2]==['docker','inspect']:return (state['owner']+'\n').encode()
                self.assertEqual(argv[:-1],SSH)
                text=argv[-1]
                if text=='pidof EP147 || true':return ((state['pid']+'\n') if state['pid'] else '').encode()
                if text.startswith("awk '{s=$0;"):return (state['birth']+'\n').encode()
                if text.startswith('AZSDK_NATIVE_BIRTH_CHECK=1\n'):
                    match=state['pid'] and '/proc/'+state['pid']+'/stat' in text and '-v b='+state['birth'] in text
                    return b'AZSDK_NATIVE_BIRTH_MATCH\n' if match else b'AZSDK_NATIVE_BIRTH_REFUSED\n'
                if 'tar -xf -' in text:
                    state['staged']+=1
                    self.assertNotIn('approved',text);self.assertNotIn('.origin',text)
                    selected=wave_files if '/sdk-wave-layer-probe' in text else files
                    with tarfile.open(fileobj=io.BytesIO(options['input'])) as z:self.assertEqual(set(z.getnames()),set(selected))
                    return ''.join(hashlib.sha256(data).hexdigest()+'  /stage/'+name+'\n' for name,data in selected.items()).encode()
                if text.startswith('umask 077; mkdir /tmp/az-sdk-panel.'):
                    self.assertEqual(options['input'],b'fixture receiver');return b''
                if 'mv /run/az-community.req ' in text:
                    self.assertTrue(state['request']);self.assertEqual(state['owner'],TOKEN)
                    state['consumed']+=1;state['request']=False;state['pid']='';state['birth']='';return b''
                self.assertTrue(text.startswith('AZSDK_PUBLIC_READ=1\n'),text[:100])
                if 'heartbeat_now=' in text:
                    return ('pid=100\nfirmware_sha256='+'d'*64+'\nMAP azmod.so '+'f'*64+'\n'+''.join('FLAG '+flag+'\n' for flag in public.runtime.FLAGS)+'deck_mask=EEEE\nheartbeat_pid=100\nheartbeat_up=100.00\nheartbeat_now=100.00\n').encode()
                if 'cat '+public.APPROVED in text:return ((probe_id+' '+probe_sha+'\n')+('sdk-wave-layer-probe '+wave_sha+'\n' if wave and mode!='missing-wave-approval' else '')).encode() if state['approved'] else b''
                if 'cat /proc/' in text and '/maps' in text:
                    return (('1-2 r-xp 00000000 01:01 7 '+probe_path+'\n')+('3-4 r-xp 00000000 01:01 8 /home/root/settings/az-usbload/community/sdk-wave-layer-probe/sdk-wave-layer-probe.so\n' if wave and mode!='missing-wave-map' else '')).encode() if state['phase']=='settings' else b''
                if 'sha256sum /home/root/settings/az-usbload/community/sdk-wave-layer-probe/' in text:return (wave_sha+'  /home/root/settings/az-usbload/community/sdk-wave-layer-probe/sdk-wave-layer-probe.so\n').encode()
                if 'cat /tmp/azmod-community/sdk-wave-layer-probe.log' in text:
                    return (('loaded 0.1.0 on fixture (0 hooks)\nSDK_PUBLIC_WAVE fixture\n\n__AZSDK_WAVE_POSITION__\n'+json.dumps(dict(BINDING,schema=1,pid=int(state['pid']),status='ok',sequence=0,native_monotonic_ns=100_000_000_000,native_boottime_ns=100_000_000_000,samples=[]))+'\n__AZSDK_WAVE_UPTIME__\n100.0 0\n') if state['phase']=='settings' else 'allowed on the deck: sha256 '+wave_sha+' (loads after Reload)\n').encode()
                if 'sha256sum '+probe_path in text:return (probe_sha+'  '+probe_path+'\n').encode()
                if '__AZSDK_POSITION__' in text:
                    state['now']+=.02;now=int(state['now']*1e9)
                    for deck,row in rows.items():
                        state['seq']+=1
                        value=dict(row,seq=state['seq'],query_ns=now)
                        state['log'].append('SDK_PUBLIC_STATE '+' '.join(k+'='+str(v) for k,v in value.items()))
                    packet=dict(BINDING,schema=1,pid=int(state['pid']),status='ok',sequence=0,native_monotonic_ns=now,native_boottime_ns=now,samples=[])
                    heartbeat='azdecks 1\npid '+state['pid']+'\nup '+str(state['now'])+'\ndecks EEEE\n'
                    return ('\n'.join(state['log'])+'\n\n__AZSDK_POSITION__\n'+json.dumps(packet)+'\n__AZSDK_HEARTBEAT__\n'+heartbeat+'\n__AZSDK_UPTIME__\n'+str(state['now'])+' 0\n').encode()
                if 'cat '+probe_log in text:
                    return (('loaded 0.1.0 on fixture (0 hooks)\nSDK_PUBLIC_STATE capabilities version=1 fields=768 commands=0\n') if state['phase']=='settings' else 'allowed on the deck: sha256 '+probe_sha+' (loads after Reload)\n').encode()
                if '/run/az-community.req' in text:return b'apply\n' if state['request'] else b''
                if 'cat /tmp/az-decks;' in text:
                    state['reload_reads']+=1
                    mask='UUUU' if mode=='unknown-heartbeat' or unknown_first and state['reload_reads']==1 else 'LEEE' if mode=='loaded-heartbeat' else 'EEEE'
                    return ('azdecks 1\npid '+state['pid']+'\nup '+str(state['now'])+'\ndecks '+mask+'\n\n__AZSDK_UPTIME__\n'+str(state['now'])+' 0\n').encode()
                self.fail('unhandled fake request '+text[-200:])
            class Receiver:
                _strict_ssh=staticmethod(RealPanel._strict_ssh)
                def __init__(self,ssh,path,sha,pid,birth,token,guard):
                    self.pid=pid;self.birth=birth;self.owner_token=token;self.closed=False;self.sequence=0
                def verify_process(self,pid,birth):return (pid,birth)==(state['pid'],state['birth'])
                def close(self):self.closed=True
                def send(self,frame):return True
            class Controller:
                timeout=30;interval=.2
                def __init__(self):
                    self.pid=state['pid'];self.driver=SimpleNamespace(BUS_ERP=0,BUS_MIXER=2);self.events=[]
                def _release(self,bus):pass
                def observe(self):return {'mask':'EEEE'}
                def _now(self):return state['now']
                def sleep(self,seconds):state['now']+=seconds
                def button(self,deck,name):
                    state['settings_inputs']+=1;state['now']+=.02
                    key='mt' if name=='mastertempo' else 'sync';row=rows[deck]
                    row[key]=0 if row[key]==1 else 1;row[key+'_unit']=deck+(0 if key=='mt' else 10)
                    row[key+'_rev']+=1;row[key+'_ns']=int(state['now']*1e9);row['valid']|=256 if key=='mt' else 512
                    if key=='sync':row['raw']=2 if row[key] else 0
                    self.events.append(dict(action=name,deck=deck))
            def prepare_allow(controller,read_guest,sessions):
                state['ui']+=1
                state['approved']=mode not in ('missing-approval','loaded-missing-approval');state['request']=True
                if mode=='changed-owner':state['owner']='b'*32
                return dict(ui_allow=True,ui_reload=True,evidence='fixture UI screenshot')
            def observed(*args,**options):
                phase=args[3];phase.mkdir();state['phase']=phase.name
                ctx=args[7]
                if phase.name=='approval':state.update(pid='100',birth='1000')
                else:
                    self.assertEqual(state['pid'],'');self.assertEqual(ctx['stopped_identity'],{'pid':'100','birth':'1000'})
                    state.update(pid='100' if mode=='same-pid' else '200',birth='2000')
                channel=ctx['panel_transport_factory'](state['pid'],state['birth'],lambda:None)
                try:result=options['execute_controls'](Controller())
                finally:channel.close()
                if mode=='changed-before-apply' and phase.name=='approval':state['owner']='b'*32
                result['stage']='/tmp/az-sdk-runtime.'+'c'*32
                return result
            context=dict(token=TOKEN,container='az-sdk-automation-200-'+'a'*12,work_dir=root,qmp_port=4490,
                         prepare_allow=prepare_allow,position_binding=BINDING,probe_manifest=binding,probe_package=probe,probe_id=probe_id,
                         driver_path=driver,driver_sha256=public.digest(driver),receiver_path=receiver,receiver_sha256=public.digest(receiver))
            if wave:context['wave_probe']=wave_config
            if resume:
                prior=dict(status='INCOMPLETE',errors=[],missing=['fresh empty-unit heartbeat'],health={'status':'PASS'},
                           pid='100',birth='1000',ui_allow=True,ui_reload=True,reload_request='apply',approved_sha256=probe_sha,
                           artifact_sha256={'azmod.so':'f'*64},position_binding=BINDING,stage='/tmp/az-sdk-runtime.'+'c'*32)
                context['approval_resume']=dict(container=context['container'],token=TOKEN if resume=='ok' else 'b'*32,approval=prior)
            if mode=='wave-only':context['run_settings']=False
            if mode in ('loaded','loaded-same-pid','loaded-same-birth','loaded-missing-approval'):
                def loaded_executor(stopped,remaining,verify_new=None):
                    self.assertEqual(stopped,{'pid':'100','birth':'1000'})
                    self.assertEqual(state['pid'],'');self.assertEqual(state['consumed'],1)
                    self.assertTrue(120<=remaining<=1380)
                    state.update(pid='100' if mode=='loaded-same-pid' else '200',birth='1000' if mode=='loaded-same-birth' else '2000')
                    self.assertTrue(callable(verify_new),'new process must be verified before LOAD')
                    verify_new(state['pid'],state['birth']);state['loaded_inputs']+=1
                    events=[dict(action='LOAD',deck=1)]
                    if probe_id=='sdk-slip-capture':
                        from sdk_emulator_slip import run_slip_trial
                        raw=[];events=[]
                        def load(deck):
                            events.append(dict(action='LOAD',deck=deck));raw.append(dict(seq=len(raw)+1,observed_ns=int(state['now']*1e9)+1,raw=0,receiver=0x8000))
                        def button(deck,name):
                            state['now']+=.01;events.append(dict(action=name,deck=deck));raw.append(dict(seq=len(raw)+1,observed_ns=int(state['now']*1e9)+1,raw=1 if len(events)==2 else 0,receiver=0x8000))
                        def read():
                            state['now']+=.01
                            return dict(records=list(raw),native_now_ns=int(state['now']*1e9),stats={'emitted':len(raw)})
                        controller=SimpleNamespace(timeout=30,configured_control_timeout=30,interval=.2,_owner=lambda:None,_now=lambda:state['now'],
                            load=load,button=button,sleep=lambda n:None,screenshot=lambda n:n,wait=lambda predicate,label:{'decks':[{'loaded':True,'title':'track'}]})
                        result=run_slip_trial(controller,lambda c:dict(status='PASS',selected_title='track',evidence='actual UI row'),['track'],read,lambda:verify_new(state['pid'],state['birth']),state['now']+100,ready_for_slip=lambda c:True)
                        self.assertEqual(result['status'],'PASS')
                    return dict(status='PASS',pid=state['pid'],birth=state['birth'],mapped_probe_sha256=probe_sha,events=events,scope='fixture loaded checks')
                context.update(run_settings=False,loaded_executor=loaded_executor)
            capture=[]
            def validate_wave(text,ctx,expected):
                self.assertEqual(text,'SDK_PUBLIC_WAVE fixture\n')
                self.assertEqual(ctx['old_process'],{'pid':'100','birth':'1000'})
                self.assertEqual(ctx['new_process'],{'pid':'200','birth':'2000'})
                self.assertEqual(ctx['pin_before'],ctx['pin_after'])
                self.assertEqual(ctx['approved_probe_sha256'],wave_sha)
                self.assertEqual(ctx['apply_count'],1);self.assertTrue(ctx['old_absent'])
                self.assertEqual(expected['mapped_probe_sha256'],wave_sha);capture.append(ctx)
                if mode=='wave-final-missing':
                    from sdk_emulator_wave_layer import MissingEvidence
                    raise MissingEvidence('bounded final wave record unavailable')
                if mode=='wave-invalid-payload':raise ValueError('independent sample inverse mismatch')
                return dict(status='PASS',scope='fixture wave validation')
            from sdk_emulator_wave_layer import validate_wave_capture
            with patch('sdk_emulator_wave_layer.validate_wave_capture',side_effect=validate_wave), patch.object(public.runtime,'validate_package',return_value=dict(position_binding=BINDING,sha256={'azmod.so':'f'*64},bindings={'source':{'build_id':'e'*40}})), \
                 patch.object(public,'PanelTransport',Receiver),patch.object(public,'run_observed_runtime',side_effect=observed):
                if mode=='stale-owner':
                    with self.assertRaisesRegex(ValueError,'ownership'):public.run_public_runtime(SSH,root,root,root/'out','d'*64,1500,command,context)
                    self.assertEqual(state['staged'],0);return
                if resume and resume!='ok':
                    with self.assertRaisesRegex(ValueError,'same owned process'):
                        public.run_public_runtime(SSH,root,root,root/'out','d'*64,1500,command,context)
                    self.assertEqual((state['staged'],state['ui'],state['consumed'],state['loaded_inputs']),(0,0,0,0));return
                report=public.run_public_runtime(SSH,root,root,root/'out','d'*64,1500,command,context)
            if resume:
                self.assertEqual((state['staged'],state['ui']),(0,0))
                self.assertTrue(report['approval']['resumed_pending_ui'])
            self.assertEqual(state['consumed'],int(mode in ('ok','same-pid','missing-wave-map','wave-final-missing','wave-invalid-payload','wave-only','loaded','loaded-same-pid','loaded-same-birth')))
            if wave:
                self.assertEqual(state['staged'],2,'both normal packages must stage without touching approval records')
                if mode in ('ok','wave-only'):
                    self.assertEqual(len(capture),1);self.assertEqual(report['settings']['wave']['status'],'PASS')
                elif mode in ('wave-final-missing','wave-invalid-payload'):self.assertEqual(len(capture),1)
                else:self.assertEqual(len(capture),0)
            if mode=='wave-final-missing':
                self.assertEqual(report['status'],'INCOMPLETE',report)
                self.assertEqual(report['settings']['status'],'PASS');self.assertEqual(state['settings_inputs'],24)
                self.assertEqual(report['wave']['status'],'INCOMPLETE')
            elif mode=='wave-invalid-payload':
                self.assertEqual(report['status'],'FAIL',report)
                self.assertEqual(report['wave']['status'],'FAIL',report)
                self.assertEqual(report['settings']['status'],'INCOMPLETE')
                self.assertEqual(state['settings_inputs'],0)
            elif mode=='wave-only':
                self.assertEqual(state['settings_inputs'],0,'wave-only phase must never repeat qualified settings stimuli')
                self.assertEqual(report['status'],'PASS',report)
                self.assertEqual(report['settings_execution'],'NOT_RUN')
                self.assertEqual(report['settings']['checks'],{})
                self.assertEqual(report['settings']['events'],[])
            elif mode=='loaded':
                self.assertEqual(report['status'],'PASS',report)
                self.assertEqual(state['settings_inputs'],0)
                self.assertEqual(report['loaded']['events'],[dict(action='LOAD',deck=1)]+([dict(action='slip',deck=1)]*2 if probe_id=='sdk-slip-capture' else []))
                self.assertEqual(state['loaded_inputs'],1)
                self.assertEqual(state['consumed'],1)
                if unknown_first:self.assertEqual(state['reload_reads'],2)
            elif mode=='ok':
                self.assertEqual(report['status'],'PASS',report)
                self.assertEqual(state['settings_inputs'],24)
                self.assertEqual(report['settings']['pid'],'200')
            else:
                self.assertNotEqual(report['status'],'PASS',report)
                self.assertEqual(state['loaded_inputs'],0)
                self.assertEqual(state['settings_inputs'],0,'new PID must qualify before any public setting stimulus')
                if mode in ('unknown-heartbeat','loaded-heartbeat'):self.assertEqual(state['consumed'],0)
            return report
    def test_loop_command_module_uses_actual_approval_and_new_process(self):
        self.case('loaded',probe_id='sdk-loop-command-probe')
    def test_loop_command_missing_approval_never_sends_a_load(self):
        self.case('loaded-missing-approval',probe_id='sdk-loop-command-probe')
    def test_raw_slip_module_uses_actual_approval_and_new_process_without_settings(self):
        self.case('loaded',probe_id='sdk-slip-capture')
    def test_raw_slip_missing_approval_never_sends_a_load(self):
        self.case('loaded-missing-approval',probe_id='sdk-slip-capture')
    def test_loaded_phase_uses_real_approval_one_apply_and_new_process(self):self.case('loaded')
    def test_reload_waits_for_fresh_empty_state_after_unknown_heartbeat(self):self.case('loaded',unknown_first=True)
    def test_unknown_or_loaded_heartbeat_never_authorizes_restart(self):
        clock=[0.]
        def advance(seconds):clock[0]+=30
        with patch.object(public,'time',SimpleNamespace(monotonic=lambda:clock[0],sleep=advance)):
            self.case('unknown-heartbeat')
        self.case('loaded-heartbeat')
    def test_loaded_phase_refuses_reused_pid(self):self.case('loaded-same-pid')
    def test_loaded_phase_refuses_reused_birth(self):self.case('loaded-same-birth')
    def test_loaded_phase_missing_approval_never_loads(self):self.case('loaded-missing-approval')
    def test_wave_only_phase_does_not_repeat_or_claim_settings_controls(self):self.case('wave-only',wave=True)
    def test_missing_wave_final_remains_incomplete_without_losing_independent_settings(self):self.case('wave-final-missing',wave=True)
    def test_invalid_observed_wave_payload_fails_before_setting_stimuli(self):self.case('wave-invalid-payload',wave=True)
    def test_wave_and_settings_share_normal_ui_apply_but_have_independent_public_proofs(self):self.case(wave=True)
    def test_missing_wave_actual_approval_stops_both_before_apply(self):self.case('missing-wave-approval',wave=True)
    def test_missing_wave_mapping_stops_before_setting_controls(self):self.case('missing-wave-map',wave=True)
    def test_actual_public_executor_wiring_consumes_one_real_ui_apply_then_uses_new_pid(self):self.case()
    def test_missing_actual_approval_stops_before_apply_or_settings(self):self.case('missing-approval')
    def test_stale_owner_stops_before_any_staging(self):self.case('stale-owner')
    def test_changed_owner_after_ui_stops_before_apply_or_settings(self):self.case('changed-owner')
    def test_changed_owner_at_apply_boundary_prevents_consumption_and_new_process(self):self.case('changed-before-apply')
    def test_reused_native_pid_stops_before_any_public_setting_input(self):self.case('same-pid')

if __name__=='__main__':unittest.main()
