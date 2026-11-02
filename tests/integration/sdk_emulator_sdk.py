#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Owned-emulator SDK scalar runtime checks, package preparation and evidence.

This controller accepts loopback SSH from the nonce-owned smoke VM only. It
does not establish loaded-track binding, native commands or hardware acceptance.
"""
from sdk_timestamps import compare_timestamps
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import time
import uuid

from sdk_runtime_check import validate_health, validate_transcript
from sdk_probe_binding import binding_proof, build_id as _build_id

FLAGS=('azmod.track-data.on','azmod.native-loop.on','azmod.native-live.on',
       'azmod.native-sources.on','azmod.native-source-files.on','azmod.native-load.on','azmod-logs.on')
FILES=('azmod.so','azstem.so','loop-probe.so','source-probe.so')
SCOPE='all-deck scalar registration/receipt invariants in an owned emulator; no loaded-track, commands, persistence or audio acceptance'

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def _hash(value):
    return isinstance(value,str) and re.fullmatch('[0-9a-f]{64}',value)

def validate_package(package,source_root):
    package,source_root=Path(package),Path(source_root).resolve()
    path=package/'runtime.json'
    if path.is_symlink() or path.stat().st_size>262144:raise ValueError('invalid runtime manifest')
    manifest=json.loads(path.read_text())
    if manifest.get('schema')!=2 or set(manifest.get('sha256',{}))!=set(FILES):raise ValueError('runtime artifact inventory mismatch')
    if type(manifest.get('loop_operator')) is not bool:raise ValueError('explicit loop probe mode required')
    builds={}
    for name in FILES:
        p=package/name
        if p.is_symlink() or not p.is_file() or not 64<=p.stat().st_size<=64*1024*1024:raise ValueError('invalid runtime artifact '+name)
        if not _hash(manifest['sha256'][name]) or digest(p)!=manifest['sha256'][name]:raise ValueError('runtime artifact hash mismatch '+name)
        builds[name]=_build_id(p.read_bytes())
    sources=manifest.get('source_sha256')
    if not isinstance(sources,dict) or not sources:raise ValueError('runtime source closure required')
    for name,expected in sources.items():
        path=source_root/name
        if not isinstance(name,str) or Path(name).is_absolute() or '..' in Path(name).parts or '\\' in name or '\n' in name or source_root not in path.resolve().parents:
            raise ValueError('unsafe runtime source path')
        if path.is_symlink() or not path.is_file() or not _hash(expected) or digest(path)!=expected:raise ValueError('runtime compiled source changed '+name)
    for family,library in [('loop','azstem.so'),('source','azmod.so')]:
        binding=manifest.get('bindings',{}).get(family,{})
        proof=binding_proof(package,family)
        if binding!={'build_id':builds[library],'delta':proof['delta']}:
            raise ValueError('runtime probe build binding mismatch '+family)
        if manifest.get('binding_proofs',{}).get(family)!=proof:
            raise ValueError('runtime symbol/artifact binding proof mismatch '+family)
    return manifest

def prepare_package(destination,core,source_manifest,probes,source_root):
    """Bind the existing reviewed core/probe pair to current source bytes."""
    destination,core,probes=Path(destination),Path(core),Path(probes)
    if destination.exists():raise ValueError('runtime package destination must be new')
    prior=json.loads((probes/'manifest.json').read_text())
    production=json.loads(Path(source_manifest).read_text())
    sources={'src/'+name:sha for name,sha in production.items()}
    sources.update(prior['source_sha256'])
    names={'azmod.so':core/'azmod.so','azstem.so':core/'azstem.so',
           'loop-probe.so':probes/'operator-loop-probe.so','source-probe.so':probes/'source-probe.so'}
    for name,path in names.items():
        original=prior['core_libraries'][name] if name in ('azmod.so','azstem.so') else prior['probes'][path.name]
        if digest(path)!=original:raise ValueError('reviewed core/probe hash changed '+name)
    destination.mkdir(parents=True)
    for name,path in names.items():shutil.copyfile(str(path),str(destination/name))
    for name in ('azmod.so.full','azstem.so.full'):
        shutil.copyfile(str(core/name),str(destination/name))
    proofs={family:binding_proof(destination,family) for family in ('loop','source')}
    manifest={'schema':2,'sha256':{name:digest(path) for name,path in names.items()},
              'source_sha256':sources,'bindings':prior['bindings'],'loop_operator':True,
              'scope':SCOPE,'binding_proofs':proofs}
    (destination/'runtime.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    return validate_package(destination,source_root)

def _fields(line):
    pairs=[part.split('=',1) for part in line.split() if '=' in part]
    if len({p[0] for p in pairs})!=len(pairs):raise ValueError('duplicate probe fields')
    return dict(pairs)

def _rows(text,prefix):return [_fields(line) for line in text.splitlines() if line.startswith(prefix)]

def validate_evidence(loop,source,health,identity,manifest,firmware_sha256):
    errors,missing=[],[]
    report={'scope':SCOPE,'loaded_acceptance':'INCOMPLETE','source_binding_acceptance':'INCOMPLETE',
            'native_commands_acceptance':'INCOMPLETE','missing_decks':[]}
    health_result=validate_health(health);report['health']=health_result
    if health_result['status']=='FAIL':errors.extend(health_result['errors'])
    elif health_result['status']!='PASS':missing.append('runtime health')
    if health_result['status']=='PASS' and len(health)>1 and compare_timestamps(health[-1]['local_monotonic'] - health[0]['local_monotonic'], 120, units_per_second=1) < 0:missing.append('120-second runtime observation')
    pid=health[0].get('pid','') if health else ''
    if identity.get('pid') not in (None,pid):errors.append('mapped process identity changed')
    if identity.get('pid') is None:missing.append('mapped process identity')
    if identity.get('firmware_sha256') not in (None,firmware_sha256):errors.append('mapped firmware hash mismatch')
    if identity.get('firmware_sha256') is None:missing.append('mapped firmware hash')
    mapped=identity.get('mapped',{})
    for name,expected in manifest['sha256'].items():
        if name not in mapped:missing.append('mapped artifact '+name)
        elif mapped[name]!=expected:errors.append('mapped artifact hash mismatch '+name)
    if 'flags' not in identity:missing.append('feature flags')
    elif set(identity['flags'])!=set(FLAGS):errors.append('runtime feature flags mismatch')
    if identity.get('deck_mask')!='EEEE':missing.append('four observed empty decks')
    if not identity.get('deck_heartbeat_fresh'):missing.append('fresh deck heartbeat')
    if len(loop.encode())>262144 or len(source.encode())>65536:
        errors.append('probe byte cap exceeded');loop=source=''
    transcript=validate_transcript(loop,(1,2,3,4));report['transcript']=transcript
    if transcript['status']=='FAIL':errors.extend(transcript['errors'])
    for deck in range(1,5):
        needed={'loop','play','quantize','key_lock'}-set(transcript['coverage'][str(deck)])
        if needed:
            report['missing_decks'].append(deck);missing.append('deck%d streams: %s'%(deck,','.join(sorted(needed))))
    try:
        headers=_rows(loop,'pid=');source_headers=_rows(source,'pid=')
        if len(headers)!=1 or headers[0].get('exports')!='1,1':missing.append('current loop exports')
        if len(source_headers)!=1:missing.append('source probe header')
        for header in headers+source_headers:
            if header.get('pid')!=pid:errors.append('probe PID mismatch')
        for header in source_headers:
            if int(header.get('nice_result','-1'))!=0:errors.append('source probe could not establish its low-priority cadence')
        if 'live_exports=1,1' not in loop.splitlines():missing.append('current live exports')
        if 'private_load_export=1' not in loop.splitlines():missing.append('current private load ABI')
        if 'diagnostic_available=1' not in loop.splitlines():missing.append('current matched loop helper')
        for prefix in ['registration_mask=','live_registration_mask=']:
            masks=_rows(loop,prefix)
            key=prefix[:-1]
            if not masks or int(masks[-1][key])!=15:missing.append('all four '+key)
            if any(int(row[key]) & ~15 for row in masks):errors.append('invalid registration mask')
        endings=_rows(loop,'complete ')
        if len(endings)!=1:missing.append('loop completion footer')
        for ending in endings:
            if int(ending.get('errors','-1'))!=0:errors.append('loop probe reported errors')
            if any(ending.get(key)!='15' for key in ['seen','submitted','live_seen','live_submitted']):missing.append('all four completed loop/live slots')
        for row in _rows(loop,'live_deck='):
            if compare_timestamps(int(row.get('age_ns', '-1')), 0) < 0:errors.append('invalid receipt age')
            elif compare_timestamps(int(row['age_ns']), 5 * 10 ** 9) > 0:missing.append('fresh live receipt')
        if identity.get('deck_mask')=='EEEE':
            for row in _rows(loop,'deck='):
                if row.get('engaged')!='0' or row.get('bounds')!='0':errors.append('empty deck published an engaged loop')
        resolvers=_rows(source,'resolver_available=')
        if len(resolvers)!=1 or resolvers[0].get('resolver_available')!='1':missing.append('matched source helper')
        elif resolvers[0].get('build_id')!=manifest['bindings']['source']['build_id']:errors.append('source helper build ID mismatch')
        for line in source.splitlines():
            row=_fields(line)
            if int(row.get('invalid','0')):errors.append('source probe reported invalid records')
            if int(row.get('history_limit','0')):missing.append('source scan history limit')
        endings=_rows(source,'complete ')
        if len(endings)!=1:missing.append('source completion footer')
        for ending in endings:
            status=ending.get('status')
            if status=='resolver_refused':missing.append('source resolver refused experimental/mismatched ABI')
            elif status!='time_limit':errors.append('source probe failed: '+str(status))
            elif int(ending.get('passes','0'))<1:missing.append('complete source-ID scan')
    except (KeyError,ValueError,TypeError,OverflowError) as error:
        errors.append('malformed runtime probe: '+str(error))
    report.update(status='FAIL' if errors else 'INCOMPLETE' if missing else 'PASS',
                  errors=sorted(set(errors)),missing=sorted(set(missing)))
    return report

def after_baseline(baseline,activate):
    if baseline.get('status')!='PASS':return {'status':'INCOMPLETE','scope':SCOPE,'reason':'stock baseline did not pass; SDK activation not attempted'}
    try:
        return activate()
    except ValueError as error:
        return {'status':'FAIL','scope':SCOPE,'errors':['runtime prerequisite integrity failure: '+str(error)]}

def _stage(stage):
    if not re.fullmatch('/tmp/az-sdk-runtime\\.[a-f0-9]{32}',stage):raise ValueError('owned emulator runtime stage required')
    return stage

def mask_guard():
    return '''mask=$(systemctl is-enabled EP147.service 2>/dev/null || true)
case "$mask" in masked|masked-runtime) ;; *)
printf 'Original EP147 service is not masked: %s\\n' "$mask" >&2
exit 92;;
esac
'''

def setup_script(stage):
    _stage(stage)
    flags=' '.join('/home/root/settings/'+name for name in FLAGS)
    return 'set -e\n'+mask_guard()+'''systemctl stop az-sdk-baseline.service
touch %s
chmod 700 %s/start.sh
cp /run/systemd/system/az-sdk-baseline.service /run/systemd/system/az-sdk-runtime.service
sed -i 's|^ExecStart=.*|ExecStart=%s/start.sh|;s/^Restart=.*/Restart=no/' /run/systemd/system/az-sdk-runtime.service
systemctl daemon-reload
systemctl start az-sdk-runtime.service --no-block
'''%(flags,stage,stage)

def startup_script(baseline,stage):
    """Preserve the tested shim's startup configuration in the SDK process."""
    _stage(stage)
    original='export LD_PRELOAD=/home/root/ep122_shim.so'
    lines=baseline.splitlines()
    if len(baseline.encode())>65536 or lines.count(original)!=1 or sum('LD_PRELOAD' in line for line in lines)!=1:
        raise ValueError('one tested shim-only startup preload required')
    preloads='/home/root/ep122_shim.so:'+':'.join(stage+'/'+name for name in FILES)
    replacement='export LD_PRELOAD='+preloads+'\nexport AZSDK_LOOP_PROBE=1\nexport AZSDK_SOURCE_PROBE=1'
    return '\n'.join(replacement if line==original else line for line in lines)+'\n'

