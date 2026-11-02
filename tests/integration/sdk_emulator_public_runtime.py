#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Owned emulator normal-Allow/reload adapter and public setting acceptance.

The stock deployment watcher is outside these temporary SDK service units. This
adapter consumes an actual UI `apply` request and restarts only the nonce-owned
SDK unit, retaining its staging and actual UI-written trust records. It proves
normal mod-loader initialization after that restart, not USB deployment copying.
"""
from sdk_timestamps import compare_timestamps
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile
import time
import uuid

import sdk_emulator_sdk as runtime
from sdk_emulator_loaded import run_observed_runtime,native_birth_check_script
from sdk_emulator_controls import decode_observation,position_motion,MissingEvidence,StaleObservation
from sdk_emulator_public import (probe_archive,stage_script,read_script,approved,public_rows,
                                 PublicSettingChecks,run_public_phases,new_process)
from sdk_panel_transport import PanelTransport,PanelTransportSessions
from sdk_runtime_check import validate_health

APPROVED='/home/root/settings/azmod-community/approved'
PROBE='/home/root/settings/az-usbload/community/sdk-state-probe/sdk-state-probe.so'
LOG='/tmp/azmod-community/sdk-state-probe.log'
WAVE_PROBE='/home/root/settings/az-usbload/community/sdk-wave-layer-probe/sdk-wave-layer-probe.so'
WAVE_LOG='/tmp/azmod-community/sdk-wave-layer-probe.log'

def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run_public_runtime(ssh,package,source_root,output,firmware_sha256,seconds,command,context):
    PanelTransport._strict_ssh(ssh)
    if type(seconds) is not int or not 240<=seconds<=1500:raise ValueError('bounded two-phase public runtime required')
    if not callable(context.get('prepare_allow')):raise ValueError('normal UI Allow/reload callback required')
    token=context.get('token');container=context.get('container','');output=Path(output)
    if not isinstance(token,str) or not re.fullmatch('[a-f0-9]{32}',token) or not re.fullmatch('az-sdk-automation-[a-z0-9-]+',container):
        raise ValueError('one owned emulator identity required')
    if output.exists():raise ValueError('new public evidence output required')
    manifest=runtime.validate_package(package,source_root)
    binding=manifest['position_binding']
    if context.get('position_binding')!=binding:raise ValueError('bound native query-clock provider required')
    probe_manifest=Path(context['probe_manifest']);probe_binding=json.loads(probe_manifest.read_text())
    probe_id=context.get('probe_id','sdk-state-probe')
    if probe_id not in ('sdk-state-probe','sdk-slip-capture','sdk-loop-command-probe','sdk-cue-command-probe'):raise ValueError('reviewed normal probe id required')
    probe_path=PROBE.replace('sdk-state-probe',probe_id);probe_log=LOG.replace('sdk-state-probe',probe_id)
    files=probe_archive(Path(context['probe_package']).read_bytes(),probe_binding,mod_id=probe_id)
    if digest(Path(source_root)/'src/azmod/azmod-mod.h')!=probe_binding['sha256']['include/azmod-mod.h']:
        raise ValueError('public SDK header differs from the core closure')
    run_settings=context.get('run_settings',True)
    if type(run_settings) is not bool:raise ValueError('explicit settings execution required')
    loaded_executor=context.get('loaded_executor')
    if loaded_executor is not None and not callable(loaded_executor):raise ValueError('loaded runtime callback required')
    wave=context.get('wave_probe')
    if probe_id in ('sdk-slip-capture','sdk-loop-command-probe','sdk-cue-command-probe') and (run_settings or wave is not None or loaded_executor is None):raise ValueError('raw Slip/loop capture requires only the explicit loaded callback')
    if loaded_executor is not None and (run_settings or wave is not None):raise ValueError('separate loaded public scope required')
    if not run_settings and wave is None and loaded_executor is None:raise ValueError('wave-only phase requires a bound wave probe')
    wave_files=None
    if wave is not None:
        from sdk_emulator_wave_layer import wave_probe_archive
        if set(wave)!={'package','package_sha256','binary_sha256','profile','core_build_id','core_sha256','core_source_manifest_sha256','public_header_sha256'}:raise ValueError('exact bound wave configuration required')
        if wave['public_header_sha256']!=probe_binding['sha256']['include/azmod-mod.h'] or wave['core_sha256']!=manifest['sha256']['azmod.so'] or wave['core_build_id']!=manifest['bindings']['source']['build_id']:raise ValueError('wave core/header binding mismatch')
        wave_files=wave_probe_archive(Path(wave['package']).read_bytes(),package_sha256=wave['package_sha256'],binary_sha256=wave['binary_sha256'])
    driver=Path(context['driver_path']);receiver=Path(context['receiver_path'])
    if driver.is_symlink() or not driver.is_file() or digest(driver)!=context['driver_sha256']:
        raise ValueError('immutable actual panel driver required')
    if receiver.is_symlink() or not receiver.is_file() or digest(receiver)!=context['receiver_sha256']:
        raise ValueError('immutable native receiver required')
    if len(receiver.read_bytes())>1048576:raise ValueError('receiver byte cap exceeded')
    deadline=time.monotonic()+seconds
    def owner():
        if compare_timestamps(time.monotonic(), deadline, units_per_second=1) >= 0:raise RuntimeError('public overall deadline expired')
        value=command(['docker','inspect','--format','{{index .Config.Labels "az-sdk-token"}}',container],timeout=10)
        if len(value)>128 or value.decode().strip()!=token:raise ValueError('emulator ownership changed')
        return token
    owner();output.mkdir()
    frozen=output/'panel-driver.py';frozen.write_bytes(driver.read_bytes())
    def independent(pid,birth):
        owner();value=command(ssh+[native_birth_check_script(pid,birth)],timeout=5)
        if value not in (b'AZSDK_NATIVE_BIRTH_MATCH\n',b'AZSDK_NATIVE_BIRTH_REFUSED\n'):
            raise RuntimeError('independent native identity check unavailable')
        return value==b'AZSDK_NATIVE_BIRTH_MATCH\n'
    def bounded_read(identity,body,cap=262144):
        owner();data=command(ssh+[read_script(identity['pid'],identity['birth'],body,cap)],timeout=15)
        owner()
        if len(data)>cap:raise ValueError('public owned read byte cap exceeded')
        return data.decode(errors='strict')
    # Stage before the first SDK start, with normal-loader filenames and enabled
    # list. Approval is read here and subsequently written only by the native UI.
    pid=command(ssh+['pidof EP147 || true'],timeout=5).decode().strip()
    if not re.fullmatch('[1-9][0-9]*',pid):raise ValueError('one live stock baseline required before normal staging')
    stat=command(ssh+["awk '{s=$0;sub(/^[0-9]+ \\(.*\\) /,\"\",s);split(s,f,\" \");print f[20]}' /proc/"+pid+'/stat'],timeout=5).decode().strip()
    baseline=dict(pid=pid,birth=stat)
    if not independent(pid,stat):raise ValueError('baseline identity changed before staging')
    old_approval=bounded_read(baseline,'test ! -f '+APPROVED+' || cat '+APPROVED,32768)
    (output/'approval-before.txt').write_text(old_approval)
    resume=context.get('approval_resume')
    if resume is not None:
        if not isinstance(resume,dict) or not isinstance(resume.get('approval'),dict):
            raise ValueError('pending approval resume requires genuine UI proof')
        prior=resume['approval']
        if (wave is not None or resume.get('container')!=container or resume.get('token')!=token or
            prior.get('status')!='INCOMPLETE' or prior.get('errors') or prior.get('missing')!=['fresh empty-unit heartbeat'] or
            prior.get('health',{}).get('status')!='PASS' or
            any(prior.get(key)!=baseline[key] for key in ('pid','birth')) or
            prior.get('ui_allow') is not True or prior.get('ui_reload') is not True or
            prior.get('reload_request')!='apply' or prior.get('approved_sha256')!=probe_binding['binary']['sha256'] or
            prior.get('artifact_sha256')!=manifest['sha256'] or prior.get('position_binding')!=binding or
            not approved(old_approval,probe_id,probe_binding['binary']['sha256'])):
            raise ValueError('pending approval resume requires the same owned process and genuine UI proof')
        runtime._stage(prior.get('stage',''))
    elif any(line.split()[:1] in ([probe_id],['sdk-wave-layer-probe']) for line in old_approval.splitlines()):
        raise ValueError('probe already approved before the normal UI phase')
    def stage_probe(files,plan,label):
        payload=io.BytesIO()
        with tarfile.open(fileobj=payload,mode='w') as archive:
            for name,data in files.items():
                info=tarfile.TarInfo(name);info.size=len(data);info.mode=0o644;archive.addfile(info,io.BytesIO(data))
        owner()
        if not independent(pid,stat):raise ValueError('baseline identity changed before probe upload')
        (output/(label+'-stage.sh')).write_text(plan)
        uploaded=command(ssh+[plan],input=payload.getvalue(),timeout=20).decode()
        actual={line.split()[1].rsplit('/',1)[-1]:line.split()[0] for line in uploaded.splitlines() if len(line.split())==2}
        if actual!={n:hashlib.sha256(data).hexdigest() for n,data in files.items()}:raise ValueError('normal staged probe hashes mismatch')
        if not independent(pid,stat):raise ValueError('baseline identity changed during probe staging')
        (output/(label+'-stage-hashes.txt')).write_text(uploaded)
    if resume is None:stage_probe(files,stage_script(token,probe_id),'normal')
    if wave_files is not None:
        from sdk_emulator_wave_layer import wave_stage_script
        stage_probe(wave_files,wave_stage_script(token),'wave')
    probe_sha=probe_binding['binary']['sha256'];phase_meta={};stopped=None

    def run_phase(name):
        nonlocal stopped
        owner();phase=output/name
        if name=='approval' and resume is not None:
            # Recheck the still-pending, already observed UI action in place.
            # No trust records or installed files are rewritten on this path.
            prior=resume['approval']
            measured=runtime._identity(bounded_read(baseline,runtime.identity_script(baseline['pid'],prior['stage'])))
            if (measured.get('pid')!=baseline['pid'] or measured.get('firmware_sha256')!=firmware_sha256 or
                measured.get('mapped')!=manifest['sha256'] or set(measured.get('flags',[]))!=set(runtime.FLAGS)):
                raise ValueError('pending approval native artifacts or feature flags changed')
            raw=bounded_read(baseline,"cat /tmp/az-decks; printf '\\n__AZSDK_UPTIME__\\n'; cat /proc/uptime",8192)
            heartbeat,uptime=raw.split('\n__AZSDK_UPTIME__\n',1)
            snapshot=decode_observation(baseline['pid'],heartbeat,uptime.split()[0],'pid='+baseline['pid']+' exports=1,1\n')
            if snapshot['mask']!='EEEE' or measured.get('deck_mask')!='EEEE' or not measured.get('deck_heartbeat_fresh'):
                raise MissingEvidence('fresh empty-unit heartbeat')
            if probe_path in bounded_read(baseline,'cat /proc/'+baseline['pid']+'/maps'):
                raise ValueError('pending approval probe already ran')
            if bounded_read(baseline,'sha256sum '+probe_path,4096).split()!=[probe_sha,probe_path]:
                raise ValueError('pending approved probe binary changed')
            if 'allowed on the deck: sha256 '+probe_sha not in bounded_read(baseline,'cat '+probe_log):
                raise MissingEvidence('normal Allow log unavailable for pending approval')
            if bounded_read(baseline,'head -c 513 /run/az-community.req',1024)!='apply\n':
                raise MissingEvidence('actual UI apply request unavailable')
            phase_meta[name]=dict(baseline,stage=prior['stage'])
            return dict(prior,status='PASS',missing=[],resumed_pending_ui=True)
        if name=='loaded':
            remaining=int(deadline-time.monotonic())-120
            if loaded_executor is None or stopped is None or compare_timestamps(remaining,120,units_per_second=1)<0:raise MissingEvidence('loaded phase cannot fit the unchanged total deadline')
            pinned_loaded=[None]
            def verify_new(pid,birth):
                owner();identity=dict(pid=pid,birth=birth);new_process(stopped,identity)
                if not independent(pid,birth):raise ValueError('loaded native identity refused before controls')
                if pinned_loaded[0] is not None and pinned_loaded[0]!=identity:raise ValueError('loaded native identity changed')
                pinned_loaded[0]=identity
                return True
            result=loaded_executor(dict(stopped),remaining,verify_new)
            owner()
            if pinned_loaded[0] is None or pinned_loaded[0]!={k:result.get(k) for k in ('pid','birth')}:raise ValueError('loaded callback must pin the actual new process before controls')
            return result
        remaining=min(600,int(deadline-time.monotonic())-120)
        if compare_timestamps(remaining,120,units_per_second=1)<0:raise MissingEvidence('public phase cannot fit the unchanged total deadline')
        channel=[None];meta={};read_index=0
        def verify(pid,birth):
            return channel[0].verify_process(pid,birth) if channel[0] is not None else independent(pid,birth)
        def factory(pid,birth,guard):
            if name=='settings':new_process(stopped,dict(pid=pid,birth=birth))
            guard();stage='/tmp/az-sdk-panel.'+uuid.uuid4().hex
            command(ssh+['umask 077; mkdir '+stage+'; cat >'+stage+'/receiver; chmod 700 '+stage+'/receiver'],input=receiver.read_bytes(),timeout=15)
            meta.update(pid=pid,birth=birth,owner=token,stage=stage,receiver_sha256=context['receiver_sha256'])
            value=PanelTransportSessions(pid,birth,token,guard,independent,
                lambda label:PanelTransport(ssh,stage+'/receiver',context['receiver_sha256'],pid,birth,token,guard),max_sessions=1)
            channel[0]=value;value.renew('public-'+name,neutral=True)
            (phase/'panel-transport.json').write_text(json.dumps(meta,indent=2)+'\n')
            return value
        def read_guest(body,cap=262144):return bounded_read(meta,body,cap)
        def api_rows(controller):
            nonlocal read_index
            end=min(deadline,time.monotonic()+30)
            while True:
                read_index+=1
                if read_index>512:raise RuntimeError('public observation read limit reached')
                raw=read_guest('cat '+probe_log+"; printf '\\n__AZSDK_POSITION__\\n'; cat /tmp/az-sdk-position."+meta['pid']+
                               ".json; printf '\\n__AZSDK_HEARTBEAT__\\n'; cat /tmp/az-decks; printf '\\n__AZSDK_UPTIME__\\n'; cat /proc/uptime",524288)
                with (phase/('public-read-%04d.txt'%read_index)).open('x') as stream:stream.write(raw)
                log,rest=raw.split('\n__AZSDK_POSITION__\n',1);packet,rest=rest.split('\n__AZSDK_HEARTBEAT__\n',1)
                heartbeat,uptime=rest.split('\n__AZSDK_UPTIME__\n',1)
                positions=json.loads(packet)
                snapshot=decode_observation(meta['pid'],heartbeat,uptime.split()[0],'pid='+meta['pid']+' exports=1,1\n')
                if snapshot['mask']!='EEEE':raise ValueError('public unit-setting phase requires all decks empty')
                motion=position_motion(positions,1,meta['pid'],binding,now_boottime_ns=snapshot['boottime_ns'])
                if not motion.get('binding_verified') or not motion.get('fresh'):raise MissingEvidence('fresh bound public query clock unavailable')
                now=positions['native_monotonic_ns']+snapshot['boottime_ns']-positions['native_boottime_ns']
                try:return public_rows(log,now),now+10_000_001
                except MissingEvidence:
                    if compare_timestamps(time.monotonic(), end, units_per_second=1) >= 0:raise
                    time.sleep(.2)
        def action(controller):
            controller.timeout=30
            controller._release(controller.driver.BUS_ERP);controller._release(controller.driver.BUS_MIXER)
            if controller.observe()['mask']!='EEEE':raise ValueError('normal public phase requires empty native units')
            proof=dict(status='INCOMPLETE',pid=meta['pid'],birth=meta['birth'],scope='normal UI approval' if name=='approval' else 'public unit MT/Sync settings')
            try:
                if name=='approval':
                    ui=context['prepare_allow'](controller,read_guest,channel[0])
                    trust=read_guest('test ! -f '+APPROVED+' || cat '+APPROVED,32768)
                    (phase/'approval-after-ui.txt').write_text(trust)
                    if not approved(trust,probe_id,probe_sha):raise MissingEvidence('actual UI Allow hash unavailable')
                    if wave is not None:
                        from sdk_emulator_wave_layer import wave_approved
                        if not wave_approved(trust,wave['binary_sha256']):raise MissingEvidence('actual wave UI Allow hash unavailable')
                        wave_allow=read_guest('test ! -f '+WAVE_LOG+' || cat '+WAVE_LOG)
                        if 'allowed on the deck: sha256 '+wave['binary_sha256'] not in wave_allow:raise MissingEvidence('normal wave Allow log unavailable')
                        proof['approved_wave_sha256']=wave['binary_sha256']
                    maps=read_guest('cat /proc/'+meta['pid']+'/maps')
                    if probe_path in maps or (wave is not None and WAVE_PROBE in maps):raise ValueError('probe ran before the requested reload')
                    log=read_guest('test ! -f '+probe_log+' || cat '+probe_log)
                    if 'allowed on the deck: sha256 '+probe_sha not in log:raise MissingEvidence('normal Allow log unavailable')
                    request=read_guest('test ! -f /run/az-community.req || head -c 513 /run/az-community.req',1024)
                    if request!='apply\n':raise MissingEvidence('actual UI apply request unavailable')
                    proof.update(status='PASS',ui_allow=ui.get('ui_allow') is True,ui_reload=ui.get('ui_reload') is True,
                                 reload_request='apply',approved_sha256=probe_sha,ui_evidence=ui)
                else:
                    maps=read_guest('cat /proc/'+meta['pid']+'/maps')
                    if not any(line.split()[-1:]==[probe_path] for line in maps.splitlines()):raise MissingEvidence('normal-loaded public probe mapping unavailable')
                    measured=read_guest('sha256sum '+probe_path,4096).split()
                    if measured!=[probe_sha,probe_path]:raise ValueError('mapped public probe file hash mismatch')
                    log=read_guest('cat '+probe_log)
                    if 'loaded 0.1.0 on ' not in log or 'SDK_PUBLIC_STATE capabilities ' not in log:
                        raise MissingEvidence('normal loader/public service initialization unavailable')
                    proof.update(mapped_probe_sha256=probe_sha,checks={str(d):{} for d in range(1,5)},observation_setup=[],roundtrips=[])
                    if wave is not None:
                        from sdk_emulator_wave_layer import validate_wave_capture,MissingEvidence as WaveMissingEvidence
                        if not any(line.split()[-1:]==[WAVE_PROBE] for line in maps.splitlines()):raise MissingEvidence('normal-loaded wave probe mapping unavailable')
                        if read_guest('sha256sum '+WAVE_PROBE,4096).split()!=[wave['binary_sha256'],WAVE_PROBE]:raise ValueError('mapped wave probe file hash mismatch')
                        before_pin=dict(pid=meta['pid'],birth=meta['birth'])
                        if not independent(meta['pid'],meta['birth']):raise ValueError('wave native identity changed before capture')
                        raw=read_guest('cat '+WAVE_LOG+"; printf '\n__AZSDK_WAVE_POSITION__\n'; cat /tmp/az-sdk-position."+meta['pid']+".json; printf '\n__AZSDK_WAVE_UPTIME__\n'; cat /proc/uptime",65536)
                        (phase/'wave-read.txt').write_text(raw)
                        log,rest=raw.split('\n__AZSDK_WAVE_POSITION__\n',1)
                        packet,uptime=rest.split('\n__AZSDK_WAVE_UPTIME__\n',1);positions=json.loads(packet)
                        boot=int(float(uptime.split()[0])*1e9)
                        motion=position_motion(positions,1,meta['pid'],binding,now_boottime_ns=boot)
                        if not motion.get('binding_verified') or not motion.get('fresh'):raise MissingEvidence('fresh bound wave query clock unavailable')
                        now=positions['native_monotonic_ns']+boot-positions['native_boottime_ns']
                        if 'loaded 0.1.0 on ' not in log:raise MissingEvidence('normal wave loader initialization unavailable')
                        transcript=''.join(line+'\n' for line in log.splitlines() if line.startswith('SDK_PUBLIC_WAVE '))
                        expected={k:wave[k] for k in ('profile','core_build_id','core_sha256','core_source_manifest_sha256','public_header_sha256')}
                        expected.update(owner_nonce=token,mapped_probe_sha256=wave['binary_sha256'],probe_package_sha256=wave['package_sha256'],source_port=0)
                        if not independent(meta['pid'],meta['birth']):raise ValueError('wave native identity changed during capture')
                        captured=dict(expected,old_process=dict(stopped),new_process=before_pin,pin_before=before_pin,pin_after=dict(before_pin),old_absent=True,
                                      approval_origin='actual_ui',approved_probe_sha256=wave['binary_sha256'],apply_count=1,loader='normal_sdk_entry_library_service',capture_monotonic_ns=now)
                        (phase/'wave-context.json').write_text(json.dumps(dict(context=captured,expected=expected),indent=2)+'\n')
                        try:proof['wave']=validate_wave_capture(transcript,captured,expected)
                        except WaveMissingEvidence as error:proof['wave']=dict(status='INCOMPLETE',missing=[str(error)])
                        except ValueError as error:proof['wave']=dict(status='FAIL',errors=[str(error)])
                        (phase/'wave-proof.json').write_text(json.dumps(proof['wave'],indent=2)+'\n')
                        if proof['wave']['status']=='FAIL':raise MissingEvidence('wave validation failed; settings stimuli not run')
                    if not run_settings:
                        proof.update(status='PASS',settings_execution='NOT_RUN',checks={},scope='public waveform only; settings controls not run')
                        return proof
                    checks=PublicSettingChecks(controller,lambda:api_rows(controller))
                    for deck in range(1,5):
                        for stream in ('mt','sync'):
                            proof['observation_setup'].append(checks.establish(deck,stream))
                            result=checks.roundtrip(deck,stream);proof['roundtrips'].append(result)
                            proof['checks'][str(deck)][stream]='PASS'
                    proof['status']='PASS'
            except MissingEvidence as error:proof['missing']=[str(error)]
            except (RuntimeError,ValueError,OSError) as error:proof.update(status='FAIL',errors=[str(error)])
            finally:
                for bus in (controller.driver.BUS_ERP,controller.driver.BUS_MIXER):
                    try:controller._release(bus)
                    except (RuntimeError,ValueError,OSError) as error:proof.update(status='FAIL',cleanup_error=str(error))
                proof['events']=controller.events
                (phase/'public-controls.json').write_text(json.dumps(proof,indent=2)+'\n')
            return proof
        def validate(loop,source,health,identity,manifest,firmware,controls,positions,activation,owned,boottime):
            report=dict(controls);errors=list(report.get('errors',[]));missing=list(report.get('missing',[]))
            status=validate_health(health);report['health']=status
            if status['status']=='FAIL':errors+=status['errors']
            elif status['status']!='PASS' or len(health)<2 or compare_timestamps(health[-1]['local_monotonic'] - health[0]['local_monotonic'], 120, units_per_second=1) < 0:missing.append('120-second process health')
            if identity.get('pid')!=meta.get('pid') or identity.get('firmware_sha256')!=firmware:errors.append('public native process/firmware mismatch')
            for file,sha in manifest['sha256'].items():
                if identity.get('mapped',{}).get(file)!=sha:errors.append('mapped core artifact mismatch '+file)
            if set(identity.get('flags',[]))!=set(runtime.FLAGS):errors.append('public runtime feature flags mismatch')
            if identity.get('deck_mask')!='EEEE' or not identity.get('deck_heartbeat_fresh'):missing.append('fresh empty-unit heartbeat')
            if any(int(runtime._fields(line).get('invalid','0')) for line in source.splitlines()):errors.append('source helper invalid record')
            report.update(status='FAIL' if errors else 'INCOMPLETE' if missing or report.get('status')!='PASS' else 'PASS',errors=errors,missing=missing)
            return report
        owned=dict(context,driver_path=frozen,position_binding=binding,panel_transport_factory=factory,verify_process=verify)
        if stopped is not None:owned['stopped_identity']=dict(stopped)
        result=run_observed_runtime(ssh,package,source_root,phase,firmware_sha256,remaining,command,owned,
                                    execute_controls=action,validate_observed=validate)
        phase_meta[name]=dict(meta,stage=result.get('stage'))
        return result

    def restart(before):
        nonlocal stopped
        meta=phase_meta['approval']
        if (before['pid'],before['birth'])!=(meta['pid'],meta['birth']):raise ValueError('reload evidence identity mismatch')
        owner()
        empty_deadline=min(deadline,time.monotonic()+30)
        while True:
            raw=bounded_read(meta,"cat /tmp/az-decks; printf '\\n__AZSDK_UPTIME__\\n'; cat /proc/uptime",8192)
            heartbeat,uptime=raw.split('\n__AZSDK_UPTIME__\n',1)
            try:
                snapshot=decode_observation(meta['pid'],heartbeat,uptime.split()[0],'pid='+meta['pid']+' exports=1,1\n')
            except MissingEvidence:
                if compare_timestamps(time.monotonic(), empty_deadline, units_per_second=1) >= 0:raise
                time.sleep(.2)
                continue
            if snapshot['mask']!='EEEE':raise MissingEvidence('loaded units refuse owned public restart')
            break
        if not independent(meta['pid'],meta['birth']):raise ValueError('native changed before requested reload')
        guest='/tmp/az-sdk-public.'+token
        plan=('set -e\n'+runtime.mask_guard()+'test -f /run/az-community.req\ntest ! -L /run/az-community.req\n'
              'test "$(head -c 513 /run/az-community.req)" = apply\nmkdir '+guest+'\n'
              'mv /run/az-community.req '+guest+'/ui-apply.consumed\nsystemctl stop az-sdk-runtime.service\n')
        (output/'owned-ui-reload-adapter.sh').write_text(plan)
        owner();command(ssh+[plan],timeout=15)
        if independent(meta['pid'],meta['birth']):raise ValueError('requested SDK process still alive after stop')
        if command(ssh+['pidof EP147 || true'],timeout=5).strip():raise ValueError('unexpected native process appeared during reload')
        stopped={k:meta[k] for k in ('pid','birth')}
        (output/'reload-transition.json').write_text(json.dumps(dict(before=stopped,request='actual native UI apply',
            adapter='owned temporary SDK service; original EP147 remains masked; no USB deployment-copy claim',native_stopped=True),indent=2)+'\n')
    report=run_public_phases(run_phase,restart,probe_sha,require_settings=run_settings,loaded=loaded_executor is not None)
    if wave is not None:
        wave_result=report.get('settings',{}).get('wave',{'status':'INCOMPLETE','missing':['actual normal-loaded waveform proof unavailable']})
        report['wave']=wave_result
        report['scope']+='; separate public raw3/derived8 waveform service proof'
        if wave_result['status']=='FAIL':report['status']='FAIL'
        elif wave_result['status']!='PASS' and report['status']=='PASS':report['status']='INCOMPLETE'
    report.update(probe_manifest_sha256=digest(probe_manifest),probe_package_sha256=probe_binding['package']['sha256'],
                  probe_binary_sha256=probe_sha,total_limit_seconds=seconds,original_approval_records_edited_by_adapter=False)
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
