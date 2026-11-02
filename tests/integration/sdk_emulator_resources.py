#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sequential CPU/RAM experiment against a frozen, shim-only firmware baseline.

Successful comparison is not successful firmware startup. Infrastructure errors,
OOMs, changed inputs and missing evidence invalidate resource-only conclusions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import shutil

LOW_MEMORY = 2147483648
NORMAL_MEMORY = 4038197248
CASES = [('low-4cpu-2g-1',4,LOW_MEMORY), ('normal-6cpu-native-1',6,NORMAL_MEMORY),
         ('fourcpu-native-1',4,NORMAL_MEMORY), ('sixcpu-2g-1',6,LOW_MEMORY),
         ('normal-6cpu-native-2',6,NORMAL_MEMORY), ('low-4cpu-2g-2',4,LOW_MEMORY)]
ASSETS = ('/home/root/pdj/EP147','/home/root/ep122_shim.so',
          '/usr/lib/modules/virtio_snd.ko','/etc/asound.conf')


def prepare(output, base, profile, source=None):
    if output.exists(): raise ValueError('new experiment output required')
    if profile not in ('130','200'): raise ValueError('unsupported firmware profile')
    for name in ('Image','initramfs-patched.cpio.gz','az-mnt.img','usb.img','ssh/az-emu_ed25519'):
        if not (base/name).is_file(): raise ValueError('missing emulator input: '+name)
    source=source or Path(__file__).resolve().parent
    image=subprocess.check_output(['docker','image','inspect','az-sdk-emulator-qemu:local','--format','{{.Id}}']).decode().strip()
    output.mkdir(parents=True); frozen=output/'runner'; frozen.mkdir()
    names=('sdk_emulator_smoke.py','sdk_native_run.py','sdk_runtime_check.py')
    for name in names: shutil.copy2(source/name,frozen/name)
    manifest={'runner_sha256':{n:hashlib.sha256((frozen/n).read_bytes()).hexdigest() for n in names},
              'qemu_image_id':image,'base':str(base.resolve()),'profile':profile,
              'scope':'sequential shim-only stock firmware startup; only CPU and guest memory differ'}
    (output/'inputs.json').write_text(json.dumps(manifest,indent=2)+'\n')


def measurement(report, identity, journal, host):
    hashes = {name:sha for sha,name in re.findall(r'^([0-9a-f]{64})  (.+)$',identity,re.M)}
    counters = {name:int(value) for name,value in re.findall(r'^(oom(?:_kill|_group_kill)?|nr_throttled|throttled_usec)\s+([0-9]+)$',host,re.M)}
    errors=[]
    if report.get('error'): errors.append(report['error'])
    if not report.get('owned_vm_removed') or not report.get('base_disks_unchanged'):
        errors.append('cleanup or base-disk invariant missing')
    if report.get('container_state',{}).get('OOMKilled') is not False:
        errors.append('missing/failed container OOM evidence')
    if not {'oom','oom_kill'}.issubset(counters) or any(counters.get(k,0) for k in ('oom','oom_kill','oom_group_kill')):
        errors.append('missing cgroup evidence or memory exhaustion')
    if report.get('kernel_tests',{}).get('status') != 'INCOMPLETE':
        errors.append('unexpected SDK workload before firmware startup')
    if not set(ASSETS).issubset(hashes): errors.append('missing guest identity hashes')
    if not re.search(r'^virtio_snd\s+[0-9]+\s',identity,re.M) or 'ALPHATHETAD810' not in identity:
        errors.append('audio module or ALSA card is missing')
    keys=('qemu_image_id','kernel_sha256','initramfs_sha256','firmware_sha256','base_disks')
    if not all(report.get(k) for k in keys): errors.append('missing input identity')
    baseline=report.get('baseline',{})
    if 'DataPoolablePreviewMixer::processBlock() - retryCount < 20000' in journal:
        outcome='preview_mixer_assertion'
    elif baseline.get('status')=='PASS' and baseline.get('nonblank_display') is True:
        outcome='started'
    else:
        outcome='other_or_incomplete'
        errors.append('startup outcome not established')
    fingerprint={k:report.get(k) for k in keys}; fingerprint['guest_sha256']={k:hashes.get(k) for k in ASSETS}
    return {'valid':not errors,'errors':errors,'outcome':outcome,'resources':report.get('resources',{}),
            'identity':fingerprint,'host_counters':counters,
            'affinity_error_count':sum('taskset:' in line and 'Invalid argument' in line for line in journal.splitlines())}


