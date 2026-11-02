#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Test an isolated firmware emulator clone, then remove only that owned VM.

Original disk images are mounted read-only behind fresh qcow2 overlays. Native
kernel SDK tests run before a shim-only firmware baseline. Baseline failure never
becomes a feature pass. Requires the already-built local emulator QEMU image.
"""
from sdk_timestamps import compare_timestamps
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import re
import socket
import subprocess
import struct
import time
import uuid

from sdk_native_run import execute, digest, validate_package
from sdk_runtime_check import ssh_arguments, validate_health


FIRMWARE = {"130": "736bdc9322c00e5770af459c92cace33d8680825c07f00f909f74dfc473a77a6",
            "200": "6577f42532375936c7e3fa830efa35b7daa9cde8e0a265c1fc7143f824321925"}


def scanned_host_keys(data, port):
    """Ignore keyscan banner comments; pin only keys for this owned endpoint."""
    if len(data)>32768: raise ValueError('host-key scan exceeds byte cap')
    expected=('127.0.0.1' if port==22 else '[127.0.0.1]:%d' % port).encode()
    result=[]
    for line in data.splitlines():
        fields=line.split()
        if len(fields)!=3 or fields[0]!=expected or fields[1] not in (b'ssh-ed25519',b'ssh-rsa',b'ecdsa-sha2-nistp256'):
            continue
        try:
            blob=base64.b64decode(fields[2],validate=True)
            length=struct.unpack('>I',blob[:4])[0]
            if blob[4:4+length]!=fields[1] or len(blob)<length+8: continue
        except (ValueError,struct.error):
            continue
        value=b' '.join(fields)+b'\n'
        if value not in result: result.append(value)
    return b''.join(result)


def acceptance_status(kernel, baseline, native_runtime=None, loaded_runtime=None,controlled_runtime=None):
    statuses = (kernel.get("status"), baseline.get("status"))
    if native_runtime is not None:
        statuses += (native_runtime.get('status'),)
    if loaded_runtime is not None:
        statuses += (loaded_runtime.get('status'),)
    if controlled_runtime is not None:
        statuses += (controlled_runtime.get('status'),)
    return "FAIL" if "FAIL" in statuses else "PASS" if all(s=='PASS' for s in statuses) else "INCOMPLETE"


def baseline_result(health, display_ready):
    result = dict(health, nonblank_display=display_ready)
    if not display_ready and result["status"] != "FAIL":
        result["status"] = "INCOMPLETE"
    return result


HEALTH_COMMAND = "pidof EP147 || true; systemctl show az-sdk-baseline.service -p NRestarts -p ActiveState -p SubState"


def await_baseline(remote):
    deadline = time.monotonic() + 30
    while True:
        state = remote(HEALTH_COMMAND)
        if (re.search(r"^[1-9][0-9]*$", state, re.M) and "ActiveState=active\n" in state and
                "SubState=running\n" in state) or "ActiveState=failed\n" in state or compare_timestamps(time.monotonic(), deadline, units_per_second=1) >= 0:
            return state
        time.sleep(1)


def _baseline_principal(state):
    lines=state.splitlines()
    pids=[line for line in lines if '=' not in line]
    service=dict(line.split('=',1) for line in lines if '=' in line)
    if len(pids)!=1 or not re.fullmatch('[1-9][0-9]*',pids[0]):
        raise ValueError('baseline principal process is missing or ambiguous')
    if service.get('ActiveState')!='active' or service.get('SubState')!='running' or service.get('NRestarts')!='0':
        raise ValueError('baseline service is not active/running without restarts')
    return pids[0]


def baseline_health_command(pid,birth):
    if not re.fullmatch('[1-9][0-9]*',pid) or not re.fullmatch('[1-9][0-9]*',birth):
        raise ValueError('one pinned baseline process and birth token required')
    return ('if test -r /proc/'+pid+'/stat && test "$(awk \'{if ($3 != "Z" && $3 != "X" && $3 != "x") print $22}\' /proc/'+pid+'/stat)" = '+birth+'; then echo '+pid+'; fi; '
            'systemctl show az-sdk-baseline.service -p NRestarts -p ActiveState -p SubState')


def baseline_identity(remote,state):
    """Pin only an unambiguous principal, then verify capture before sampling.

    Later same-name fork children are a process inventory change, not a change
    of this identity. Missing, zombie or reused principals never emit its PID.
    """
    pid=_baseline_principal(state)
    def capture_clock(previous=None,start=None):
        value=time.monotonic()
        if type(value) not in (int,float) or not math.isfinite(value) or compare_timestamps(value,0,units_per_second=1)<0 or (previous is not None and compare_timestamps(value,previous,units_per_second=1)<0) or (start is not None and compare_timestamps(value-start,30,units_per_second=1)>0):
            raise ValueError('baseline identity capture needs a finite forward clock and 30-second bound')
        return value
    started=capture_clock()
    birth=remote("awk '{print $22}' /proc/"+pid+'/stat 2>/dev/null || true').strip()
    captured=capture_clock(started,started)
    if not re.fullmatch('[1-9][0-9]*',birth):raise ValueError('baseline process died before birth capture')
    confirmed=remote(baseline_health_command(pid,birth))
    captured=capture_clock(captured,started)
    if _baseline_principal(confirmed)!=pid:raise ValueError('baseline principal changed during identity capture')
    return {'pid':pid,'birth':birth,'captured_monotonic':captured,
            'service':dict(line.split('=',1) for line in confirmed.splitlines() if '=' in line)}


def memory_options(available_kib, memory_bytes=2147483648):
    if type(memory_bytes) is not int or not 1024**3 <= memory_bytes <= 4*1024**3:
        raise ValueError("guest memory must be between 1 and 4 GiB")
    limit_mib = (memory_bytes + 1048575) // 1048576 + 256
    if type(available_kib) is not int or available_kib < (limit_mib + 512) * 1024:
        raise ValueError("insufficient memory for guest,256MiB overhead and512MiB reserve")
    return ["--memory=%dm" % limit_mib, "--memory-swap=%dm" % limit_mib, "--oom-score-adj=500"]


def prepare_jog_output(work):
    """Create only this run's unused 1 MiB BAR backing; never reuse a frame."""
    path=work/'jog.shm'
    with path.open('xb') as stream:
        stream.truncate(1<<20)
    path.chmod(0o600)
    return path