def operator_stop(loop,pid):
    headers=_rows(loop,'operator_mode=')
    if not headers:return None
    if len(headers)!=1 or headers[0].get('operator_mode')!='1':raise ValueError('invalid operator probe header')
    path=headers[0].get('stop_file','')
    if not re.fullmatch('/tmp/az-sdk-loop-probe\\.'+re.escape(pid)+'\\.[0-9]+\\.stop',path):raise ValueError('operator stop path does not belong to the pinned process')
    return path

def _identity(text):
    result={'mapped':{},'flags':[],'deck_heartbeat_fresh':False}
    heartbeat={}
    for line in text.splitlines():
        fields=line.split()
        if len(fields)==3 and fields[0]=='MAP':result['mapped'][fields[1]]=fields[2]
        elif len(fields)==2 and fields[0]=='FLAG':result['flags'].append(fields[1])
        elif '=' in line:
            key,value=line.split('=',1)
            if key in ['pid','firmware_sha256','deck_mask']:result[key]=value
            elif key=='heartbeat_fresh':result['deck_heartbeat_fresh']=value=='1'
            elif key in ('heartbeat_pid','heartbeat_up','heartbeat_now'):heartbeat[key]=value
    if heartbeat:
        # /tmp/az-decks copies /proc/uptime at its native 10 ms precision.
        # Read the packet before the observation clock, so a concurrent
        # publisher cannot turn a current heartbeat into a future timestamp.
        from decimal import Decimal
        try:
            if set(heartbeat)!={'heartbeat_pid','heartbeat_up','heartbeat_now'} or any(not re.fullmatch(r'[0-9]+(?:\.[0-9]{1,9})?',heartbeat[key]) for key in ('heartbeat_up','heartbeat_now')):
                raise ValueError('invalid heartbeat clock')
            up,now=(int(Decimal(heartbeat[key])*1_000_000_000) for key in ('heartbeat_up','heartbeat_now'))
            result['deck_heartbeat_fresh']=heartbeat['heartbeat_pid']==result.get('pid') and compare_timestamps(now,up,units_per_second=1_000_000_000,sample_rate=100)>=0 and compare_timestamps(now-up,5_000_000_000,units_per_second=1_000_000_000,sample_rate=100)<=0
        except (ValueError,OverflowError):result['deck_heartbeat_fresh']=False
    return result

