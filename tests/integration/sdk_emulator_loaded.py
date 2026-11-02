#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Loaded scalar/control acceptance inside an already nonce-owned emulator.

No CLI and no global emulator transport. The outer smoke runner owns the VM,
baseline and cleanup. GUI track selection and bounded panel transport are
explicit injected callbacks. Unsupported SDK capabilities retain their own
incomplete acceptance fields and are outside this scalar/control result.
"""

from sdk_timestamps import compare_timestamps
import io
import json
from pathlib import Path
import re
import socket
import subprocess
import tarfile
import time
import uuid

import sdk_emulator_sdk as runtime
from sdk_emulator_controls import Controller, MissingEvidence, StaleObservation, decode_observation, position_motion
from sdk_runtime_check import validate_health, validate_transcript

SCOPE='all four loaded emulator decks: observed scalar receipts and panel controls'
CHECKS=('loaded_heartbeat','loaded_title','raw_play_notifications','play_pause',
        'manual_loop_paused_exit_reloop','key_lock','quantize_state')
EXCLUDED=('source_binding_acceptance','native_commands_acceptance','beatgrid_acceptance',
          'persistence_acceptance','audio_acceptance')

def native_birth_check_script(pid,birth):
    """Bounded, read-only identity check independent of the panel receiver."""
    if not isinstance(pid,str) or not re.fullmatch('[1-9][0-9]{0,9}',pid) or int(pid)>2147483647:
        raise ValueError('one pinned PID required')
    if not isinstance(birth,str) or not re.fullmatch('[1-9][0-9]{0,19}',birth) or int(birth)>=2**64:
        raise ValueError('one pinned process birth required')
    # comm may contain spaces/parentheses. Strip through its final closing
    # delimiter before numbering the remaining stat fields (birth is field20).
    return '''AZSDK_NATIVE_BIRTH_CHECK=1
check_native_birth() {
    test -r /proc/@PID@/stat || return 1
    azsdk_stat=$(head -c 8193 /proc/@PID@/stat) || exit 2
    test "${#azsdk_stat}" -le 8192 || return 1
    printf '%s\\n' "$azsdk_stat" | awk -v p=@PID@ -v b=@BIRTH@ '
        NR==1 { original=$0; sub(/^[0-9]+ \\(.*\\) /,"",$0);
                n=split($0,f," "); split(original,h," ");
                good=(("x" h[1])==("x" p) && $0!=original && n>=20 && f[1] !~ /^[ZXx]$/ && ("x" f[20])==("x" b)) }
        END { exit !(NR==1 && good) }'
}
if check_native_birth && check_native_birth; then
    printf 'AZSDK_NATIVE_BIRTH_MATCH\\n'
else
    printf 'AZSDK_NATIVE_BIRTH_REFUSED\\n'
fi
'''.replace('@PID@',pid).replace('@BIRTH@',birth)

def controls_ready(snapshot,journal,binding):
    """Readiness needs actual main dispatch, all slots and adjacent live clocks."""
    if 'ep122_startup: main dispatch ready; PreviewMixer released' not in journal:return False
    try:
        value=decode_observation(**snapshot)
        for key in ['registration_mask','live_registration_mask']:
            rows=runtime._rows(snapshot['loop_log'],key+'=')
            if not rows or rows[-1].get(key)!='15':return False
        for deck in range(1,5):
            result=position_motion(value['positions'],deck,value['pid'],binding,now_boottime_ns=value['boottime_ns'])
            if not result.get('binding_verified') or not result.get('fresh'):return False
        return True
    except (KeyError,ValueError,TypeError,IndexError,OverflowError):return False

def validate_loaded_package(package,source_root,position_binding):
    manifest=runtime.validate_package(package,source_root)
    binding=manifest.get('position_binding')
    if not isinstance(binding,dict) or binding!=position_binding:
        raise ValueError('loaded runtime position binding mismatch')
    if set(binding)!={'provider_build_id','header_sha256','engine_rate','abi_size'}:
        raise ValueError('exact position provider/header/clock/ABI binding required')
    if binding['provider_build_id']!=runtime._build_id((Path(package)/'azstem.so').read_bytes()):
        raise ValueError('position provider build ID mismatch')
    if binding['engine_rate']!=44100 or type(binding['engine_rate']) is not int or binding['abi_size']!=296 or type(binding['abi_size']) is not int:
        raise ValueError('reviewed position clock 44100 and ABI 296 required')
    if binding['header_sha256']!=manifest['source_sha256'].get('src/common/azdeckinfo.h'):
        raise ValueError('position header is not bound to compiled source')
    if 'tests/integration/sdk_position_probe.c' not in manifest['source_sha256']:
        raise ValueError('position publisher compiled source binding required')
    if manifest['loop_operator'] is not True:raise ValueError('active operator capture required for loaded controls')
    return manifest

def validate_loaded_evidence(loop,source,health,identity,manifest,firmware_sha256,controller,positions,activation_ns,context,boottime_ns):
    errors=[];missing=[]
    report=dict(scope=SCOPE,**{name:'INCOMPLETE' for name in EXCLUDED})
    report['health']=validate_health(health)
    if report['health']['status']=='FAIL':errors.extend(report['health']['errors'])
    elif report['health']['status']!='PASS':missing.append('runtime health')
    if report['health']['status']=='PASS':
        if len(health)<2 or compare_timestamps(health[-1].get('local_monotonic', 0) - health[0].get('local_monotonic', 0), 120, units_per_second=1) < 0:
            missing.append('120-second runtime observation')
    pid=health[0].get('pid','') if health else ''
    for key,expected,label in [('pid',pid,'mapped process identity'),('firmware_sha256',firmware_sha256,'mapped firmware')]:
        if identity.get(key) is None:missing.append(label)
        elif identity[key]!=expected:errors.append(label+' mismatch')
    for name,sha in manifest['sha256'].items():
        if name not in identity.get('mapped',{}):missing.append('mapped artifact '+name)
        elif identity['mapped'][name]!=sha:errors.append('mapped artifact hash mismatch '+name)
    if 'flags' not in identity:missing.append('feature flags')
    elif set(identity['flags'])!=set(runtime.FLAGS):errors.append('runtime feature flags mismatch')
    if identity.get('deck_mask')!='LLLL':missing.append('four observed loaded decks')
    if not identity.get('deck_heartbeat_fresh'):missing.append('fresh deck heartbeat')
    if len(loop.encode())>262144 or len(source.encode())>65536:
        errors.append('probe byte cap exceeded');loop=source=''
    transcript=validate_transcript(loop,(1,2,3,4));report['transcript']=transcript
    if transcript['status']=='FAIL':errors.extend(transcript['errors'])
    try:
        if type(activation_ns) is not int or not 0<=activation_ns<2**64:raise ValueError('invalid native activation receipt barrier')
        if not activation_ns:missing.append('native activation receipt barrier')
        headers=runtime._rows(loop,'pid=');source_headers=runtime._rows(source,'pid=')
        if len(headers)!=1 or headers[0].get('exports')!='1,1':missing.append('current loop exports')
        if len(source_headers)!=1:missing.append('source probe header')
        for row in headers+source_headers:
            if row.get('pid')!=pid:errors.append('probe PID mismatch')
        for row in source_headers:
            if int(row.get('nice_result','-1'))!=0:errors.append('source probe cadence failed')
        for line,label in [('live_exports=1,1','current live exports'),('private_load_export=1','current private load ABI'),('diagnostic_available=1','current matched loop helper')]:
            if line not in loop.splitlines():missing.append(label)
        for key in ['registration_mask','live_registration_mask']:
            rows=runtime._rows(loop,key+'=')
            if not rows or int(rows[-1][key])!=15:missing.append('all four '+key)
            if any(int(row[key]) & ~15 for row in rows):errors.append('invalid registration mask')
        seen=set()
        for prefix in ['deck=','live_deck=']:
            for row in runtime._rows(loop,prefix):
                deck=int(row.get('deck',row.get('live_deck','-1')))
                stream='loop' if prefix=='deck=' else row.get('stream')
                received=int(row.get('received_ns','0'))
                if not 0<=deck<=3 or not 0<=received<2**64:raise ValueError('invalid deck receipt')
                if prefix=='live_deck=' and not (compare_timestamps(0, int(row.get('age_ns', '-1'))) <= 0 and compare_timestamps(int(row.get('age_ns', '-1')), 5000000000) <= 0):
                    missing.append('fresh live receipt')
                elif activation_ns and compare_timestamps(received, activation_ns) > 0:seen.add((deck,stream))
        report['post_activation_streams']={str(d+1):sorted(s for index,s in seen if index==d) for d in range(4)}
        for deck in range(4):
            for stream in ['loop','play','quantize','key_lock']:
                if (deck,stream) not in seen:missing.append('deck%d post-activation %s receipt'%(deck+1,stream))
        footers=runtime._rows(loop,'complete ')
        if len(footers)!=1:missing.append('loop completion footer')
        for row in footers:
            if int(row.get('errors','-1'))!=0:errors.append('loop probe reported errors')
            if any(row.get(key)!='15' for key in ['seen','submitted','live_seen','live_submitted']):missing.append('all four completed loop/live slots')
        resolvers=runtime._rows(source,'resolver_available=')
        if len(resolvers)!=1 or resolvers[0].get('resolver_available')!='1':missing.append('matched source helper')
        elif resolvers[0].get('build_id')!=manifest['bindings']['source']['build_id']:errors.append('source helper build ID mismatch')
        for line in source.splitlines():
            row=runtime._fields(line)
            if int(row.get('invalid','0')):errors.append('source probe reported invalid records')
            if int(row.get('history_limit','0')):missing.append('source scan history limit')
        footers=runtime._rows(source,'complete ')
        if len(footers)!=1:missing.append('source completion footer')
        for row in footers:
            if row.get('status')=='resolver_refused':missing.append('source resolver refused experimental ABI')
            elif row.get('status')!='time_limit':errors.append('source probe failed: '+str(row.get('status')))
            elif int(row.get('passes','0'))<1:missing.append('complete source-ID scan')
        if not isinstance(controller,dict):raise ValueError('controller report mapping required')
        report['controls_status']=controller.get('status','INCOMPLETE')
        if controller.get('status')=='FAIL' or controller.get('errors'):errors.append('loaded controller reported failure')
        elif controller.get('status')!='PASS' or controller.get('missing'):missing.append('loaded controller completion')
        for key,expected in [('owner_nonce',context['token']),('pid',pid),('position_binding',context['position_binding'])]:
            if controller.get(key)!=expected:
                if controller.get('status')=='PASS' or key in controller:errors.append('controller '+key+' mismatch')
                else:missing.append('controller '+key)
        if controller.get('panel_source',{}).get('sha256')!=context['driver_sha256']:
            if controller.get('status')=='PASS':errors.append('controller panel source mismatch')
            else:missing.append('controller panel source')
        for deck in range(1,5):
            checks=controller.get('decks',{}).get(str(deck),{})
            for key in CHECKS:
                if checks.get(key)!='PASS':
                    (errors if controller.get('status')=='PASS' else missing).append('controller deck%d %s'%(deck,key))
        selection=controller.get('selection',{})
        if selection.get('status')!='PASS' or selection.get('selected_title') not in context['expected_titles'] or not selection.get('evidence'):
            missing.append('independent selected-track evidence')
        events=controller.get('events',[])
        selections=controller.get('load_selections',{})
        if not isinstance(selections,dict):raise ValueError('per-deck selection mapping required')
        seen_selection_evidence=set()
        for deck in range(1,5):
            selected=selections.get(str(deck),{})
            evidence=selected.get('evidence') if isinstance(selected,dict) else None
            if (not isinstance(selected,dict) or selected.get('status')!='PASS' or
                selected.get('selected_title') not in context['expected_titles'] or
                not isinstance(evidence,str) or not evidence or evidence in seen_selection_evidence):
                (errors if controller.get('status')=='PASS' else missing).append('deck%d current track selection UI evidence'%deck)
            else:
                seen_selection_evidence.add(evidence)
            observed=[i for i,row in enumerate(events) if row.get('deck')==deck and row.get('action')=='selection_observed' and row.get('evidence')==evidence]
            loaded=[i for i,row in enumerate(events) if row.get('deck')==deck and row.get('action')=='load']
            if len(observed)!=1 or len(loaded)!=1 or observed[0]>=loaded[0]:
                (errors if controller.get('status')=='PASS' else missing).append('deck%d independent selection before one LOAD'%deck)
        if controller.get('status')=='PASS':
            for deck in range(1,5):
                for action in ['load','play','loopin','loopout','reloop','mastertempo']:
                    if not any(row.get('deck')==deck and row.get('action')==action for row in events):errors.append('controller missing deck%d %s input'%(deck,action))
            if sum(row.get('action')=='quantize' for row in events)<2:errors.append('controller missing quantize roundtrip inputs')
        maximum=context.get('max_source_rate')
        if maximum is not None and (not isinstance(context.get('rate_bound_source'),str) or not context['rate_bound_source']):
            raise ValueError('source-rate bound needs independent provenance')
        latest_loops={int(row['deck']):row for row in runtime._rows(loop,'deck=')}
        for deck in range(1,5):
            motion=position_motion(positions,deck,pid,context['position_binding'],after_ns=activation_ns,
                loop=latest_loops.get(deck-1),max_source_rate=maximum,now_boottime_ns=boottime_ns)
            if not motion.get('fresh') or motion.get('state')=='UNKNOWN':missing.append('deck%d fresh qualified position history'%deck)
            if positions and positions.get('samples'):
                title=positions['samples'][-1]['decks'][deck-1].get('title')
                if not title:missing.append('deck%d loaded title witness'%deck)
                elif title not in context['expected_titles']:errors.append('unexpected loaded title on deck%d'%deck)
    except (KeyError,ValueError,TypeError,IndexError,OverflowError) as failure:
        errors.append('malformed loaded evidence: '+str(failure))
    report.update(status='FAIL' if errors else 'INCOMPLETE' if missing else 'PASS',errors=sorted(set(errors)),missing=sorted(set(missing)))
    return report

def run_loaded_runtime(ssh,package,source_root,output,firmware_sha256,seconds,command,context):
    return run_observed_runtime(ssh,package,source_root,output,firmware_sha256,seconds,command,context)

def run_observed_runtime(ssh,package,source_root,output,firmware_sha256,seconds,command,context,
                         execute_controls=None,validate_observed=None):
    """Compose the tested runtime lifecycle with loaded-only observations.

    Required context: container, token, work_dir, qmp_port, driver_path,
    driver_sha256, position_binding, expected_titles, prepare_track and either
    send_control(payload) or panel_transport_factory(pid,birth,guard). Every
    injected I/O callback must be bounded. The factory may return a callable or
    an object with send(payload)/close(), allowing one persistent transport.
    Optional verify_process(pid,birth) checks the pinned birth on that guest.
    Refusal or a closed channel triggers an independent read-only proc check;
    this never replaces or reopens the stateful sender.
    Optional public_probe_sha256 requests a bounded post-control public capture
    from that already approved client, before runtime cleanup.
    Optional retain_operator keeps the diagnostic observer alive for subsequent
    owned cases. Its unfinished capture cannot qualify scalar completion.
    """
    if not ssh or ssh[-1]!='root@127.0.0.1':raise ValueError('owned loopback emulator SSH required')
    if type(seconds) is not int or not 120<=seconds<=1800:raise ValueError('runtime duration must be 120..1800 seconds')
    if not isinstance(context,dict):raise ValueError('owned loaded runtime context required')
    retain_operator=context.get('retain_operator',False)
    if type(retain_operator) is not bool:raise ValueError('boolean retain_operator required')
    custom=execute_controls is not None or validate_observed is not None
    if custom and not all(callable(f) for f in (execute_controls,validate_observed)):
        raise ValueError('separate observed action and evidence validator required')
    token=context.get('token');container=context.get('container','');output=Path(output)
    if not isinstance(token,str) or not re.fullmatch('[a-f0-9]{32}',token) or not re.fullmatch('az-sdk-automation-[a-z0-9-]+',container):
        raise ValueError('owned emulator identity required')
    if type(context.get('qmp_port')) is not int or not 1<=context['qmp_port']<=65535:raise ValueError('owned QMP port required')
    work=Path(context['work_dir'])
    if work.is_symlink() or not work.is_dir():raise ValueError('owned QEMU work directory required')
    if (not custom and not callable(context.get('prepare_track'))) or not (callable(context.get('send_control')) or callable(context.get('panel_transport_factory'))):
        raise ValueError('bounded track-selection and panel-transport callbacks required')
    if context.get('prepare_deck_load') is not None and not callable(context['prepare_deck_load']):
        raise ValueError('bounded per-deck track-selection callback required')
    if context.get('prepare_controls') is not None and not callable(context['prepare_controls']):
        raise ValueError('bounded explicit observation setup callback required')
    probe_sha=context.get('public_probe_sha256')
    if probe_sha is not None and (not isinstance(probe_sha,str) or not re.fullmatch('[0-9a-f]{64}',probe_sha)):
        raise ValueError('exact normal-loader public client SHA-256 required')
    titles=context.get('expected_titles')
    if not custom and (not isinstance(titles,(list,tuple)) or not 1<=len(titles)<=8 or any(not isinstance(v,str) or not 1<=len(v)<=512 for v in titles)):
        raise ValueError('exact expected title aliases required')
    driver=Path(context['driver_path'])
    if driver.is_symlink() or not driver.is_file() or driver.stat().st_size>131072 or runtime.digest(driver)!=context.get('driver_sha256'):
        raise ValueError('panel source identity mismatch')
    manifest=validate_loaded_package(package,source_root,context.get('position_binding'))
    deadline=time.monotonic()+seconds+120
    pinned={'pid':'','birth':''};controller_report={};positions=None;boottime_ns=0;activation_ns=0
    observations=0;control_started=False;transport=None;identity_fallbacks=0
    post_control_public=None

    def owner():
        if compare_timestamps(time.monotonic(), deadline, units_per_second=1) > 0:raise RuntimeError('loaded runtime overall deadline expired')
        value=command(['docker','inspect','--format','{{index .Config.Labels "az-sdk-token"}}',container],timeout=10)
        if len(value)>128 or value.decode().strip()!=token:raise ValueError('emulator ownership changed')
        return token

    def birth_guard():
        nonlocal identity_fallbacks
        owner()
        if not pinned['birth']:return
        checker=context.get('verify_process')
        if checker is not None:
            if not callable(checker):raise ValueError('bounded process checker required')
            try:matched=checker(pinned['pid'],pinned['birth']);reason='checker-refused'
            except (RuntimeError,OSError,subprocess.TimeoutExpired) as failure:
                matched=False;reason=type(failure).__name__
            if matched is True:return
            # A receiver CHECK can expire while EP147 remains alive. Do not
            # turn a channel failure into an identity assertion or reactivate
            # the failed sender. Check the native process independently.
            text=command(ssh+[native_birth_check_script(pinned['pid'],pinned['birth'])],timeout=5)
            if len(text)>4096:raise ValueError('native birth check byte cap exceeded')
            if text not in (b'AZSDK_NATIVE_BIRTH_MATCH\n',b'AZSDK_NATIVE_BIRTH_REFUSED\n'):
                raise RuntimeError('independent native birth check unavailable')
            identity_fallbacks+=1
            if identity_fallbacks<=64:
                with (output/'process-identity-fallback.jsonl').open('a') as stream:
                    stream.write(json.dumps({'pid':pinned['pid'],'birth':pinned['birth'],'matched':text==b'AZSDK_NATIVE_BIRTH_MATCH\n',
                                             'reason':reason,'check':'independent proc stat'})+'\n')
            if text!=b'AZSDK_NATIVE_BIRTH_MATCH\n':raise ValueError('pinned emulator process identity refused by independent check')
        else:
            # Preserve the independent path even when no fast CHECK exists.
            text="test -r /proc/%s/stat && test \"$(awk '{print $22}' /proc/%s/stat)\" = %s"%(pinned['pid'],pinned['pid'],pinned['birth'])
            command(ssh+[text],timeout=10)

    def read(text,cap=524288):
        birth_guard();data=command(ssh+[text],timeout=15)
        if len(data)>cap:raise ValueError('loaded observation byte cap exceeded')
        return data.decode(errors='replace')

    def observation(reader=read):
        nonlocal observations,positions,boottime_ns
        pid=pinned['pid']
        text=reader("cat /tmp/az-decks; printf '\\n__AZSDK_LOOP__\\n'; test ! -f /tmp/az-sdk-loop-probe."+pid+'.log || cat /tmp/az-sdk-loop-probe.'+pid+".log; printf '\\n__AZSDK_POSITION__\\n'; test ! -f /tmp/az-sdk-position."+pid+'.json || cat /tmp/az-sdk-position.'+pid+".json; printf '\\n__AZSDK_UPTIME__\\n'; cat /proc/uptime")
        heartbeat,rest=text.split('\n__AZSDK_LOOP__\n',1)
        loop,rest=rest.split('\n__AZSDK_POSITION__\n',1)
        packet,uptime=rest.split('\n__AZSDK_UPTIME__\n',1)
        if len(packet.encode())>131072:raise ValueError('position publisher byte cap exceeded')
        positions=json.loads(packet) if packet.strip() else None
        boottime_ns=int(float(uptime.split()[0])*1000000000)
        value={'pid':pid,'uptime':uptime.split()[0],'heartbeat':heartbeat.strip()+'\n','loop_log':loop,'positions':positions}
        observations+=1
        if observations>2048:raise ValueError('loaded observation count cap exceeded')
        (output/('observation-%04d.json'%observations)).write_text(json.dumps(value,indent=2)+'\n')
        return value

    def capture_public_after_controls():
        # The parser imports our birth-check script, hence this lazy import.
        # This path cannot reopen a receiver or send an input.
        from sdk_emulator_public import read_script,native_rows,confirmed_master
        result=dict(status='INCOMPLETE',pid=pinned['pid'],birth=pinned['birth'],
                    scope='post-control public native settings; no source, pause or command authorization')
        end=min(deadline,time.monotonic()+30)
        def remaining():
            left=end-time.monotonic()
            if compare_timestamps(left,0.,units_per_second=1)<=0:raise MissingEvidence('post-control public capture deadline expired')
            return left
        def bounded(argv,cap,limit):
            value=command(argv,timeout=min(limit,remaining()))
            remaining()  # A read completed at/after the deadline cannot qualify.
            if len(value)>cap:raise ValueError('post-control public read byte cap exceeded')
            return value.decode(errors='strict')
        def check_owner():
            value=bounded(['docker','inspect','--format','{{index .Config.Labels "az-sdk-token"}}',container],128,10)
            if value.strip()!=token:raise ValueError('post-control emulator ownership changed')
        def read_owned(body,cap=524288):
            check_owner()
            raw=bounded(ssh+[read_script(pinned['pid'],pinned['birth'],body,cap)],cap,15)
            check_owner()
            return raw
        target='/home/root/settings/az-usbload/community/sdk-state-probe/sdk-state-probe.so'
        module=('sha256sum '+target+'; awk \'$NF=="'+target+'" {print}\' /proc/'+pinned['pid']+'/maps')
        body=("printf '__AZSDK_MODULE_BEFORE__\\n'; "+module+
              "; printf '\\n__AZSDK_LOG__\\n'; cat /tmp/azmod-community/sdk-state-probe.log; "
              "printf '\\n__AZSDK_POSITION__\\n'; cat /tmp/az-sdk-position."+pinned['pid']+".json; "
              "printf '\\n__AZSDK_HEARTBEAT__\\n'; cat /tmp/az-decks; "
              "printf '\\n__AZSDK_UPTIME__\\n'; cat /proc/uptime; "
              "printf '\\n__AZSDK_MODULE_AFTER__\\n'; "+module)
        try:
            remaining()
            snap=decode_observation(**observation(read_owned))
            proof=position_motion(snap['positions'],1,pinned['pid'],manifest['position_binding'],now_boottime_ns=snap['boottime_ns'])
            if not proof.get('binding_verified') or not proof.get('fresh'):raise MissingEvidence('fresh bound post-control native clock unavailable')
            fence=snap['positions']['native_monotonic_ns']+snap['boottime_ns']-snap['positions']['native_boottime_ns']
            result['after_ns']=fence
            for _ in range(151):
                raw=read_owned(body,655360)
                (output/'public-native-postcontrols.txt').write_text(raw)
                before,rest=raw.split('\n__AZSDK_LOG__\n',1)
                log,rest=rest.split('\n__AZSDK_POSITION__\n',1)
                packet,rest=rest.split('\n__AZSDK_HEARTBEAT__\n',1)
                heartbeat,rest=rest.split('\n__AZSDK_UPTIME__\n',1)
                uptime,after=rest.split('\n__AZSDK_MODULE_AFTER__\n',1)
                if not before.startswith('__AZSDK_MODULE_BEFORE__\n'):raise ValueError('public module capture marker missing')
                before=before.split('\n',1)[1].strip();after=after.strip()
                if before!=after:raise ValueError('public client module mapping changed during capture')
                lines=before.splitlines()
                if not lines or lines[0].split()!=[probe_sha,target]:raise ValueError('normal public client hash mismatch')
                if not any(len(line.split())==6 and line.split()[1]=='r-xp' and line.split()[-1]==target for line in lines[1:]):
                    raise ValueError('normal public client executable mapping unavailable')
                positions=json.loads(packet)
                snap=decode_observation(pinned['pid'],heartbeat,uptime.split()[0],'pid='+pinned['pid']+' exports=1,1\n')
                proof=position_motion(positions,1,pinned['pid'],manifest['position_binding'],now_boottime_ns=snap['boottime_ns'])
                if not proof.get('binding_verified') or not proof.get('fresh'):raise MissingEvidence('fresh bound public query clock unavailable')
                now=positions['native_monotonic_ns']+snap['boottime_ns']-positions['native_boottime_ns']
                try:
                    rows=native_rows(log,now)
                    if any(compare_timestamps(row['query_ns'], fence) <= 0 for row in rows.values()):raise MissingEvidence('public queries do not follow controls')
                except (MissingEvidence,StaleObservation):
                    time.sleep(min(.2,remaining()));continue
                result.update(rows=rows,mapped_probe_sha256=probe_sha,
                              tempo={str(d):'PASS' if rows[d]['tempo_valid'] else 'INCOMPLETE' for d in rows},
                              slider={str(d):'PASS' if rows[d]['slider_valid'] else 'INCOMPLETE' for d in rows})
                try:
                    deck=confirmed_master(rows)
                except MissingEvidence as failure:
                    result['master']=dict(status='INCOMPLETE',missing=[str(failure)])
                else:
                    result.update(status='PASS',master=dict(status='PASS',deck=deck,revision=rows[deck]['master_rev'],observed_ns=rows[deck]['master_ns']))
                return result
            raise MissingEvidence('post-control public capture attempt limit')
        except (MissingEvidence,StaleObservation) as failure:result['missing']=[str(failure)]
        except (ValueError,KeyError,IndexError,TypeError,UnicodeError) as failure:result.update(status='FAIL',errors=[str(failure)])
        except subprocess.CalledProcessError as failure:
            result.update(status='FAIL' if failure.returncode==93 else 'INCOMPLETE',errors=['public read refused: exit '+str(failure.returncode)])
        except (OSError,RuntimeError,subprocess.TimeoutExpired) as failure:result['missing']=[str(failure)]
        return result

    def qmp_execute(request):
        birth_guard()
        if callable(context.get('qmp_execute')):return context['qmp_execute'](request)
        ports=json.loads(command(['docker','inspect','--format','{{json .NetworkSettings.Ports}}',container],timeout=10))
        if not any(row.get('HostIp')=='127.0.0.1' and row.get('HostPort')==str(context['qmp_port']) for row in (ports.get('4446/tcp') or [])):
            raise ValueError('QMP port does not belong to the owned emulator')
        with socket.create_connection(('127.0.0.1',context['qmp_port']),timeout=5) as connection:
            stream=connection.makefile('rwb')
            def receive():
                for _ in range(64):
                    raw=stream.readline(65537)
                    if len(raw)>65536 or not raw:raise RuntimeError('bounded QMP response unavailable')
                    reply=json.loads(raw)
                    if 'return' in reply or 'error' in reply:return reply
                raise RuntimeError('QMP event limit')
            greeting=stream.readline(65537)
            if len(greeting)>65536 or 'QMP' not in json.loads(greeting):raise RuntimeError('invalid QMP greeting')
            stream.write(b'{"execute":"qmp_capabilities"}\n');stream.flush()
            if 'return' not in receive():raise RuntimeError('QMP capability negotiation failed')
            stream.write(json.dumps(request).encode()+b'\n');stream.flush();return receive()

    def controls():
        nonlocal control_started,activation_ns,controller_report,transport,post_control_public
        control_started=True
        # The publisher/collector constructors can precede the shim's 120-second
        # dispatch barrier. Leave bounded initialization margin before that
        # barrier, while preserving the original overall runtime deadline.
        ready=min(deadline,time.monotonic()+180)
        while True:
            snapshot=observation();packet=snapshot['positions']
            journal=read('journalctl _PID='+pinned['pid']+' --no-pager -n 3000',1048576)
            (output/'control-readiness-journal.txt').write_text(journal)
            if controls_ready(snapshot,journal,manifest['position_binding']):break
            if compare_timestamps(time.monotonic(), ready, units_per_second=1) >= 0:raise RuntimeError('main dispatch/registration/position readiness deadline')
            time.sleep(.2)
        activation_ns=packet['native_monotonic_ns']
        if callable(context.get('panel_transport_factory')):
            transport=context['panel_transport_factory'](pinned['pid'],pinned['birth'],birth_guard)
            sender=transport if callable(transport) else transport.send
        else:sender=context['send_control']
        def send(payload):
            birth_guard()
            if len(payload)!=153:raise ValueError('one complete panel frame required')
            return sender(payload)
        controller=Controller(driver,context['driver_sha256'],output/'controls',token,owner,send,
            qmp_execute=qmp_execute,read_observation=observation,pid=pinned['pid'],work_dir=work,
            position_binding=manifest['position_binding'],max_source_rate=context.get('max_source_rate'),
            rate_bound_source=context.get('rate_bound_source'))
        selection_options={}
        if context.get('prepare_deck_load') is not None:
            selection_options['prepare_deck_load']=context['prepare_deck_load']
        if context.get('prepare_controls') is not None:
            selection_options['prepare_controls']=context['prepare_controls']
        controller_report=(execute_controls(controller) if custom else
                           controller.run_selected_track(context['prepare_track'],titles,**selection_options))
        if probe_sha is None:observation()
        else:post_control_public=capture_public_after_controls()

    def adapter(argv,**options):
        nonlocal control_started
        birth_guard()
        text=argv[-1]
        if text.startswith('cat >') and '/start.sh && chmod 700 ' in text:
            startup=options['input'].decode()
            if startup.count('export AZSDK_SOURCE_PROBE=1\n')!=1:raise ValueError('tested runtime startup export required')
            startup=startup.replace('export AZSDK_SOURCE_PROBE=1\n','export AZSDK_SOURCE_PROBE=1\nexport AZSDK_POSITION_PROBE=1\n')
            options=dict(options,input=startup.encode());(output/'start.sh').write_text(startup)
        result=command(argv,**options)
        if 'systemctl stop az-sdk-baseline.service' in text:
            pinned.update(pid='',birth='')
        elif text.startswith('pidof EP147 || true;'):
            rows=result.decode().splitlines();candidate=next((r for r in rows if '=' not in r),'')
            if re.fullmatch('[1-9][0-9]*',candidate):pinned['pid']=candidate
        elif text.startswith("awk '{print $22}' /proc/"):
            birth=result.decode().strip()
            if birth.isdigit():pinned['birth']=birth
        elif text.startswith('test ! -f /tmp/az-sdk-source-probe.') and pinned['birth']:
            if not control_started:controls()
            else:observation()
        return result

    owner()  # Refuse a different live nonce before any target write or evidence directory.
    baseline=read('pidof EP147 || true',4096).strip()
    stopped=context.get('stopped_identity')
    if stopped is not None:
        if baseline or not isinstance(stopped,dict) or set(stopped)!={'pid','birth'}:
            raise ValueError('stopped activation requires no existing native process and its prior identity')
        proof=command(ssh+[native_birth_check_script(stopped['pid'],stopped['birth'])],timeout=5)
        if proof!=b'AZSDK_NATIVE_BIRTH_REFUSED\n':raise ValueError('stopped activation prior process absence unverified')
    else:
        if not re.fullmatch('[1-9][0-9]*',baseline):raise ValueError('one live baseline process required')
        pinned['pid']=baseline
        pinned['birth']=read("awk '{print $22}' /proc/"+baseline+'/stat',4096).strip()
        if not pinned['birth'].isdigit():raise ValueError('baseline process birth token required')
    close_error=None
    try:
        options={'retain_operator':True} if retain_operator else {}
        underlying=runtime.run_runtime(ssh,package,source_root,output,firmware_sha256,seconds,adapter,**options)
    finally:
        if transport is not None and callable(getattr(transport,'close',None)):
            try:transport.close()
            except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as failure:
                close_error=str(failure) or type(failure).__name__+': transport close returned no diagnostic'
    # Revalidate raw observations under the loaded contract, never the empty
    # runtime's EEEE requirement. The original report is retained for diagnostics.
    (output/'lifecycle-report.json').write_text(json.dumps(underlying,indent=2)+'\n')
    loop=(output/'loop.log').read_text() if (output/'loop.log').is_file() else ''
    source=(output/'source.log').read_text() if (output/'source.log').is_file() else ''
    health=json.loads((output/'samples.json').read_text())
    identity=runtime._identity((output/'mapped-identity.txt').read_text()) if (output/'mapped-identity.txt').is_file() else {}
    validator=validate_observed if custom else validate_loaded_evidence
    report=validator(loop,source,health,identity,manifest,firmware_sha256,controller_report,positions,activation_ns,context,boottime_ns)
    if underlying.get('controller_error'):
        report['controller_error']=underlying['controller_error'];report['controller_operation']=underlying.get('controller_operation','')
        if underlying['status']=='FAIL':report['status']='FAIL'
        elif report['status']!='FAIL':report['status']='INCOMPLETE'
    if close_error is not None:
        report['transport_close_error']=close_error
        if report['status']!='FAIL':report['status']='INCOMPLETE'
    if post_control_public is not None:
        report['public_native_after_controls']=post_control_public
        if post_control_public['status']=='FAIL':report['status']='FAIL'
        elif post_control_public['status']!='PASS' and report['status']=='PASS':report['status']='INCOMPLETE'
    report.update(stage=underlying.get('stage'),artifact_sha256=manifest['sha256'],position_binding=manifest['position_binding'],observations=observations,
                  process_identity_fallbacks=identity_fallbacks,process_identity_fallback_history_limit=identity_fallbacks>64)
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report