def boot_arguments(base, work, name, token, port, qmp, available_kib, vcpus=4, memory_bytes=2147483648,jog_output=False):
    if type(jog_output) is not bool:raise ValueError('jog output must be an explicit boolean')
    if not re.fullmatch(r"az-sdk-automation-[a-z0-9-]+", name) or not re.fullmatch(r"[0-9a-f]{32}", token):
        raise ValueError("invalid owned emulator identity")
    if any(type(p) is not int or not 1 <= p <= 65535 for p in (port, qmp)) or port == qmp:
        raise ValueError("invalid emulator ports")
    if type(vcpus) is not int or not 1 <= vcpus <= 16:
        raise ValueError("invalid guest CPU count")
    limits = memory_options(available_kib, memory_bytes)
    memory = str(memory_bytes // 1048576) + "M" if memory_bytes % 1048576 == 0 else str(memory_bytes) + "B"
    command = ["docker", "run", "--rm", "--platform", "linux/amd64", "-d", "--name", name,
               "--label", "az-sdk-token=" + token] + limits
    command += ["-p", "127.0.0.1:%d:2222" % port, "-p", "127.0.0.1:%d:4446" % qmp,
                "-v", str(base) + ":/base:ro", "-v", str(work) + ":/work", "az-sdk-emulator-qemu:local",
                "-machine", "virt", "-nodefaults", "-accel", "tcg,thread=multi", "-cpu", "cortex-a72",
                "-smp", str(vcpus), "-m", memory, "-kernel", "/base/Image", "-initrd", "/base/initramfs-patched.cpio.gz",
                "-append", "root=/dev/ram0 rdinit=/init loglevel=7 nowatchdog console=ttyAMA0,115200 systemd.journald.forward_to_console=1 snd_dummy.index=1 dummy.numdummies=0 nokaslr systemd.mask=EP147.service",
                "-display", "none", "-netdev", "user,id=net0,hostfwd=tcp:0.0.0.0:2222-:22",
                "-device", "virtio-net-device,netdev=net0", "-serial", "file:/work/serial.log",
                "-qmp", "tcp:0.0.0.0:4446,server=on,wait=off", "-device", "virtio-rng-device",
                "-audiodev", "wav,id=audio0,path=/dev/null", "-device", "virtio-sound-device,audiodev=audio0,streams=2",
                "-device", "virtio-gpu-device,id=gpu0,xres=1280,yres=800,edid=off,max_outputs=1",
                "-device", "virtio-tablet-pci,id=tablet0",
                "-drive", "file=/work/az-mnt.qcow2,if=none,id=mnt0,format=qcow2,cache=writeback",
                "-device", "virtio-blk-device,drive=mnt0",
                "-drive", "file=/work/usb.qcow2,if=none,id=usbdrv,format=qcow2,cache=writeback",
                "-device", "usb-ehci,id=ehci", "-device", "usb-storage,bus=ehci.0,drive=usbdrv",
                "-device", "virtio-serial-device,max_ports=8"]
    for short, native in (("main", "main"), ("sub", "sub"), ("ctrl", "ctrl"), ("gpio", "gpio")):
        command += ["-chardev", "socket,id=v%s,path=/work/%s.sock,server=on,wait=off" % (short, short),
                    "-device", "virtserialport,chardev=v%s,name=cdj3k.%s" % (short, native)]
    if jog_output:
        command += ['-object','memory-backend-file,id=jogmem,size=1048576,mem-path=/work/jog.shm,share=on',
                    '-device','ivshmem-plain,memdev=jogmem']
    return command


def ppm_ready(data):
    match = re.match(rb"P6\s+(1280)\s+(800)\s+255\s", data)
    if not match or len(data) != match.end() + 1280 * 800 * 3:
        return False
    raster = data[match.end():]
    pixels = [raster[i:i + 3] for i in range(0, len(raster), 3 * 97)]
    return len(set(pixels)) >= 8 and sum(pixel != b"\0\0\0" for pixel in pixels) > len(pixels) // 100


def qmp_capture(port):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as connection:
        stream = connection.makefile("rwb")
        if "QMP" not in json.loads(stream.readline(65537)):
            raise RuntimeError("invalid QMP greeting")
        for command in ({"execute": "qmp_capabilities"},
                        {"execute": "screendump", "arguments": {"filename": "/work/screen.ppm"}}):
            stream.write(json.dumps(command).encode() + b"\n"); stream.flush()
            for _ in range(64):
                reply = json.loads(stream.readline(65537))
                if "error" in reply:
                    raise RuntimeError("QMP capture failed")
                if "return" in reply:
                    break
            else:
                raise RuntimeError("QMP response limit")


def run(base, archive, output, profile, port, qmp, vcpus=4, memory_bytes=2147483648,
        native_tests=True, baseline_seconds=60, capture_pcm=False,
        runtime_package=None, runtime_seconds=480, loaded_context=None,
        native_source_root=None,controlled_runtime=None,jog_output=False,retain_vm=False):
    """Run one owned guest; retention transfers cleanup and output lifetime to caller."""
    if type(jog_output) is not bool:raise ValueError('jog output must be an explicit boolean')
    if type(retain_vm) is not bool:raise ValueError('VM retention must be an explicit boolean')
    if output.exists() or profile not in FIRMWARE:
        raise ValueError("new output and supported profile required")
    for name in ("Image", "initramfs-patched.cpio.gz", "az-mnt.img", "usb.img", "ssh/az-emu_ed25519"):
        if not (base / name).is_file():
            raise ValueError("missing emulator input: " + name)
    if type(baseline_seconds) is not int or not 2 <= baseline_seconds <= 300:
        raise ValueError("baseline duration must be between 2 and 300 seconds")
    source_root=Path(__file__).resolve().parents[2]
    native_root=source_root if native_source_root is None else Path(native_source_root).resolve()
    if native_tests:
        validate_package(archive, native_root)
    if controlled_runtime is not None and (not callable(controlled_runtime) or runtime_package is None or loaded_context is not None):
        raise ValueError('controlled runtime requires its own callable, package and separate scope')
    if loaded_context is not None:
        if runtime_package is None or not isinstance(loaded_context,dict):
            raise ValueError('loaded context requires an SDK runtime package and mapping')
        if not callable(loaded_context.get('prepare_track')) or not (callable(loaded_context.get('send_control')) or callable(loaded_context.get('panel_transport_factory'))):
            raise ValueError('loaded context requires bounded selection and panel callbacks')
    driver_bytes=None
    if loaded_context is not None and ('driver_path' in loaded_context or 'driver_sha256' in loaded_context):
        driver=Path(loaded_context.get('driver_path',''))
        if driver.is_symlink() or not driver.is_file() or driver.stat().st_size>131072:
            raise ValueError('panel driver must be present before boot')
        driver_bytes=driver.read_bytes()
        if hashlib.sha256(driver_bytes).hexdigest()!=loaded_context.get('driver_sha256'):
            raise ValueError('panel driver source identity mismatch before boot')
    if runtime_package is not None:
        # Lazy import preserves existing frozen baseline/resource runners, which
        # do not request or copy the optional in-process controller.
        import sdk_emulator_sdk
        if loaded_context is None:
            sdk_emulator_sdk.validate_package(runtime_package,Path(__file__).resolve().parents[2])
        else:
            import sdk_emulator_loaded
            sdk_emulator_loaded.validate_loaded_package(runtime_package,Path(__file__).resolve().parents[2],loaded_context.get('position_binding'))
        if type(runtime_seconds) is not int or not 120<=runtime_seconds<=1800:
            raise ValueError('runtime duration must be 120..1800 seconds')
    output.mkdir(parents=True)
    if driver_bytes is not None:
        frozen_driver=output/'panel-driver.py'
        with frozen_driver.open('xb') as stream:stream.write(driver_bytes)
        report_driver={'path':str(frozen_driver),'source_path':str(driver),'sha256':hashlib.sha256(driver_bytes).hexdigest()}
        (output/'panel-driver-binding.json').write_text(json.dumps(report_driver,indent=2)+'\n')
    work = output / "vm"; work.mkdir()
    if jog_output:prepare_jog_output(work)
    token = uuid.uuid4().hex
    name = "az-sdk-automation-" + profile + "-" + token[:12]
    report = {"profile": profile, "status": "INCOMPLETE", "container": name,
              "scope": "isolated emulator kernel tests and shim-only firmware baseline; no native SDK feature acceptance"}
    report['source_roots']={'native':str(native_root),'runtime':str(source_root)}
    report['resources'] = {'vcpus': vcpus, 'guest_memory_bytes': memory_bytes}
    if not native_tests:
        report['scope'] = 'shim-only stock firmware baseline; no SDK kernel tests or SDK feature acceptance'
        report['kernel_tests'] = {'status': 'INCOMPLETE', 'reason': 'not requested in baseline-only experiment'}

    def command(argv, **kwargs):
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=kwargs.pop("timeout", 60), **kwargs)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[-1000:])
        return result.stdout

    try:
        memory = command(["docker", "run", "--rm", "alpine:3.20", "cat", "/proc/meminfo"]).decode()
        available = int(re.search(r"^MemAvailable:\s+([0-9]+)", memory, re.M)[1])
        args = boot_arguments(base, work, name, token, port, qmp, available, vcpus, memory_bytes,jog_output=jog_output)
        report["kernel_sha256"] = digest(base / "Image")
        report["initramfs_sha256"] = digest(base / "initramfs-patched.cpio.gz")
        report["base_disks"] = {n: {"size": (base/n).stat().st_size, "mtime_ns": (base/n).stat().st_mtime_ns}
                                for n in ("az-mnt.img", "usb.img")}
        for disk in ("az-mnt", "usb"):
            command(["docker", "run", "--rm", "--platform", "linux/amd64", "--entrypoint", "/qemu-build/qemu-img",
                     "-v", str(base) + ":/base:ro", "-v", str(work) + ":/work", "az-sdk-emulator-qemu:local",
                     "create", "-f", "qcow2", "-F", "raw", "-b", "/base/" + disk + ".img", "/work/" + disk + ".qcow2"])
        command(args)
        report["boot_arguments"] = args
        key, known = base / "ssh/az-emu_ed25519", output / "known_hosts"
        ssh = ssh_arguments("127.0.0.1", port, key, known)
        deadline = time.monotonic() + 240
        while True:
            scan = subprocess.run(["ssh-keyscan", "-T", "5", "-p", str(port), "127.0.0.1"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            keys = scanned_host_keys(scan.stdout,port)
            if keys:
                # This port belongs to the just-created, nonce-labelled local VM.
                # Dropbear generates fresh RAM host keys per boot; pin this boot only.
                if not known.exists():
                    known.write_bytes(keys)
                try:
                    command(ssh + ["true"], timeout=12)
                    break
                except RuntimeError:
                    pass
            if compare_timestamps(time.monotonic(), deadline, units_per_second=1) >= 0:
                raise RuntimeError("emulator SSH boot deadline")
            time.sleep(1)
        remote = lambda text: command(ssh + [text], timeout=40).decode()
        actual = remote("sha256sum /home/root/pdj/EP147").split()[0]
        if actual != FIRMWARE[profile]:
            raise RuntimeError("unexpected emulator firmware identity")
        report["firmware_sha256"] = actual
        if native_tests:
            print("Emulator%s: native packet starting" % profile, flush=True)
            report["kernel_tests"] = execute(archive, native_root, "127.0.0.1", port,
                                              key, known, output / "kernel-tests", 1800, preflight_timeout=120)
        if capture_pcm:
            (output / 'guest-identity.txt').write_text(remote('uname -a; cat /proc/modules; sha256sum /home/root/pdj/EP147 /home/root/ep122_shim.so /usr/lib/modules/virtio_snd.ko /etc/asound.conf; cat /proc/asound/cards; cat /proc/asound/pcm; cat /proc/cmdline'))
            (output / 'host-before.txt').write_bytes(command(['docker', 'exec', name, 'sh', '-c', 'cat /sys/fs/cgroup/memory.events; cat /sys/fs/cgroup/memory.current; cat /sys/fs/cgroup/cpu.stat; cat /sys/fs/cgroup/cpu.max']))
        # Copy the vendor startup script into RAM, remove only its preload lines,
        # and copy the base unit. The original service remains masked throughout.
        setup = '''set -e
mkdir -p /tmp/az-sdk-baseline
grep -v '^export LD_PRELOAD=' /home/root/scripts/apl_start.sh >/tmp/az-sdk-baseline/start.body
if grep -q LD_PRELOAD /tmp/az-sdk-baseline/start.body; then exit 91; fi
head -n 1 /tmp/az-sdk-baseline/start.body >/tmp/az-sdk-baseline/start.sh
printf '%s\n' 'export LD_PRELOAD=/home/root/ep122_shim.so' >>/tmp/az-sdk-baseline/start.sh
tail -n +2 /tmp/az-sdk-baseline/start.body >>/tmp/az-sdk-baseline/start.sh
chmod 755 /tmp/az-sdk-baseline/start.sh
for unit in /etc/systemd/system/EP147.service /lib/systemd/system/EP147.service; do
 if test -f "$unit"; then cp "$unit" /run/systemd/system/az-sdk-baseline.service; break; fi
done
sed -i 's|^ExecStart=.*|ExecStart=/tmp/az-sdk-baseline/start.sh|;s/^Restart=.*/Restart=no/' /run/systemd/system/az-sdk-baseline.service
systemctl daemon-reload
systemctl start az-sdk-baseline.service --no-block
'''
        remote(setup)
        initial=await_baseline(remote)
        try:
            identity=baseline_identity(remote,initial)
        except ValueError as failure:
            report['baseline']={'status':'FAIL','errors':[str(failure)],'scope':'pinned principal process/service health only'}
            raise
        report['baseline_identity']=identity
        health_command=baseline_health_command(identity['pid'],identity['birth'])
        samples = [{'pid':identity['pid'],'service':identity['service'],'local_monotonic':identity['captured_monotonic']}]
        end = time.monotonic() + baseline_seconds
        for index in range(baseline_seconds * 2):
            text = remote(health_command)
            lines = text.splitlines()
            samples.append({"pid": next((line for line in lines if "=" not in line), ""),
                            "service": dict(line.split("=", 1) for line in lines if "=" in line), "local_monotonic": time.monotonic()})
            if capture_pcm and index % 5 == 0:
                pcm = remote('cat /proc/uptime; cat /proc/loadavg; grep -E "MemAvailable|MemFree" /proc/meminfo; for p in /proc/asound/card*/pcm*/sub*/status /proc/asound/card*/pcm*/sub*/hw_params; do test ! -f "$p" || { echo "$p"; cat "$p"; }; done; cat /proc/pressure/cpu /proc/pressure/memory 2>/dev/null || true')
                with (output / 'pcm-samples.txt').open('a') as stream:
                    stream.write('SAMPLE %d\n%s\n' % (index, pcm))
            if compare_timestamps(time.monotonic(), end, units_per_second=1) >= 0:
                break
            if len(samples) >= 2 and all(not s['pid'] and s['service'].get('ActiveState') in ('failed','inactive') for s in samples[-2:]):
                break
            time.sleep(1)
        (output / "baseline-samples.json").write_text(json.dumps(samples, indent=2) + "\n")
        report["baseline"] = validate_health(samples)
        (output / "baseline-journal.txt").write_text(remote("journalctl -u az-sdk-baseline.service --no-pager -n 1200"))
        if capture_pcm:
            (output / 'guest-after.txt').write_text(remote('dmesg | tail -n 100; cat /proc/interrupts; cat /proc/asound/timers'))
            (output / 'host-after.txt').write_bytes(command(['docker', 'exec', name, 'sh', '-c', 'cat /sys/fs/cgroup/memory.events; cat /sys/fs/cgroup/memory.current; cat /sys/fs/cgroup/cpu.stat; cat /sys/fs/cgroup/cpu.max']))
            report['container_state'] = json.loads(command(['docker','inspect','--format','{{json .State}}',name]).decode())
            report['qemu_image_id'] = command(['docker','inspect','--format','{{.Image}}',name]).decode().strip()
        qmp_capture(qmp)
        report["baseline"] = baseline_result(report["baseline"], ppm_ready((work / "screen.ppm").read_bytes()))
        if runtime_package is not None:
            (output/'baseline-screen.ppm').write_bytes((work/'screen.ppm').read_bytes())
            phase='controlled_runtime' if controlled_runtime is not None else 'native_runtime' if loaded_context is None else 'loaded_runtime'
            phase_dir=output/phase.replace('_','-')
            if controlled_runtime is not None:
                report['scope']='stock baseline and explicitly scoped owned runtime checks; no implied loaded-track acceptance'
                owned_context=dict(container=name,token=token,work_dir=work,qmp_port=qmp)
                activate=lambda: controlled_runtime(ssh,runtime_package,source_root,phase_dir,FIRMWARE[profile],runtime_seconds,command,owned_context)
            elif loaded_context is None:
                report['scope']='standalone checks, stock baseline and all-deck SDK scalar observations; no loaded-track/command/persistence acceptance'
                activate=lambda: sdk_emulator_sdk.run_runtime(ssh,runtime_package,Path(__file__).resolve().parents[2],
                                                              phase_dir,FIRMWARE[profile],runtime_seconds,command)
            else:
                report['scope']='standalone checks, stock baseline and all-deck loaded scalar/control observations; no source-binding/commands/persistence/audio acceptance'
                owned_context=dict(loaded_context,container=name,token=token,work_dir=work,qmp_port=qmp)
                if driver_bytes is not None:owned_context['driver_path']=frozen_driver
                activate=lambda: sdk_emulator_loaded.run_loaded_runtime(ssh,runtime_package,Path(__file__).resolve().parents[2],
                                                                        phase_dir,FIRMWARE[profile],runtime_seconds,command,owned_context)
            report[phase]=sdk_emulator_sdk.after_baseline(report['baseline'],activate)
            if report['baseline']['status']=='PASS':
                qmp_capture(qmp)
                report[phase]=baseline_result(report[phase],ppm_ready((work/'screen.ppm').read_bytes()))
                phase_dir.mkdir(exist_ok=True)
                (phase_dir/'report.json').write_text(json.dumps(report[phase],indent=2)+'\n')
        report["status"] = acceptance_status(report["kernel_tests"], report["baseline"])
    except (RuntimeError, ValueError, subprocess.TimeoutExpired, OSError) as error:
        report["error"] = str(error)
    finally:
        # Preserve a recorded kernel failure even if a later baseline read fails.
        report["status"] = (acceptance_status(report.get("kernel_tests", {}), report.get("baseline", {})) if native_tests
                            else report.get('baseline', {}).get('status', 'INCOMPLETE'))
        if runtime_package is not None:
            phase='controlled_runtime' if controlled_runtime is not None else 'native_runtime' if loaded_context is None else 'loaded_runtime'
            report['status']=acceptance_status({'status':report['status']},{'status':'PASS'},**{phase:report.get(phase,{})})
        if "error" in report and report["status"] != "FAIL":
            report["status"] = "INCOMPLETE"
        try:
            owner = command(["docker", "inspect", "--format", '{{index .Config.Labels "az-sdk-token"}}', name]).decode().strip()
            if owner == token:
                if retain_vm:
                    report.update(owned_vm_removed=False,owned_vm_retained=True)
                    report['retained_vm_context']=dict(container=name,token=token,work_dir=str(work),qmp_port=qmp,
                                                       ssh=ssh if 'ssh' in locals() else None)
                else:
                    command(["docker", "rm", "-f", name])
                    report["owned_vm_removed"] = True
        except (RuntimeError, subprocess.TimeoutExpired):
            pass
        if "base_disks" in report:
            report["base_disks_unchanged"] = all((base/n).stat().st_size == s["size"] and compare_timestamps((base / n).stat().st_mtime_ns, s['mtime_ns']) == 0
                                                 for n, s in report["base_disks"].items())
            if not report["base_disks_unchanged"]:
                report["status"] = "FAIL"
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--profile", choices=FIRMWARE, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--qmp-port", type=int, required=True)
    parser.add_argument('--runtime-package',type=Path)
    parser.add_argument('--runtime-seconds',type=int,default=480)
    parser.add_argument('--jog-output',action='store_true',help='expose the existing shim JOG1 publisher; no deck/Slip meaning is implied')
    args = parser.parse_args()
    result = run(args.base.resolve(), args.archive.resolve(), args.output.resolve(), args.profile, args.port, args.qmp_port,
                 runtime_package=args.runtime_package.resolve() if args.runtime_package else None,runtime_seconds=args.runtime_seconds,jog_output=args.jog_output)
    print(json.dumps(result, indent=2))
    raise SystemExit({"PASS": 0, "FAIL": 1, "INCOMPLETE": 2}[result["status"]])