def identity_script(pid,stage):
    if not re.fullmatch('[1-9][0-9]*',pid):raise ValueError('one pinned process required')
    _stage(stage)
    text='p='+pid+'; printf "pid=%s\\n" "$p"; printf "firmware_sha256=%s\\n" "$(sha256sum /proc/$p/exe | cut -d\" \" -f1)"\n'
    for name in FILES:
        path=stage+'/'+name
        text+='range=$(awk -v p='+path+' \'$NF==p {print $1;exit}\' /proc/$p/maps); if test -n "$range"; then h=$(sha256sum /proc/$p/map_files/$range 2>/dev/null | cut -d" " -f1); test -z "$h" || printf "MAP '+name+' %s\\n" "$h"; fi\n'
    for name in FLAGS:text+='test ! -e /home/root/settings/'+name+' || echo FLAG '+name+'\n'
    text+='''if test -f /tmp/az-decks; then
awk '($1=="pid"){pid=$2} ($1=="up"){up=$2} ($1=="decks"){mask=$2} END{printf "deck_mask=%s\\nheartbeat_pid=%s\\nheartbeat_up=%s\\n",mask,pid,up}' /tmp/az-decks
printf "heartbeat_now=%s\\n" "$(cut -d' ' -f1 /proc/uptime)"
fi
'''
    return text

