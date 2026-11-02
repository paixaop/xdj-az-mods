#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
import base64
import hashlib
import struct
import subprocess
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/integration"))
spec = importlib.util.spec_from_file_location("sdk_emulator_smoke", ROOT / "tests/integration/sdk_emulator_smoke.py")
emulator = importlib.util.module_from_spec(spec); spec.loader.exec_module(emulator)


class EmulatorTests(unittest.TestCase):
    def test_native_packet_uses_its_explicit_frozen_source_root(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t).resolve()
            report,_=self.baseline_case('children',native_source_root=root)
            self.assertEqual(report['kernel_tests']['status'],'PASS')
            self.assertEqual(report['source_roots']['native'],str(root))
            self.assertNotEqual(report['source_roots']['runtime'],str(root))

    def baseline_case(self,mutation,native_source_root=None,jog_output=False,retain_vm=False):
        family=b'ssh-ed25519';blob=struct.pack('>I',len(family))+family+struct.pack('>I',32)+b'x'*32
        key=b'[127.0.0.1]:2360 ssh-ed25519 '+base64.b64encode(blob)+b'\n'
        with tempfile.TemporaryDirectory() as t:
            base=Path(t)/'base';(base/'ssh').mkdir(parents=True)
            for name in ['Image','initramfs-patched.cpio.gz','az-mnt.img','usb.img','ssh/az-emu_ed25519']:(base/name).write_bytes(b'x')
            output=Path(t)/'out';inventory=[0];pinned=[0];commands=[]
            def command(argv,**kwargs):
                value=b'';text=argv[-1];commands.append(text)
                if argv[:2]==['docker','rm']:commands.append('OWNED_VM_DELETE')
                if argv[:2]==['docker','run'] and '-machine' in argv:
                    backing=output/'vm/jog.shm'
                    self.assertEqual(backing.exists(),jog_output)
                    if jog_output:
                        self.assertEqual(backing.stat().st_size,1<<20)
                        self.assertEqual(backing.read_bytes(),bytes(1<<20))
                        self.assertIn('memory-backend-file,id=jogmem,size=1048576,mem-path=/work/jog.shm,share=on',argv)
                        self.assertIn('ivshmem-plain,memdev=jogmem',argv)
                service='NRestarts=0\nActiveState=active\nSubState=running\n'
                if argv[0]=='ssh-keyscan':value=key
                elif argv[:2]==['docker','inspect']:value=(b'b' if mutation=='foreign-owner' else b'a')*32
                elif text=='/proc/meminfo':value=b'MemAvailable: 8388608 kB\n'
                elif text=='sha256sum /home/root/pdj/EP147':value=emulator.FIRMWARE['130'].encode()+b'  EP147\n'
                elif text.startswith('pidof EP147'):
                    inventory[0]+=1
                    pid='934 658' if mutation=='ambiguous' or inventory[0]>1 else '658'
                    value=(pid+'\n'+service).encode()
                elif text.startswith("awk '{print $22}' /proc/658/stat"):
                    value=b'' if mutation=='capture-death' else b'42\n'
                elif text.startswith('if test -r /proc/658/stat'):
                    pinned[0]+=1
                    pid='' if mutation=='capture-race' or (mutation in ['loss','reuse'] and pinned[0]>1) else '658\n'
                    if mutation=='service' and pinned[0]>1:service=service.replace('ActiveState=active','ActiveState=failed').replace('SubState=running','SubState=failed')
                    if mutation=='restart' and pinned[0]>1:service=service.replace('NRestarts=0','NRestarts=1')
                    value=(pid+service).encode()
                return SimpleNamespace(returncode=0,stdout=value,stderr=b'')
            def screen(port):(output/'vm/screen.ppm').write_bytes(b'P6\n1280 800\n255\n'+bytes(range(256))*12000)
            def validate(archive,root):
                self.assertEqual(root,native_source_root)
                return {'archive_sha256':'a'*64,'source_inputs':73}
            def execute(archive,root,*args,**kwargs):
                self.assertEqual(root,native_source_root)
                return {'status':'PASS'}
            with patch.object(emulator.subprocess,'run',side_effect=command), \
                 patch.object(emulator.uuid,'uuid4',return_value=SimpleNamespace(hex='a'*32)), \
                 patch.object(emulator.time,'sleep'),patch.object(emulator.time,'monotonic',side_effect=range(10000)), \
                 patch.object(emulator,'validate_package',side_effect=validate), \
                 patch.object(emulator,'execute',side_effect=execute), \
                 patch.object(emulator,'qmp_capture',side_effect=screen):
                options={} if native_source_root is None else {'native_source_root':native_source_root}
                if jog_output:options['jog_output']=True
                if retain_vm:options['retain_vm']=True
                report=emulator.run(base,None,output,'130',2360,4460,native_tests=native_source_root is not None,baseline_seconds=8,**options)
            return report,commands

    def test_explicit_retention_keeps_owned_vm_and_default_cleanup_remains(self):
        kept,calls=self.baseline_case('children',retain_vm=True)
        self.assertTrue(kept['owned_vm_retained']);self.assertFalse(kept['owned_vm_removed'])
        self.assertNotIn('OWNED_VM_DELETE',calls)
        removed,calls=self.baseline_case('children')
        self.assertTrue(removed['owned_vm_removed']);self.assertNotIn('owned_vm_retained',removed)
        self.assertIn('OWNED_VM_DELETE',calls)
        foreign,calls=self.baseline_case('foreign-owner',retain_vm=True)
        self.assertNotIn('owned_vm_retained',foreign);self.assertNotIn('owned_vm_removed',foreign)
        self.assertNotIn('OWNED_VM_DELETE',calls)

    def test_actual_run_creates_fresh_owned_jog_backing_only_when_requested(self):
        for requested in (False,True):
            with self.subTest(requested=requested):
                report,_=self.baseline_case('children',jog_output=requested)
                self.assertEqual(report['baseline']['status'],'PASS',report)
                self.assertTrue(report['owned_vm_removed'])

    def test_jog_output_option_is_strict_and_default_launch_is_unchanged(self):
        args=(Path('/base'),Path('/owned-work'),'az-sdk-automation-unit','a'*32,2230,4450,8*1024*1024)
        default=emulator.boot_arguments(*args)
        self.assertEqual(default,emulator.boot_arguments(*args,jog_output=False))
        enabled=emulator.boot_arguments(*args,jog_output=True)
        self.assertEqual(enabled[:-4],default)
        self.assertEqual(enabled[-4:],['-object','memory-backend-file,id=jogmem,size=1048576,mem-path=/work/jog.shm,share=on',
                                      '-device','ivshmem-plain,memdev=jogmem'])
        for bad in (1,'yes',None):
            with self.subTest(bad=bad),self.assertRaises(ValueError):emulator.boot_arguments(*args,jog_output=bad)

    def test_jog_backing_refuses_existing_files_and_symlinks(self):
        with tempfile.TemporaryDirectory() as t:
            work=Path(t)
            target=work/'existing';target.write_bytes(b'preserve')
            for link in (False,True):
                backing=work/'jog.shm'
                if link:backing.symlink_to(target)
                else:backing.write_bytes(b'preserve')
                with self.subTest(link=link),self.assertRaises(FileExistsError):emulator.prepare_jog_output(work)
                self.assertEqual(target.read_bytes(),b'preserve')
                backing.unlink()

    def test_baseline_keeps_principal_identity_when_same_name_fork_children_appear(self):
        report,commands=self.baseline_case('children')
        self.assertEqual(report['baseline']['status'],'PASS',report)
        self.assertEqual(report['baseline_identity']['pid'],'658')
        self.assertEqual(report['baseline_identity']['birth'],'42')
        self.assertEqual(sum(c.startswith('pidof EP147') for c in commands),1)
        checks=[c for c in commands if c.startswith('if test -r /proc/658/stat')]
        self.assertGreaterEqual(len(checks),3)
        self.assertTrue(all("/proc/658/stat)\" = 42" in c for c in checks))

    def test_baseline_rejects_initial_ambiguity_capture_death_reuse_loss_and_service_failures(self):
        for mutation in ['ambiguous','capture-death','capture-race','reuse','loss','service','restart']:
            with self.subTest(mutation=mutation):
                report,_=self.baseline_case(mutation)
                self.assertEqual(report['baseline']['status'],'FAIL',report)

    def test_baseline_capture_requires_bounded_forward_finite_clock(self):
        state='658\nNRestarts=0\nActiveState=active\nSubState=running\n'
        def remote(command):return '42\n' if command.startswith('awk ') else state
        for clocks in [[float('nan')],[float('inf')],[-1],[True],[5,4],[5,36],[5,5,float('nan')],[5,5,36]]:
            with self.subTest(clocks=clocks),patch.object(emulator.time,'monotonic',side_effect=clocks):
                with self.assertRaises(ValueError):emulator.baseline_identity(remote,state)

    def test_pinned_birth_guard_checks_actual_proc_stat_live_and_dead_states(self):
        command=emulator.baseline_health_command('658','42')
        program=command.split("awk '",1)[1].split("'",1)[0]
        for state in ['R','S','D','T','Z','X','x']:
            text='658 (EP147) '+state+' '+' '.join(['0']*18+['42'])+'\n'
            result=subprocess.run(['awk',program],input=text.encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            with self.subTest(state=state):
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(result.stdout.strip(),b'' if state in ['Z','X','x'] else b'42')
        for pid,birth in [('658;true','42'),('658','42;true'),('0','42'),('658','0')]:
            with self.assertRaises(ValueError):emulator.baseline_health_command(pid,birth)

    def test_requested_native_runtime_is_an_additional_required_gate(self):
        for baseline,native,expected in [('PASS','PASS','PASS'),('PASS','FAIL','FAIL'),
                                         ('PASS','INCOMPLETE','INCOMPLETE'),('FAIL','PASS','FAIL')]:
            self.assertEqual(emulator.acceptance_status({'status':'PASS'},{'status':baseline},{'status':native}),expected)

    def test_loaded_runtime_is_a_required_gate_and_cannot_hide_other_failures(self):
        for kernel,baseline,loaded,expected in [('PASS','PASS','PASS','PASS'),('PASS','PASS','FAIL','FAIL'),
                                              ('PASS','PASS','INCOMPLETE','INCOMPLETE'),('FAIL','PASS','PASS','FAIL'),('PASS','FAIL','PASS','FAIL')]:
            self.assertEqual(emulator.acceptance_status({'status':kernel},{'status':baseline},loaded_runtime={'status':loaded}),expected)

    def test_loaded_context_requires_a_package_and_callbacks_before_vm_execution(self):
        with tempfile.TemporaryDirectory() as t:
            base=Path(t)/'base';(base/'ssh').mkdir(parents=True)
            for name in ['Image','initramfs-patched.cpio.gz','az-mnt.img','usb.img','ssh/az-emu_ed25519']:(base/name).write_bytes(b'x')
            for package,context in [(None,{}),(Path(t)/'package',[]),(Path(t)/'package',{}),
                                    (Path(t)/'package',{'prepare_track':lambda c:None})]:
                with self.subTest(context=context),patch.object(emulator.subprocess,'run') as remote:
                    with self.assertRaises(ValueError):
                        emulator.run(base,None,Path(t)/'out','130',2360,4460,native_tests=False,
                                     runtime_package=package,loaded_context=context)
                    remote.assert_not_called()

    def test_loaded_activation_uses_only_owned_context_after_baseline_and_requires_display(self):
        self.activation_uses_owned_context(False)

    def test_controlled_runtime_has_its_own_scope_and_owned_identity_and_required_failure_gate(self):
        self.activation_uses_owned_context(True)

    def activation_uses_owned_context(self,controlled):
        import sdk_emulator_sdk as sdk
        import sdk_emulator_loaded as loaded
        family=b'ssh-ed25519';blob=struct.pack('>I',len(family))+family+struct.pack('>I',32)+b'x'*32
        key=b'[127.0.0.1]:2360 ssh-ed25519 '+base64.b64encode(blob)+b'\n'
        for baseline,native,display,expected in [('FAIL','PASS',True,'FAIL'),('PASS','FAIL',True,'FAIL'),
                                                ('PASS','INCOMPLETE',True,'INCOMPLETE'),('PASS','PASS',True,'PASS'),
                                                ('PASS','PASS',False,'INCOMPLETE')]:
            with self.subTest(baseline=baseline,native=native,display=display),tempfile.TemporaryDirectory() as t:
                base=Path(t)/'base';(base/'ssh').mkdir(parents=True)
                for name in ['Image','initramfs-patched.cpio.gz','az-mnt.img','usb.img','ssh/az-emu_ed25519']:(base/name).write_bytes(b'x')
                output=Path(t)/'out';screens=[0];phase_calls=[]
                phase='controlled_runtime' if controlled else 'loaded_runtime';phase_dir=phase.replace('_','-')
                driver=Path(t)/'az-drive.py';driver.write_bytes(b'approved driver bytes')
                driver_sha=hashlib.sha256(driver.read_bytes()).hexdigest()
                context={'position_binding':{},'prepare_track':lambda c:None,'send_control':lambda payload:None,
                         'driver_path':driver,'driver_sha256':driver_sha,
                         'container':'unrelated','token':'b'*32,'work_dir':'/unrelated','qmp_port':9999}
                def command(argv,**kwargs):
                    value=b''
                    if not controlled and argv[0]=='ssh' and 'systemctl start az-sdk-baseline.service' in argv[-1]:
                        driver.unlink()
                    if argv[0]=='ssh-keyscan':value=key
                    elif argv[:2]==['docker','inspect']:value=b'a'*32
                    elif argv[-1]=='/proc/meminfo':value=b'MemAvailable: 8388608 kB\n'
                    elif argv[-1]=='sha256sum /home/root/pdj/EP147':value=emulator.FIRMWARE['130'].encode()+b'  EP147\n'
                    elif argv[-1].startswith('pidof EP147'):value=b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                    elif argv[-1].startswith("awk '{print $22}' /proc/123/stat"):value=b'42\n'
                    elif argv[-1].startswith('if test -r /proc/123/stat'):value=b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                    return SimpleNamespace(returncode=0,stdout=value,stderr=b'')
                def screen(port):
                    screens[0]+=1
                    pixels=bytes(range(256))*12000 if display or screens[0]==1 else b'\0'*3072000
                    (output/'vm/screen.ppm').write_bytes(b'P6\n1280 800\n255\n'+pixels)
                def runtime_phase(*args):
                    phase_calls.append(True)
                    self.assertEqual(args[3],output/phase_dir)
                    owned=args[7]
                    self.assertEqual(owned['container'],'az-sdk-automation-130-'+'a'*12)
                    self.assertEqual(owned['token'],'a'*32);self.assertEqual(owned['work_dir'],output/'vm');self.assertEqual(owned['qmp_port'],4460)
                    if not controlled:self.assertIs(owned['prepare_track'],context['prepare_track'])
                    if not controlled:
                        self.assertFalse(driver.exists(),'mutable checkout disappeared during baseline')
                        frozen=Path(owned['driver_path'])
                        self.assertEqual(frozen,output/'panel-driver.py')
                        self.assertEqual(frozen.read_bytes(),b'approved driver bytes')
                        self.assertEqual(owned['driver_sha256'],driver_sha)
                    (output/phase_dir).mkdir()
                    return {'status':native}
                with patch.object(emulator.subprocess,'run',side_effect=command), \
                     patch.object(emulator.uuid,'uuid4',return_value=SimpleNamespace(hex='a'*32)), \
                     patch.object(emulator.time,'sleep'),patch.object(emulator.time,'monotonic',side_effect=range(10000)), \
                     patch.object(emulator,'qmp_capture',side_effect=screen), \
                     patch.object(emulator,'validate_health',return_value={'status':baseline}), \
                     patch.object(loaded,'validate_loaded_package',return_value={}), \
                     patch.object(sdk,'validate_package',return_value={}), \
                     patch.object(loaded,'run_loaded_runtime',side_effect=runtime_phase) as activate, \
                     patch.object(sdk,'run_runtime') as empty:
                    result=emulator.run(base,None,output,'130',2360,4460,native_tests=False,baseline_seconds=2,
                                        runtime_package=Path(t)/'package',runtime_seconds=120,loaded_context=None if controlled else context,
                                        controlled_runtime=runtime_phase if controlled else None)
                self.assertEqual(bool(phase_calls),baseline=='PASS');empty.assert_not_called()
                self.assertEqual(activate.called,baseline=='PASS' and not controlled)
                self.assertEqual(result['status'],expected,result)
                self.assertIn(phase,result);self.assertNotIn('native_runtime',result)
                if controlled:self.assertNotIn('loaded_runtime',result)
                self.assertEqual(context['container'],'unrelated');self.assertEqual(context['token'],'b'*32)
                self.assertTrue(result['owned_vm_removed'])

    def test_runtime_activation_waits_for_baseline_and_failure_survives_cleanup(self):
        import sdk_emulator_sdk as sdk
        family=b'ssh-ed25519';blob=struct.pack('>I',len(family))+family+struct.pack('>I',32)+b'x'*32
        key=b'[127.0.0.1]:2360 ssh-ed25519 '+base64.b64encode(blob)+b'\n'
        for baseline,native in [('FAIL','PASS'),('PASS','FAIL'),('PASS','INCOMPLETE'),('PASS','PASS')]:
            with self.subTest(baseline=baseline,native=native),tempfile.TemporaryDirectory() as t:
                base=Path(t)/'base';(base/'ssh').mkdir(parents=True)
                for name in ['Image','initramfs-patched.cpio.gz','az-mnt.img','usb.img','ssh/az-emu_ed25519']:(base/name).write_bytes(b'x')
                output=Path(t)/'out'
                def command(argv,**kwargs):
                    value=b''
                    if argv[0]=='ssh-keyscan':value=key
                    elif argv[:2]==['docker','inspect']:value=b'a'*32
                    elif argv[-1]=='/proc/meminfo':value=b'MemAvailable: 8388608 kB\n'
                    elif argv[-1]=='sha256sum /home/root/pdj/EP147':value=emulator.FIRMWARE['130'].encode()+b'  EP147\n'
                    elif argv[-1].startswith('pidof EP147'):value=b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                    elif argv[-1].startswith("awk '{print $22}' /proc/123/stat"):value=b'42\n'
                    elif argv[-1].startswith('if test -r /proc/123/stat'):value=b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                    return SimpleNamespace(returncode=0,stdout=value,stderr=b'')
                def screen(port):(output/'vm/screen.ppm').write_bytes(b'P6\n1280 800\n255\n'+bytes(range(256))*12000)
                def runtime_phase(*args):
                    (output/'native-runtime').mkdir()
                    return {'status':native}
                with patch.object(emulator.subprocess,'run',side_effect=command), \
                     patch.object(emulator.uuid,'uuid4',return_value=SimpleNamespace(hex='a'*32)), \
                     patch.object(emulator.time,'sleep'),patch.object(emulator.time,'monotonic',side_effect=range(10000)), \
                     patch.object(emulator,'qmp_capture',side_effect=screen), \
                     patch.object(emulator,'validate_health',return_value={'status':baseline}), \
                     patch.object(sdk,'validate_package',return_value={}), \
                     patch.object(sdk,'run_runtime',side_effect=runtime_phase) as activate:
                    result=emulator.run(base,None,output,'130',2360,4460,native_tests=False,
                                        baseline_seconds=2,runtime_package=Path(t)/'package',runtime_seconds=120)
                self.assertEqual(activate.called,baseline=='PASS')
                self.assertEqual(result['status'],'FAIL' if 'FAIL' in [baseline,native] else native)
                self.assertTrue(result['owned_vm_removed'])

    def test_keyscan_banner_or_wrong_endpoint_cannot_be_pinned(self):
        family=b'ssh-ed25519'
        blob=struct.pack('>I',len(family))+family+struct.pack('>I',32)+b'x'*32
        line=b'[127.0.0.1]:2360 ssh-ed25519 '+base64.b64encode(blob)+b'\n'
        comment=b'# 127.0.0.1:2360 SSH-2.0-dropbear_2024.86\n'
        self.assertEqual(emulator.scanned_host_keys(comment,2360),b'')
        self.assertEqual(emulator.scanned_host_keys(comment+line,2360),line)
        self.assertEqual(emulator.scanned_host_keys(line,2361),b'')
        self.assertEqual(emulator.scanned_host_keys(line.replace(b'ssh-ed25519 ',b'ssh-rsa '),2360),b'')
        self.assertEqual(emulator.scanned_host_keys(b'[127.0.0.1]:2360 ssh-ed25519 invalid\n',2360),b'')
    def test_refuses_memory_exhaustion_and_invalid_resource_values(self):
        for value in (None, True, -1, 0, (2304 + 512) * 1024 - 1):
            with self.assertRaises(ValueError): emulator.memory_options(value)
        self.assertIn("--memory=2304m", emulator.memory_options((2304 + 512) * 1024))
        self.assertIn("--memory-swap=2304m", emulator.memory_options(8 * 1024 * 1024))

    def test_vm_only_writes_overlays_and_binds_loopback_ports(self):
        args = emulator.boot_arguments(Path("/base-source"), Path("/owned-work"), "az-sdk-automation-unit", "a" * 32,
                                       2230, 4450, 8 * 1024 * 1024)
        self.assertIn("/base-source:/base:ro", args)
        self.assertIn("/owned-work:/work", args)
        self.assertIn("127.0.0.1:2230:2222", args)
        self.assertIn("127.0.0.1:4450:4446", args)
        drives = [args[i + 1] for i, value in enumerate(args[:-1]) if value == "-drive"]
        self.assertEqual(len(drives), 2)
        self.assertTrue(all("/work/" in value and "format=qcow2" in value for value in drives))
        self.assertIn("2048M", args)
        self.assertTrue(any("systemd.mask=EP147.service" in value for value in args))

    def test_resource_comparison_changes_only_cpu_and_memory_values(self):
        base = Path("/base"); work = Path("/work")
        args = []
        for cpus, memory in ((4, 2147483648), (6, 2147483648), (4, 4038197248), (6, 4038197248)):
            value = emulator.boot_arguments(base, work, "az-sdk-automation-unit", "a"*32, 2230, 4450,
                                             8*1024*1024, vcpus=cpus, memory_bytes=memory)
            self.assertEqual(value[value.index("-smp")+1], str(cpus))
            self.assertEqual(value[value.index("-m")+1], "2048M" if memory == 2147483648 else "4038197248B")
            self.assertIn("--memory-swap=" + next(x.split("=",1)[1] for x in value if x.startswith("--memory=")), value)
            args.append(["RESOURCE" if x.startswith(("--memory=", "--memory-swap=")) or
                         (i and value[i-1] in ("-smp", "-m")) else x for i,x in enumerate(value)])
        self.assertTrue(all(value == args[0] for value in args))

    def test_large_guest_requires_reserved_headroom_and_valid_resource_values(self):
        for memory in (True, 0, -1, 2**64, 3.5):
            with self.assertRaises(ValueError): emulator.memory_options(8*1024*1024, memory_bytes=memory)
        with self.assertRaises(ValueError):
            emulator.memory_options(4*1024*1024, memory_bytes=4038197248)
        for cpus in (True, 0, -1, 17):
            with self.assertRaises(ValueError):
                emulator.boot_arguments(Path('/b'), Path('/w'), 'az-sdk-automation-unit', 'a'*32,
                                        2230,4450,8*1024*1024,vcpus=cpus)

    def test_invalid_vm_identity_or_ports_cannot_be_launched(self):
        for name, token, port, qmp in (("existing-user-vm", "a"*32, 2230,4450),
                                      ("az-sdk-automation-unit", "bad",2230,4450),
                                      ("az-sdk-automation-unit", "a"*32,2230,2230),
                                      ("az-sdk-automation-unit", "a"*32,True,4450)):
            with self.assertRaises(ValueError):
                emulator.boot_arguments(Path("/base"), Path("/work"), name, token, port, qmp, 8 * 1024 * 1024)

    def test_black_uniform_truncated_and_wrong_size_displays_cannot_pass(self):
        header = b"P6\n1280 800\n255\n"
        for raster in (b"", b"\0" * (1280*800*3), b"\x80" * (1280*800*3)):
            self.assertFalse(emulator.ppm_ready(header + raster))
        self.assertFalse(emulator.ppm_ready(b"P6\n1 1\n255\nabc"))
        self.assertTrue(emulator.ppm_ready(header + bytes(range(256)) * 12000))

    def test_kernel_pass_never_hides_baseline_failure_or_missing_coverage(self):
        for kernel, baseline, expected in (("PASS","PASS","PASS"), ("PASS","FAIL","FAIL"),
                                           ("FAIL","PASS","FAIL"), ("PASS","INCOMPLETE","INCOMPLETE"),
                                           ("PASS",None,"INCOMPLETE")):
            self.assertEqual(emulator.acceptance_status({"status":kernel}, {"status":baseline}), expected)

    def test_blank_display_cannot_erase_observed_firmware_failure(self):
        for health, expected in (("PASS", "INCOMPLETE"), ("FAIL", "FAIL"), ("INCOMPLETE", "INCOMPLETE")):
            result = emulator.baseline_result({"status": health, "errors": []}, False)
            self.assertEqual(result["status"], expected)
            self.assertFalse(result["nonblank_display"])
        self.assertEqual(emulator.baseline_result({"status": "PASS"}, True)["status"], "PASS")

    def test_startup_grace_is_bounded_and_does_not_hide_service_failure(self):
        from unittest.mock import Mock
        waiting = "ActiveState=activating\nSubState=start\nNRestarts=0\n"
        ready = "123\nActiveState=active\nSubState=running\nNRestarts=0\n"
        failed = "ActiveState=failed\nSubState=failed\nNRestarts=0\n"
        for rows, expected, sleeps in (([waiting, ready], ready, 1), ([failed], failed, 0)):
            remote = Mock(side_effect=rows)
            with patch.object(emulator.time, "sleep") as pause:
                self.assertEqual(emulator.await_baseline(remote), expected)
                self.assertEqual(pause.call_count, sleeps)
        remote = Mock(return_value=waiting)
        with patch.object(emulator.time, "sleep"), patch.object(emulator.time, "monotonic", side_effect=range(100)):
            self.assertEqual(emulator.await_baseline(remote), waiting)
        self.assertLessEqual(remote.call_count, 31)

    def test_failed_kernel_and_vm_ownership_survive_later_transport_failure(self):
        for owner, kernel in (("a" * 32, "FAIL"), ("unrelated-owner", "FAIL"), ("a" * 32, "PASS"), ("a" * 32, None)):
            with tempfile.TemporaryDirectory() as temporary:
                base = Path(temporary) / "base"; (base / "ssh").mkdir(parents=True)
                for name in ("Image", "initramfs-patched.cpio.gz", "az-mnt.img", "usb.img", "ssh/az-emu_ed25519"):
                    (base / name).write_bytes(b"fixture")
                commands = []
                scans = [0]
                def command(argv, **kwargs):
                    commands.append(argv)
                    output, status = b"", 0
                    if argv[0] == "ssh-keyscan":
                        scans[0] += 1
                        if scans[0] == 1:
                            return SimpleNamespace(returncode=0,stdout=b'# 127.0.0.1:2360 SSH-2.0-dropbear\n',stderr=b'')
                        family=b'ssh-ed25519'
                        blob=struct.pack('>I',len(family))+family+struct.pack('>I',32)+b'x'*32
                        output=b'[127.0.0.1]:2360 ssh-ed25519 '+base64.b64encode(blob)+b'\n'
                    elif argv[-1] == 'true':
                        known=Path(next(x.split('=',1)[1] for x in argv if x.startswith('UserKnownHostsFile=')))
                        self.assertTrue(known.read_bytes().startswith(b'[127.0.0.1]:2360 ssh-ed25519 '))
                    elif argv[:2] == ["docker", "inspect"]: output = owner.encode()
                    elif argv[-1] == "/proc/meminfo": output = b"MemAvailable: 8388608 kB\n"
                    elif argv[-1] == "sha256sum /home/root/pdj/EP147": output = emulator.FIRMWARE["130"].encode() + b"  EP147\n"
                    elif argv[-1].startswith("pidof EP147"): output = b"123\nNRestarts=0\nActiveState=active\nSubState=running\n"
                    elif argv[-1].startswith("awk '{print $22}' /proc/123/stat"):output=b'42\n'
                    elif argv[-1].startswith('if test -r /proc/123/stat'):output=b'123\nNRestarts=0\nActiveState=active\nSubState=running\n'
                    elif argv[-1].startswith("journalctl"): status = 1
                    return SimpleNamespace(returncode=status, stdout=output, stderr=b"transport lost" if status else b"")
                with patch.object(emulator, "validate_package") as packet_validation, patch.object(emulator, "digest", return_value="fixture"), \
                     patch.object(emulator.uuid, "uuid4", return_value=SimpleNamespace(hex="a" * 32)), \
                     patch.object(emulator.subprocess, "run", side_effect=command), \
                     patch.object(emulator.time, "sleep"), \
                     patch.object(emulator.time, "monotonic", side_effect=range(10000)), \
                     patch.object(emulator, "execute", return_value={"status": kernel}) as native:
                    options = {"native_tests": False, "baseline_seconds": 2} if kernel is None else {}
                    report = emulator.run(base, Path(temporary) / "packet", Path(temporary) / "out", "130", 2360, 4460, **options)
                self.assertEqual(native.called, kernel is not None)
                if native.called:self.assertEqual(native.call_args[1].get('preflight_timeout'),120)
                self.assertEqual(packet_validation.called, kernel is not None)
                if kernel is None:
                    self.assertEqual(report['kernel_tests']['status'], 'INCOMPLETE')
                self.assertEqual(report["status"], "FAIL" if kernel == "FAIL" else "INCOMPLETE")
                removals = [args for args in commands if args[:3] == ["docker", "rm", "-f"]]
                self.assertEqual(len(removals), int(owner == "a" * 32))
                if removals:
                    self.assertEqual(removals[0][-1], "az-sdk-automation-130-" + "a" * 12)


if __name__ == "__main__": unittest.main()
