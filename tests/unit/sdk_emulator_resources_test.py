#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import unittest
import tempfile
import json
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('resources', ROOT/'tests/integration/sdk_emulator_resources.py')
resources = importlib.util.module_from_spec(spec); spec.loader.exec_module(resources)


class ComparisonTests(unittest.TestCase):
    def test_new_experiment_freezes_runner_and_refuses_to_replace_existing_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            p=Path(temporary); base=p/'base'; (base/'ssh').mkdir(parents=True)
            for name in ('Image','initramfs-patched.cpio.gz','az-mnt.img','usb.img','ssh/az-emu_ed25519'):
                (base/name).write_bytes(b'fixture')
            source=p/'source';source.mkdir()
            for name in ('sdk_emulator_smoke.py','sdk_native_run.py','sdk_runtime_check.py'):
                (source/name).write_bytes(b'frozen runner')
            output=p/'out'
            with patch.object(resources.subprocess,'check_output',return_value=b'sha256:fixture\n'):
                resources.prepare(output,base,'200',source)
                original=(output/'inputs.json').read_bytes()
                with self.assertRaises(ValueError): resources.prepare(output,base,'200',source)
            self.assertEqual((output/'inputs.json').read_bytes(),original)
            manifest=json.loads(original)
            self.assertEqual(len(manifest['runner_sha256']),3)
            self.assertEqual((output/'runner/sdk_emulator_smoke.py').read_bytes(),b'frozen runner')

    def test_completed_comparison_can_be_analyzed_without_docker_or_running_code(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); frozen=root/'runner'; frozen.mkdir()
            (root/'inputs.json').write_text(json.dumps({'runner_sha256':{},'qemu_image_id':'sha256:'+'a'*64}))
            for name,cpus,memory in resources.CASES:
                folder=root/name; (folder/'vm').mkdir(parents=True)
                report,identity,journal,host=self.fixture(cpus,memory)
                (folder/'report.json').write_text(json.dumps(report))
                for file,text in (('guest-identity.txt',identity),('baseline-journal.txt',journal),('host-after.txt',host)):
                    (folder/file).write_text(text)
                (folder/'vm/serial.log').write_text('Memory: 1K/%dK available\nsmp: Brought up 1 node, %d CPUs\n'%(memory//1024,cpus))
            args=SimpleNamespace(output=root,base=None,profile='200',analyze_only=True,port=2370,qmp_port=4470)
            with patch.object(resources.argparse.ArgumentParser,'parse_args',return_value=args), \
                 patch.object(resources.subprocess,'check_output',side_effect=AssertionError('offline analysis contacted Docker')), \
                 patch('builtins.print'):
                self.assertEqual(resources.main(),0)
    def fixture(self, cpus=4, memory=2147483648):
        report = {'resources': {'vcpus': cpus, 'guest_memory_bytes': memory},
                  'kernel_tests': {'status':'INCOMPLETE'}, 'baseline': {'status':'FAIL'},
                  'owned_vm_removed': True, 'base_disks_unchanged': True,
                  'qemu_image_id': 'sha256:'+'a'*64, 'kernel_sha256':'b'*64,
                  'initramfs_sha256':'c'*64, 'firmware_sha256':'d'*64,
                  'base_disks': {'az-mnt.img': {'size':42,'mtime_ns':100}},
                  'container_state': {'OOMKilled':False}}
        identity = '\n'.join('e'*64+'  '+n for n in ('/home/root/pdj/EP147','/home/root/ep122_shim.so',
                             '/usr/lib/modules/virtio_snd.ko','/etc/asound.conf'))
        identity += '\nvirtio_snd 123 0 - Live 0x123\n0 [ALPHATHETAD810]: virtual audio\n'
        return report, identity, 'ERR DataPoolablePreviewMixer::processBlock() - retryCount < 20000', 'oom 0\noom_kill 0\nnr_throttled 0\n'

    def measure(self, cpus=4, memory=2147483648):
        return resources.measurement(*self.fixture(cpus,memory))

    def test_matching_full_factorial_can_reject_resource_only_explanation(self):
        cases=[self.measure(c,m) for c in (4,6) for m in (2147483648,4038197248)]
        result=resources.compare(cases)
        self.assertTrue(result['valid_comparison'])
        self.assertEqual(result['conclusion'],'same_preview_mixer_assertion_at_all_resource_settings')

    def test_missing_case_or_changed_driver_cannot_produce_a_conclusion(self):
        cases=[self.measure(c,m) for c in (4,6) for m in (2147483648,4038197248)]
        self.assertFalse(resources.compare(cases[:-1])['valid_comparison'])
        cases[-1]['identity']['guest_sha256']['/usr/lib/modules/virtio_snd.ko']='f'*64
        self.assertFalse(resources.compare(cases)['valid_comparison'])

    def test_oom_missing_metrics_sdk_workload_and_transport_errors_invalidate_run(self):
        for change in ('oom','missing','sdk','transport','cleanup','driver','alsa'):
            report, identity, journal, host=self.fixture()
            if change=='oom': host='oom 1\noom_kill 1\n'
            if change=='missing': host=''
            if change=='sdk': report['kernel_tests']['status']='PASS'
            if change=='transport': report['error']='SSH timeout'
            if change=='cleanup': report['owned_vm_removed']=False
            if change=='driver': identity=identity.replace('virtio_snd 123','missing 123')
            if change=='alsa': identity=identity.replace('ALPHATHETAD810','missing')
            self.assertFalse(resources.measurement(report,identity,journal,host)['valid'],change)

    def test_baseline_health_alone_does_not_override_assertion_or_blank_display(self):
        report,identity,journal,host=self.fixture(); report['baseline']={'status':'PASS','nonblank_display':True}
        self.assertEqual(resources.measurement(report,identity,journal,host)['outcome'],'preview_mixer_assertion')
        self.assertEqual(resources.measurement(report,identity,'clean',host)['outcome'],'started')
        report['baseline']['nonblank_display']=False
        self.assertNotEqual(resources.measurement(report,identity,'clean',host)['outcome'],'started')

    def test_changed_outcome_is_reported_without_claiming_driver_root_cause(self):
        cases=[self.measure(c,m) for c in (4,6) for m in (2147483648,4038197248)]
        cases[-1]['outcome']='started'
        result=resources.compare(cases)
        self.assertTrue(result['valid_comparison'])
        self.assertEqual(result['conclusion'],'startup_outcome_changed_with_resources')

    def test_guest_must_actually_boot_the_requested_cpus_and_memory(self):
        text='Memory: 3615388K/3943552K available\nsmp: Brought up 1 node, 6 CPUs\n'
        self.assertTrue(resources.boot_resources(text,6,4038197248)['valid'])
        self.assertFalse(resources.boot_resources(text,4,4038197248)['valid'])
        self.assertFalse(resources.boot_resources(text,6,2147483648)['valid'])
        self.assertFalse(resources.boot_resources('',6,4038197248)['valid'])


if __name__=='__main__': unittest.main()