def run_runtime(ssh,package,source_root,output,firmware_sha256,seconds,command,*,retain_operator=False):
    """Run only inside the smoke runner's owned loopback guest; all units are RAM copies."""
    if not ssh or ssh[-1]!='root@127.0.0.1':raise ValueError('owned loopback emulator SSH required')
    if type(seconds) is not int or not 120<=seconds<=1800:raise ValueError('runtime duration must be 120..1800 seconds')
    if type(retain_operator) is not bool:raise ValueError('explicit operator retention required')
    package,output=Path(package),Path(output)
    manifest=validate_package(package,source_root)
    if output.exists():raise ValueError('runtime evidence directory must be new')
    output.mkdir()
    stage='/tmp/az-sdk-runtime.'+uuid.uuid4().hex
    samples=[];loop=source='';identity={};pid=birth='';error=None;integrity_failure=False;operation='';error_operation=''
    diagnostic_errors=[]
    def invoke(text,**options):
        nonlocal operation
        operation=text[:2048]
        return command(ssh+[text],**options)
    def remote(text,cap=524288,timeout=45):
        data=invoke(text,timeout=timeout)
        if len(data)>cap:raise ValueError('runtime remote output cap exceeded')
        return data.decode(errors='replace')
    def logs():
        nonlocal loop,source
        loop=remote('test ! -f /tmp/az-sdk-loop-probe.'+pid+'.log || cat /tmp/az-sdk-loop-probe.'+pid+'.log',262144)
        (output/'loop.log').write_text(loop)
        source=remote('test ! -f /tmp/az-sdk-source-probe.'+pid+'.log || cat /tmp/az-sdk-source-probe.'+pid+'.log',65536)
        (output/'source.log').write_text(source)
    def interrupted_diagnostics():
        """Read existing evidence once, without retrying any activation/control.

        The command adapter may trigger controls for normal probe reads. These
        explicitly marked, birth-guarded scripts cannot match those callbacks.
        Six reads at five seconds each bound this best-effort failure path.
        """
        nonlocal loop,source,identity
        if not re.fullmatch('[1-9][0-9]*',pid) or not birth.isdigit():return
        guard='test -r /proc/'+pid+'/stat && test "$(awk \'{print $22}\' /proc/'+pid+'/stat)" = '+birth
        check='AZSDK_RUNTIME_DIAGNOSTIC_GUARD=1\nif '+guard+'; then printf "AZSDK_RUNTIME_BIRTH_OK\\n"; fi\n'
        try:
            if remote(check,4096,5).strip()!='AZSDK_RUNTIME_BIRTH_OK':
                diagnostic_errors.append('collection skipped: pinned process unavailable or changed')
                return
        except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as failure:
            diagnostic_errors.append('birth guard: '+(str(failure) or type(failure).__name__)[:2048])
            return
        jobs=[('loop','loop.log','test ! -f /tmp/az-sdk-loop-probe.'+pid+'.log || cat /tmp/az-sdk-loop-probe.'+pid+'.log',262144),
              ('source','source.log','test ! -f /tmp/az-sdk-source-probe.'+pid+'.log || cat /tmp/az-sdk-source-probe.'+pid+'.log',65536),
              ('journal','journal.txt','journalctl -u az-sdk-runtime.service _PID='+pid+' --no-pager -n 3000',524288),
              ('maps','maps.txt','cat /proc/'+pid+'/maps',524288),
              ('identity','mapped-identity.txt',identity_script(pid,stage),524288)]
        for name,filename,body,cap in jobs:
            # Check before and after reading; never retain evidence if this PID
            # now names a different process. head bounds even a corrupt log.
            script='AZSDK_RUNTIME_DIAGNOSTIC_READ='+name+'\nset -e\n'+guard+' || exit 93\n{\n'+body+'\n} | head -c '+str(cap+1)+'\n'+guard+' || exit 93\n'
            try:
                text=remote(script,cap,5)
                (output/filename).write_text(text)
                if name=='loop':loop=text
                elif name=='source':source=text
                elif name=='identity':identity=_identity(text)
            except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as failure:
                diagnostic_errors.append(name+': '+(str(failure) or type(failure).__name__)[:2048])
    try:
        remote(mask_guard())
        remote('umask 077; mkdir '+stage)
        payload=io.BytesIO()
        with tarfile.open(fileobj=payload,mode='w') as archive:
            for name in FILES:archive.add(str(package/name),arcname=name,recursive=False)
        invoke('tar -xf - -C '+stage,input=payload.getvalue(),timeout=60)
        uploaded=remote('sha256sum '+' '.join(stage+'/'+name for name in FILES))
        actual={line.split()[1].rsplit('/',1)[-1]:line.split()[0] for line in uploaded.splitlines() if len(line.split())==2}
        if actual!=manifest['sha256']:raise ValueError('uploaded runtime hashes mismatch')
        startup=startup_script(remote('cat /tmp/az-sdk-baseline/start.sh',65536),stage)
        (output/'start.sh').write_text(startup)
        invoke('cat >'+stage+'/start.sh && chmod 700 '+stage+'/start.sh',input=startup.encode(),timeout=45)
        plan=setup_script(stage);(output/'setup.sh').write_text(plan)
        remote(plan)
        deadline=time.monotonic()+90
        while compare_timestamps(time.monotonic(), deadline, units_per_second=1) < 0:
            rows=remote('pidof EP147 || true; systemctl show az-sdk-runtime.service -p NRestarts -p ActiveState -p SubState',4096).splitlines()
            candidate=next((row for row in rows if '=' not in row),'')
            service=dict(row.split('=',1) for row in rows if '=' in row)
            if service.get('ActiveState')=='failed':
                samples.append({'pid':candidate,'service':service,'local_monotonic':time.monotonic()})
                raise RuntimeError('SDK service failed before process capture')
            if re.fullmatch('[1-9][0-9]*',candidate):pid=candidate;break
            time.sleep(1)
        if not pid:raise RuntimeError('native SDK process startup deadline')
        birth=remote("awk '{print $22}' /proc/"+pid+'/stat',4096).strip()
        if not birth.isdigit():raise ValueError('missing pinned process birth token')
        start=time.monotonic();stop_sent=False
        while True:
            text=remote('if test -r /proc/'+pid+'/stat && test "$(awk \'{print $22}\' /proc/'+pid+'/stat)" = '+birth+'; then echo '+pid+'; fi; systemctl show az-sdk-runtime.service -p NRestarts -p ActiveState -p SubState',4096)
            rows=text.splitlines()
            samples.append({'pid':next((r for r in rows if '=' not in r),''),
                            'service':dict(r.split('=',1) for r in rows if '=' in r),'local_monotonic':time.monotonic()})
            logs()
            elapsed=time.monotonic()-start
            observed=samples[-1]['local_monotonic']-samples[0]['local_monotonic']
            footer=bool(_rows(source,'complete '))
            if manifest['loop_operator'] and not retain_operator and not stop_sent and footer and compare_timestamps(observed,120,units_per_second=1)>=0:
                path=operator_stop(loop,pid)
                if path:remote('touch '+path);stop_sent=True
            capture_ready=bool(_rows(loop,'complete ')) or (retain_operator and manifest['loop_operator'])
            if compare_timestamps(observed,120,units_per_second=1)>=0 and capture_ready and footer:break
            if compare_timestamps(elapsed, seconds, units_per_second=1) >= 0 or validate_health(samples)['status']=='FAIL':break
            time.sleep(5)
        if manifest['loop_operator'] and not retain_operator and not _rows(loop,'complete '):
            path=operator_stop(loop,pid)
            if path:remote('touch '+path);time.sleep(1);logs()
        identity_text=remote(identity_script(pid,stage));(output/'mapped-identity.txt').write_text(identity_text)
        identity=_identity(identity_text)
        (output/'journal.txt').write_text(remote('journalctl -u az-sdk-runtime.service --no-pager -n 3000'))
    except (RuntimeError,ValueError,OSError,subprocess.TimeoutExpired) as failure:
        error=str(failure) or type(failure).__name__+': remote operation returned no diagnostic'
        error_operation=operation
        integrity_failure=isinstance(failure,ValueError)
        interrupted_diagnostics()
    report=validate_evidence(loop,source,samples,identity,manifest,firmware_sha256)
    report.update(stage=stage,artifact_sha256=manifest['sha256'])
    if error is not None:
        report['controller_error']=error
        report['controller_operation']=error_operation
        if integrity_failure:report['status']='FAIL'
        elif report['status']!='FAIL':report['status']='INCOMPLETE'
    if diagnostic_errors:report['diagnostic_collection_errors']=diagnostic_errors
    (output/'samples.json').write_text(json.dumps(samples,indent=2)+'\n')
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination',type=Path);parser.add_argument('core',type=Path)
    parser.add_argument('source_manifest',type=Path);parser.add_argument('probes',type=Path)
    args=parser.parse_args()
    prepare_package(args.destination,args.core,args.source_manifest,args.probes,Path(__file__).resolve().parents[2])
    print('Prepared source/hash/build-ID verified emulator runtime package: '+str(args.destination))

if __name__=='__main__':main()