def compare(cases):
    expected={(c,m) for c in (4,6) for m in (LOW_MEMORY,NORMAL_MEMORY)}
    actual={(r['resources'].get('vcpus'),r['resources'].get('guest_memory_bytes')) for r in cases}
    same_inputs=bool(cases) and all(r['identity']==cases[0]['identity'] for r in cases)
    valid=expected.issubset(actual) and same_inputs and all(r['valid'] for r in cases)
    conclusion='incomplete_comparison'
    if valid:
        outcomes={r['outcome'] for r in cases}
        if outcomes=={'preview_mixer_assertion'}: conclusion='same_preview_mixer_assertion_at_all_resource_settings'
        elif len(outcomes)>1: conclusion='startup_outcome_changed_with_resources'
        else: conclusion='startup_succeeded_at_all_resource_settings'
    return {'valid_comparison':valid,'same_inputs':same_inputs,'conclusion':conclusion,'cases':cases,
            'scope':'CPU/topology and RAM effects in tested configurations; does not identify a particular emulator defect'}


def boot_resources(serial, cpus, memory_bytes):
    cpu=re.search(r'smp: Brought up [0-9]+ nodes?, ([0-9]+) CPUs',serial)
    memory=re.search(r'Memory: [0-9]+K/([0-9]+)K available',serial)
    actual_cpu=int(cpu[1]) if cpu else None
    actual_memory=int(memory[1])*1024 if memory else None
    return {'valid':actual_cpu==cpus and actual_memory==memory_bytes,
            'online_cpus':actual_cpu,'guest_memory_bytes':actual_memory}


def read_case(folder):
    report=json.loads((folder/'report.json').read_text())
    read=lambda name:(folder/name).read_text() if (folder/name).exists() else ''
    result=measurement(report,read('guest-identity.txt'),read('baseline-journal.txt'),read('host-after.txt'))
    serial=(folder/'vm/serial.log').read_text(errors='replace') if (folder/'vm/serial.log').exists() else ''
    expected=result['resources']
    result['boot_resources']=boot_resources(serial,expected.get('vcpus'),expected.get('guest_memory_bytes'))
    if not result['boot_resources']['valid']:
        result['valid']=False; result['errors'].append('guest did not confirm expected CPU/RAM')
    result['affinity_error_count']=sum('taskset:' in line and 'Invalid argument' in line for line in serial.splitlines())
    result['name']=folder.name
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--port',type=int,default=2370)
    parser.add_argument('--qmp-port',type=int,default=4470)
    parser.add_argument('--analyze-only',action='store_true')
    parser.add_argument('--base',type=Path,help='create a new experiment from this existing emulator build')
    parser.add_argument('--profile',choices=('130','200'),default='200')
    args=parser.parse_args(); root=args.output.resolve()
    if args.base:
        if args.analyze_only: parser.error('--base cannot be combined with --analyze-only')
        prepare(root,args.base.resolve(),args.profile)
    manifest=json.loads((root/'inputs.json').read_text()); frozen=root/'runner'
    for name,sha in manifest['runner_sha256'].items():
        if hashlib.sha256((frozen/name).read_bytes()).hexdigest()!=sha:
            raise ValueError('frozen runner changed')
    if not args.analyze_only:
        image=subprocess.check_output(['docker','image','inspect','az-sdk-emulator-qemu:local','--format','{{.Id}}']).decode().strip()
        if image!=manifest['qemu_image_id']: raise ValueError('QEMU image changed')
        sys.path.insert(0,str(frozen))
        from sdk_emulator_smoke import run
    cases=[]
    for name,cpus,memory in CASES:
        folder=root/name
        if not (folder/'report.json').exists():
            if args.analyze_only: raise ValueError('case is incomplete')
            if folder.exists(): raise ValueError('unfinished case directory; inspect before retry')
            print('START '+name,flush=True)
            run(Path(manifest['base']),None,folder,manifest.get('profile','200'),args.port,args.qmp_port,vcpus=cpus,
                memory_bytes=memory,native_tests=False,baseline_seconds=120,capture_pcm=True)
        case=read_case(folder)
        if case['identity']['qemu_image_id']!=manifest['qemu_image_id']: raise ValueError('recorded QEMU image mismatch')
        if case['resources']!={'vcpus':cpus,'guest_memory_bytes':memory}: raise ValueError('case resource mismatch')
        cases.append(case)
        print(name+': '+case['outcome']+' valid='+str(case['valid']),flush=True)
        (root/'comparison.json').write_text(json.dumps(compare(cases),indent=2)+'\n')
        if not case['valid']: break
    result=compare(cases)
    print(json.dumps({'valid_comparison':result['valid_comparison'],'conclusion':result['conclusion']},indent=2))
    return 0 if result['valid_comparison'] else 2


if __name__=='__main__': raise SystemExit(main())
