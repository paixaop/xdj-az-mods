#!/usr/bin/env python3
"""Exercise the real position reader/JSON publisher with copied mock SDK records."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import unittest
import shutil

ROOT=Path(__file__).resolve().parents[2]

@unittest.skipUnless(sys.platform.startswith('linux'),'Linux preload probe')
class PositionProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='az-position-test-')
        cls.binary=Path(cls.temporary.name)/'fixture'
        flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread']
        mode=os.environ.get('SAN') or 'plain'
        if mode=='asan': flags+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
        elif mode=='tsan': flags+=['-fsanitize=thread']
        elif mode!='plain': raise ValueError('invalid SAN')
        p=subprocess.run([os.environ.get('HOST_CC','cc'),*flags,str(ROOT/'tests/unit/sdk_position_probe_fixture.c'),'-ldl','-lm','-o',str(cls.binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True)
        if p.returncode: raise RuntimeError(p.stdout+p.stderr)
        cls.flags=flags
        cls.compiler=os.environ.get('HOST_CC','cc')
        cls.runtime=Path(cls.temporary.name)/'EP147'
        cls.probe=Path(cls.temporary.name)/'probe.so'
        cls.short=Path(cls.temporary.name)/'short-probe.so'
        cls.mock=Path(cls.temporary.name)/'mock.so'
        cls.wrong=Path(cls.temporary.name)/'wrong.so'
        cls.object=Path(cls.temporary.name)/'object.so'
        cls.build_id='11223344556677889900aabbccddeeff11223344'
        def build(output,sources,extra):
            p=subprocess.run([cls.compiler,*flags,*extra,*[str(ROOT/s) for s in sources],'-ldl','-lm','-o',str(output)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True)
            if p.returncode: raise RuntimeError(p.stdout+p.stderr)
        build(cls.runtime,['tests/unit/sdk_position_runtime_fixture.c'],[])
        shared=['-shared','-fPIC']
        build(cls.probe,['tests/integration/sdk_position_probe.c'],shared+['-DAZSDK_POSITION_TEST','-DAZSDK_POSITION_HEADER_SHA256="'+'a'*64+'"','-DAZSDK_POSITION_PROVIDER_SHA256="'+'b'*64+'"','-DAZSDK_POSITION_BUILD_ID="'+cls.build_id+'"'])
        build(cls.short,['tests/integration/sdk_position_probe.c'],shared+['-DAZSDK_POSITION_TEST','-DAZSDK_POSITION_TEST_WINDOW_NS=500000000ULL','-DAZSDK_POSITION_HEADER_SHA256="'+'a'*64+'"','-DAZSDK_POSITION_PROVIDER_SHA256="'+'b'*64+'"','-DAZSDK_POSITION_BUILD_ID="'+cls.build_id+'"'])
        build(cls.mock,['tests/unit/sdk_position_mock_provider.c'],shared+['-Wl,--build-id=0x'+cls.build_id])
        build(cls.wrong,['tests/unit/sdk_position_mock_provider.c'],shared+['-Wl,--build-id=0x'+'22'*20])
        build(cls.object,['tests/unit/sdk_position_mock_provider.c'],shared+['-Wl,--build-id=0x'+cls.build_id,'-DPOSITION_MOCK_OBJECT'])
        cls.sanitizer=''
        if mode in ('asan','tsan'):
            library='libasan.so' if mode=='asan' else 'libtsan.so'
            cls.sanitizer=subprocess.check_output([cls.compiler,'-print-file-name='+library],universal_newlines=True).strip()+':'
    @classmethod
    def tearDownClass(cls): cls.temporary.cleanup()
    def run_case(self,mode,output=None):
        argv=[str(self.binary),mode]+([str(output)] if output else [])
        p=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True,encoding='utf-8',timeout=5)
        self.assertEqual(p.returncode,0,p.stderr)
        return json.loads(p.stdout)
    def test_all_decks_and_engine_domain(self):
        d=self.run_case('normal'); self.assertEqual(d['status'],'ok'); self.assertEqual(d['engine_rate'],44100)
        self.assertEqual(d['abi_size'],296); self.assertEqual(d['native_boottime_ns'],20000000000)
        decks=d['samples'][-1]['decks']; self.assertEqual([x['deck'] for x in decks],list(range(4)))
        for i,x in enumerate(decks): self.assertEqual((x['loaded'],x['title'],x['playhead'],x['pos_s']),(1,'Deck '+str(i+1),(i+1)*44100,i+1))
    def test_bounded_rolling_history(self):
        d=self.run_case('history'); self.assertEqual(len(d['samples']),12)
        self.assertEqual([x['sequence'] for x in d['samples']],list(range(5,17)))
        self.assertTrue(all(a['native_monotonic_ns']<b['native_monotonic_ns'] for a,b in zip(d['samples'],d['samples'][1:])))
    def test_titles_escape_and_preserve_utf8(self):
        d=self.run_case('escape'); self.assertEqual(d['samples'][0]['decks'][0]['title'],'quote" slash\\ newline\n日')
    def test_unknown_unloaded_and_nonfinite_positions(self):
        for mode in ['nan','negative','unknown','unloaded']:
            with self.subTest(mode=mode):
                d=self.run_case(mode)
                for x in d['samples'][0]['decks']: self.assertIsNone(x['pos_s'])
    def test_errors_clear_history_and_old_data(self):
        for mode in ['bad_title','bad_utf8','bad_size','bad_loaded','failure','failure_after','backward','boot_error']:
            with self.subTest(mode=mode):
                d=self.run_case(mode); self.assertEqual(d['status'],'unavailable'); self.assertEqual(d['samples'],[])
    def test_identity_change_resets_history(self):
        d=self.run_case('identity'); self.assertEqual(len(d['samples']),1); self.assertEqual(d['samples'][0]['sequence'],2)
        self.assertTrue(all(x['title']=='new title' for x in d['samples'][0]['decks']))
    def test_atomic_private_publication(self):
        with tempfile.TemporaryDirectory() as output:
            expected=self.run_case('normal',Path(output)); files=list(Path(output).iterdir()); self.assertEqual(len(files),1)
            self.assertEqual(files[0].stat().st_mode&0o777,0o600); self.assertEqual(json.loads(files[0].read_text()),expected)
    def test_no_symlink_clobber(self):
        with tempfile.TemporaryDirectory() as output:
            self.run_case('symlink',Path(output)); self.assertEqual((Path(output)/'sentinel').read_text(),'sentinel')
    def test_no_unowned_regular_or_hardlink_clobber(self):
        for mode in ['preexisting','hardlink']:
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as output:
                self.run_case(mode,Path(output))
                files=list(Path(output).glob('*.json')); self.assertEqual(len(files),1)
                self.assertEqual(files[0].read_text(),'sentinel')
    def test_oversized_publication_clears_owned_stale_packet(self):
        with tempfile.TemporaryDirectory() as output:
            self.run_case('oversized',Path(output)); self.assertEqual(list(Path(output).iterdir()),[])
    def test_publication_failure_removes_owned_stale_packet(self):
        with tempfile.TemporaryDirectory() as output:
            self.run_case('write_failure',Path(output)); self.assertEqual(list(Path(output).iterdir()),[])
    def test_window_is_bounded_and_clock_failures_close_it(self): self.assertTrue(self.run_case('window')['window'])
    def test_supported_loaded_session_retains_position_witness(self):
        self.assertTrue(self.run_case('loaded_session')['loaded_session'])
    def test_renewal_accepts_only_new_private_bounded_nonce(self):
        with tempfile.TemporaryDirectory() as output:
            self.assertTrue(self.run_case('renew',output)['renew'])

    def runtime_case(self,mode='normal',provider=None,opt_in='1',name='EP147',dynamic=False):
        with tempfile.TemporaryDirectory() as output:
            executable=self.runtime
            if name!='EP147':
                executable=Path(self.temporary.name)/name
                shutil.copyfile(str(self.runtime),str(executable)); executable.chmod(0o700)
            env=dict(os.environ,AZSDK_POSITION_TEST_DIR=output)
            if opt_in is None: env.pop('AZSDK_POSITION_PROBE',None)
            else: env['AZSDK_POSITION_PROBE']=opt_in
            if dynamic:
                argv=[str(executable),'dynamic',str(provider or self.mock),str(self.probe)]
                env['LD_PRELOAD']=self.sanitizer.rstrip(':')
            else:
                selected=self.short if mode=='renew' else self.probe
                env['LD_PRELOAD']=self.sanitizer+(':'.join(str(p) for p in [provider,selected] if p))
                argv=[str(executable),mode]
            p=subprocess.run(argv,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True,encoding='utf-8',timeout=6)
            self.assertEqual(p.returncode,0,p.stderr)
            result=json.loads(p.stdout)
            files=list(Path(output).glob('*.json'))
            final=json.loads(files[0].read_text()) if files else None
            return result,final
    def test_opt_in_and_exact_process_gate(self):
        for flag,name in [(None,'EP147'),('0','EP147'),('01','EP147'),('1','other')]:
            with self.subTest(flag=flag,name=name):
                d,final=self.runtime_case(provider=self.mock,opt_in=flag,name=name)
                self.assertTrue(d['missing']); self.assertIsNone(final)
    def test_real_constructor_and_provider_binding(self):
        d,final=self.runtime_case(provider=self.mock)
        self.assertEqual(d['status'],'ok'); self.assertEqual(d['provider_build_id'],self.build_id)
        self.assertGreaterEqual(len(d['samples']),5); self.assertGreater(d['native_boottime_ns'],0)
        self.assertEqual(final['status'],'stopped'); self.assertEqual(final['samples'],[])
    def test_missing_wrong_or_nonfunction_provider_is_unavailable(self):
        for provider in [None,self.wrong,self.object]:
            with self.subTest(provider=provider):
                d,final=self.runtime_case(provider=provider)
                self.assertEqual(d['status'],'unavailable'); self.assertEqual(d['samples'],[])
    def test_provider_remains_pinned_after_original_handle_closes(self):
        d,final=self.runtime_case(provider=self.mock,dynamic=True)
        self.assertEqual(d['status'],'ok'); self.assertGreaterEqual(len(d['samples']),5)
        self.assertEqual(final['status'],'stopped')
    def test_stop_marker_clears_history(self):
        d,final=self.runtime_case('stop',self.mock)
        self.assertEqual(d['status'],'stopped'); self.assertEqual(d['samples'],[])
    def test_expired_capture_renews_in_the_same_process_without_old_history(self):
        d,final=self.runtime_case('renew',self.mock)
        before,after=d['before'],d['after']
        self.assertEqual(before['status'],'stopped');self.assertEqual(before['samples'],[])
        self.assertEqual(after['pid'],before['pid']);self.assertEqual(after['status'],'ok')
        self.assertEqual(after['capture_nonce'],'1'*32);self.assertTrue(after['samples'])
        self.assertGreater(after['samples'][0]['sequence'],before['sequence'])
        self.assertEqual(final['status'],'stopped')
    def test_forked_child_does_not_touch_parent_worker(self):
        d,final=self.runtime_case('fork',self.mock)
        self.assertEqual(d['status'],'ok'); self.assertGreaterEqual(len(d['samples']),5)

if __name__=='__main__': unittest.main()
