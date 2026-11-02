#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fake I/O tests for loop command emulator trust, staging and load wiring.

No SSH process, Docker container, firmware, approval file or panel is contacted.
"""
from contextlib import ExitStack
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
import sdk_emulator_loop_runtime as loop
from sdk_emulator_loop import records
from sdk_panel_transport import PanelTransport as RealPanel
from sdk_emulator_controls import Controller as RealController

TOKEN='a'*32
SSH=['ssh','-T','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile=/tmp/pinned',
     '-o','BatchMode=yes','-o','ConnectTimeout=5','-p','2390','root@127.0.0.1']
BINDING=dict(provider_build_id='b'*40,header_sha256='c'*64,engine_rate=44100,abi_size=296)

def position_packet(values,start_ns=1000000000,step_ns=100000000):
 samples=[]
 for index,frame in enumerate(values,1):
  stamp=start_ns+index*step_ns
  samples.append(dict(sequence=index,native_monotonic_ns=stamp,decks=[dict(deck=d,read_rc=0,loaded=1,title='test track',playhead=frame,pos_s=frame/44100) for d in range(4)]))
 stamp=samples[-1]['native_monotonic_ns']
 return dict(BINDING,schema=1,pid=200,status='ok',sequence=len(samples),native_monotonic_ns=stamp,native_boottime_ns=stamp+5000000000,samples=samples)

def quantize_snapshot(values,revision=1,received_ns=1000000000,native_now=1000000000):
 return dict(native_now=native_now,decks=[dict(streams={'loop':dict(unit=str(11+d)),'quantize':dict(valid='1' if value is not None else '0',unit=str(11+d),revision=str(revision),received_ns=str(received_ns),quantize=value)}) for d,value in enumerate(values)])

def motion_snapshot(moving=True):
 packet=position_packet([44100+(4410*i if moving else 0) for i in range(1,7)],start_ns=2000000000)
 return dict(positions=packet,boottime_ns=packet['native_boottime_ns'])

class LoopRuntime(unittest.TestCase):
 def case(self,mode='ok',expected=None):
  with tempfile.TemporaryDirectory() as temporary:
   root=Path(temporary);(root/'src/azmod').mkdir(parents=True)
   (root/'src/azmod/azmod-mod.h').write_bytes(b'header')
   (root/'runtime.json').write_text(json.dumps(dict(position_binding=BINDING)))
   driver=root/'driver';driver.write_bytes(b'fixture driver')
   receiver=root/'receiver';receiver.write_bytes(b'fixture receiver')
   probe=root/'probe.azmod'
   files={'azmod.json':json.dumps(dict(id=loop.ID,version='0.1.0',api=1,hooks=False,firmware=['any'])).encode(),
          loop.ID+'.so':b'fixture normal module','LICENSE':b'GPL','icon.png':b'icon'}
   with zipfile.ZipFile(probe,'w') as archive:
    for name,data in files.items():archive.writestr(name,data)
   probe_sha=hashlib.sha256(files[loop.ID+'.so']).hexdigest()
   manifest=dict(package={'sha256':loop.sha(probe)},binary={'sha256':probe_sha},sha256={'include/azmod-mod.h':loop.sha(root/'src/azmod/azmod-mod.h')})
   pb=root/'probe.json';pb.write_text(json.dumps(manifest))
   state=dict(staged=0,flags=0,public=0,allow=0,observed=0,commands=0,refusals=0,continuation=0,captures=0,loads=[],plays=[],selections=[],requests=[],scripts=[],renew=[],verify=[],motions=[],quantize=[],order=[],failures=[],controller=None)
   class Controller:
    timeout=17
    def __init__(self):
     self.driver=SimpleNamespace(BUS_ERP=0,BUS_MIXER=2);self.events=[];self.pid='200';self.position_binding=BINDING;self.setup_attempts=set()
     self.q_values=[1]*4 if mode in ('quantize-on','quantize-no-transition') else [0]*4
     if mode=='quantize-string-on':self.q_values=['1']*4
     if mode=='quantize-warm-off':self.q_values=[None]*4
     if mode=='quantize-mixed':self.q_values=[0,1,0,1]
     self.q_revision=1
    _stream=staticmethod(RealController._stream)
    _newer=staticmethod(RealController._newer)
    _native_now=staticmethod(lambda snapshot,deck:snapshot.get('native_now'))
    def observe(self):
     state['order'].append('observe_quantize')
     stamp=1000000000+self.q_revision*100000000
     return quantize_snapshot(self.q_values,self.q_revision,stamp,stamp)
    def quantize(self):
     self_last=state['renew'][-1] if state['renew'] else None
     assert self_last=='load-deck1', 'Quantize must use the fresh load receiver, not the old navigation receiver'
     state['order'].append('quantize')
     state['quantize'].append(self.timeout)
     if mode!='quantize-no-transition':self.q_values=(['0']*4 if mode=='quantize-string-on' else [0]*4);self.q_revision+=1
    def _release(self,bus):self.events.append(dict(action='release',bus=bus))
    def panel_inputs_neutral(self):return True
    def screenshot(self,label):return 'screenshot-'+label
    def load(self,deck):
     state['loads'].append((deck,self.timeout))
     if mode=='load-error':raise loop.MissingEvidence('fixture load timeout')
    def wait(self,predicate,label,**options):
     if 'Quantize' in label:
      value=self.observe()
      if not predicate(value) and not options.get('allow_partial'):raise loop.MissingEvidence('Quantize transition not observed')
      return value
     if 'playback source advances' in label:
      self_timeout=self.timeout;assert self_timeout==17
      snapshot=motion_snapshot(moving=mode!='stationary')
      if not predicate(snapshot):raise loop.MissingEvidence('fixture playback source did not advance')
      state['motions'].append((label,self_timeout));return snapshot
     packet=position_packet([44100]*6)
     snapshot={'decks':[dict(loaded=True,title='test track') for _ in range(4)],'positions':packet,'boottime_ns':packet['native_boottime_ns']}
     assert predicate(snapshot)
     if mode=='wrong-title':snapshot['decks'][0]['title']='other track'
     if mode=='missing-position':snapshot.pop('positions')
     return snapshot
    def button(self,deck,name):state['plays'].append((deck,name,self.timeout))
    def sleep(self,seconds):pass
   class Panel:
    _strict_ssh=staticmethod(RealPanel._strict_ssh)
    def __init__(self,*args):
     self.pid=args[3];self.birth=args[4];self.owner_token=args[5]
   class Sessions:
    def __init__(self,pid,birth,token,guard,independent,factory,max_sessions):
     assert (pid,birth,token)==('200','2000',TOKEN);self.factory=factory;guard()
    def renew(self,label,neutral):
     self.assert_neutral=neutral
     if not neutral:raise ValueError('neutral inputs required')
     state['renew'].append(label);state['order'].append('renew:'+label)
     return self.factory(label)
    def close(self):state['order'].append('close')
   def command(argv,**options):
    if argv[:2]==['docker','inspect']:return ((TOKEN if mode!='owner' else 'b'*32)+'\n').encode()
    self.assertEqual(argv[:-1],SSH);text=argv[-1];state['scripts'].append(text)
    self.assertNotIn('approved.sha256',text)
    if 'tar -xf -' in text:
     state['staged']+=1
     self.assertIn('/proc/mounts',text);self.assertIn('rw(,|$)',text)
     self.assertIn('test ! -e /media/usb/sda1/MODS/community-mods/'+loop.ID,text)
     self.assertNotIn('mkdir -p /media/usb/sda1\n',text)
     if mode=='usb-refused':raise RuntimeError('guest USB is not a mounted writable filesystem')
     with tarfile.open(fileobj=io.BytesIO(options['input'])) as archive:
      self.assertEqual(set(archive.getnames()),set(files));self.assertEqual(len(archive.getnames()),4)
      for name,data in files.items():self.assertEqual(archive.extractfile(name).read(),data)
     pairs=[(name,hashlib.sha256(data).hexdigest()) for name,data in files.items()]
     if mode=='stage-hash':pairs[0]=(pairs[0][0],'0'*64)
     if mode=='stage-extra':pairs.append(('extra','1'*64))
     return ''.join(sha+'  /stage/'+name+'\n' for name,sha in pairs).encode()
    if 'COMMAND_FLAG' in text:
     state['flags']+=1
     flags=['azmod.native-exit.on','azmod.native-reloop.on','azmod.native-loop-commands.on']
     if mode=='flag':flags.pop()
     return ''.join('COMMAND_FLAG '+name+'\n' for name in flags).encode()
    if text.startswith('AZSDK_NATIVE_BIRTH_CHECK=1\n'):return b'AZSDK_NATIVE_BIRTH_MATCH\n'
    if text.startswith('umask 077; mkdir /tmp/az-sdk-panel.'):
     self.assertEqual(options['input'],b'fixture receiver');return b''
    if 'usb-renotify.sh' in text:return b''
    if '.request-' in text:
     self.assertTrue(text.startswith('AZSDK_PUBLIC_READ=1\n'));self.assertEqual(text.count('test "$(check_public_native)" = AZSDK_NATIVE_BIRTH_MATCH || exit 93'),2)
     self.assertIn('/request.processing',text);self.assertIn('test ! -L ',text)
     self.assertIn('umask 077\ncat >',text);self.assertRegex(text,r'mv [^\n]+/.request-[a-f0-9]{32} [^\n]+/request\n')
     state['requests'].append(options['input']);return b''
    self.assertTrue(text.startswith('AZSDK_PUBLIC_READ=1\n'),text)
    if '/maps' in text:
     return ('1-2 r-xp 00000000 01:01 7 /home/root/settings/az-usbload/community/'+loop.ID+'/'+loop.ID+'.so\n').encode() if mode!='unmapped' else b''
    if 'sha256sum /home/root/settings/az-usbload/community/' in text:
     path='/home/root/settings/az-usbload/community/'+loop.ID+'/'+loop.ID+'.so'
     return ((probe_sha if mode!='mapped-hash' else '0'*64)+'  '+path+'\n').encode()
    if '__AZSDK_LOOP_RESULTS__' in text:
     state['captures']+=1
     self.assertIn('__AZSDK_LOOP_HEALTH__',text);self.assertIn('/results.200.log',text)
     self.assertEqual(text.count('test "$(check_public_native)" = AZSDK_NATIVE_BIRTH_MATCH || exit 93'),2)
     service='NRestarts=1\nActiveState=failed\nSubState=failed\n' if mode=='health-fail' else 'NRestarts=0\nActiveState=active\nSubState=running\n'
     result_log=''
     def receipt(seq):
      return f'SDK_LOOP v=1 seq={seq} event=snapshot id=0 deck=0 op=0 target_load=0 rc=0 outcome=-1 reason=0 revision=0 receipt_ns=0 query_ns={seq*100} load_valid=0 native_load=0 load_unit=0 load_ns=0 engaged_valid=0 engaged=-1 bounds_valid=0 in=0 out=0 loop_unit=0 loop_rev=0 loop_ns=0\n'
     if mode=='partial-tail':result_log=receipt(1)+(receipt(2)[:80] if state['captures']==1 else receipt(2))
     if mode=='malformed-complete':result_log='SDK_LOOP v=1 seq=1 broken=yes\n'
     return ('fixture SDK loop transcript\n\n__AZSDK_LOOP_RESULTS__\n'+result_log+'\n__AZSDK_LOOP_HEALTH__\n'+service).encode()
    if 'systemctl show az-sdk-runtime.service' in text:self.fail('health must share the guarded transcript capture')
    if 'cat '+loop.LOG in text:return b'fixture SDK loop transcript\n'
    if '/results.200.log' in text:return b''
    self.fail('unhandled fixture command '+text)
   def prepare_allow(*args):state['allow']+=1
   def prepare_deck(controller,logical):
    self.assertEqual(len(state['loads']),logical-1);state['selections'].append(logical);state['order'].append('selection:'+str(logical))
    return dict(status='PASS',selected_title='test track',evidence='selection.png') if mode not in ('selection','setup-fail-callback') else dict(status='INCOMPLETE')
   context=dict(token=TOKEN,container='az-sdk-automation-fixture',prepare_allow=prepare_allow,prepare_deck=prepare_deck,
                driver_path=driver,driver_sha256=loop.sha(driver),receiver_path=receiver,receiver_sha256=loop.sha(receiver),
                expected_titles=['test track'],position_binding=BINDING,probe_manifest=pb,probe_package=probe,debug=True)
   def on_failure(controller,result,read_guest):
    self.assertIs(controller,state['controller']);self.assertNotEqual(result['status'],'PASS')
    self.assertEqual(read_guest('cat '+loop.LOG).strip(),'fixture SDK loop transcript')
    state['failures'].append(result['status'])
   if mode.endswith('-callback'):context['on_failure']=on_failure
   if mode=='bad-callback':context['on_failure']=1
   if mode=='quiet':context.pop('debug')
   if mode=='bad-debug':context['debug']=1
   if mode=='header':(root/'src/azmod/azmod-mod.h').write_bytes(b'changed header')
   if mode=='probe-hash':probe.write_bytes(probe.read_bytes()+b'changed')
   if mode=='driver':context['driver_sha256']='0'*64
   if mode=='receiver':context['receiver_sha256']='0'*64
   if mode=='unsafe-usb':context['usb_mount']='/media/usb/sda1;touch /bad'
   if mode=='position':context['position_binding']=dict(BINDING,abi_size=999)
   if mode=='bad-container':context['container']='unowned-container'
   def public(ssh,package,source,output,firmware,seconds,cmd,config):
    state['public']+=1
    output.mkdir()
    self.assertIs(config['prepare_allow'],prepare_allow);self.assertEqual(config['probe_id'],loop.ID)
    self.assertFalse(config['run_settings']);self.assertTrue(callable(config['loaded_executor']))
    if mode=='unapproved':return dict(status='INCOMPLETE',missing=['normal UI approval absent'])
    config['prepare_allow'](None,None,None)
    def verify_new(pid,birth):
     self.assertNotEqual((pid,birth),('100','1000'));state['verify'].append((pid,birth));return True
    return config['loaded_executor'](dict(pid='100',birth='1000'),600,verify_new)
   def observed(ssh,package,source,output,firmware,seconds,cmd,config,execute_controls,validate_observed):
    state['observed']+=1;self.assertEqual(config['stopped_identity'],dict(pid='100',birth='1000'))
    output.mkdir(parents=True)
    config['panel_transport_factory']('200','2000',lambda:None)
    controller=Controller();state['controller']=controller
    return execute_controls(controller)
   def run_refusals(read_log,send_request,health,pid,sample_rate):
    self.assertEqual(sample_rate,BINDING['engine_rate'])
    state['refusals']+=1;self.assertEqual(pid,'200');self.assertTrue(health())
    if mode=='partial-tail':
     self.assertEqual([r['seq'] for r in records(read_log())],[1]);self.assertTrue(health())
     self.assertEqual([r['seq'] for r in records(read_log())],[1,2])
    if mode=='malformed-complete':records(read_log())
    self.assertFalse(state['loads']);self.assertFalse(state['plays']);self.assertFalse(state['selections']);self.assertEqual(state['commands'],0)
    if mode in ('refusal-fail','refusal-fail-callback'):return dict(status='FAIL',errors=['fixture refusal not enforced'],missing=[],evidence=[])
    return dict(status='PASS',errors=[],missing=[],evidence=[dict(kind='fixture actual admission refusal')])
   def run_commands(read_log,send_request,health,pid,setup_deck,sample_rate):
    self.assertEqual(sample_rate,BINDING['engine_rate'])
    self.assertEqual(state['refusals'],1)
    state['commands']+=1;self.assertEqual(pid,'200');captured=state['captures'];self.assertTrue(health());self.assertEqual(state['captures'],captured+1)
    for deck in range(4):
     try:setup_deck(deck)
     except loop.MissingEvidence as error:
      if mode!='setup-fail-callback':raise
      return dict(status='INCOMPLETE',errors=[],missing=[str(error)])
     if mode=='command-fail-callback':return dict(status='FAIL',errors=['fixture native command did not apply'],missing=[])
    self.assertEqual(read_log().splitlines()[0] if mode=='partial-tail' else read_log().strip(),'fixture SDK loop transcript');self.assertEqual(state['captures'],captured+1)
    self.assertEqual(read_log().splitlines()[0] if mode=='partial-tail' else read_log().strip(),'fixture SDK loop transcript');self.assertEqual(state['captures'],captured+2)
    send_request('snapshot request\n')
    return dict(status='PASS',errors=[],missing=[])
   def ready(text,pid):
    self.assertEqual(pid,'200');self.assertEqual(text.strip(),'fixture SDK loop transcript')
    return dict(data='/media/usb/sda1/MODS/community-mods/'+loop.ID+'/data') if mode!='unsafe-data' else dict(data='/tmp/fallback')
   def continuation(*args,**kwargs):
    self.assertEqual(state['order'][-1],'close')
    self.assertEqual(args[3],200)
    state['continuation']+=1;state['order'].append('continuation')
    return dict(status='PASS',post_deadline_wraps=2)
   result=None
   with ExitStack() as stack:
    stack.enter_context(patch.object(loop.runtime,'validate_package',side_effect=ValueError('native package/source closure mismatch') if mode in ('native-package','native-source') else None,return_value=dict(sha256={})))
    stack.enter_context(patch.object(loop,'PanelTransport',Panel));stack.enter_context(patch.object(loop,'PanelTransportSessions',Sessions))
    stack.enter_context(patch.object(loop,'run_continuation',side_effect=continuation));stack.enter_context(patch.object(loop,'run_public_runtime',side_effect=public));stack.enter_context(patch.object(loop,'run_observed_runtime',side_effect=observed))
    stack.enter_context(patch.object(loop,'run_refusals',side_effect=run_refusals));stack.enter_context(patch.object(loop,'run_commands',side_effect=run_commands));stack.enter_context(patch.object(loop,'ready',side_effect=ready))
    if expected:
     with self.assertRaisesRegex(expected[0],expected[1]):loop.run_loop_runtime(SSH,root,root,root/'out','d'*64,900,command,context)
    else:result=loop.run_loop_runtime(SSH,root,root,root/'out','d'*64,900,command,context)
   if mode in ('load-error','wrong-title'):
    self.assertIsNotNone(state['controller']);self.assertEqual(state['controller'].timeout,17)
   names={'out-usb-stage.txt','out-command-flags.txt','out-adapter-inputs.json','report.json','admission-refusals.json','loop-commands.json'}
   state['extra_files']=[str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() and (p.name in names or p.name.startswith(('command-read-','command-health-')))]
   return state,result
 def test_owned_normal_loader_wiring_and_atomic_request(self):
  state,result=self.case();self.assertEqual(result['status'],'PASS')
  self.assertEqual((state['staged'],state['flags'],state['public'],state['allow'],state['observed'],state['commands']),(1,1,1,1,1,1))
  self.assertEqual(state['loads'],[(d,180) for d in range(1,5)])
  self.assertEqual(state['plays'],[(d,'play',17) for d in range(1,5)])
  self.assertEqual(state['selections'],[1,2,3,4]);self.assertEqual(len(state['motions']),4);self.assertEqual(state['requests'],[b'snapshot request\n'])
  self.assertEqual(state['refusals'],1);self.assertEqual(result['admission_refusals']['status'],'PASS');self.assertTrue(state['extra_files'])
  self.assertEqual(state['continuation'],1);self.assertEqual(result['continuation']['post_deadline_wraps'],2)
  self.assertTrue(state['verify']);self.assertTrue(all(v==('200','2000') for v in state['verify']))
 def test_partial_receipt_eof_defers_only_fragment_until_next_capture(self):
  state,result=self.case('partial-tail');self.assertEqual(result['status'],'PASS')
  self.assertGreaterEqual(state['captures'],4)
 def test_complete_malformed_receipt_still_rejected(self):
  self.case('malformed-complete',expected=(ValueError,'exact loop receipt fields required'))
 def test_default_quiet_mode_omits_optional_logs_and_reports(self):
  state,result=self.case('quiet');self.assertEqual(result['status'],'PASS')
  self.assertFalse(state['extra_files']);self.assertEqual(state['commands'],1);self.assertTrue(state['requests'])
 def test_unapproved_never_loads_or_submits(self):
  state,result=self.case('unapproved');self.assertEqual(result['status'],'INCOMPLETE')
  self.assertEqual((state['allow'],state['observed'],state['commands'],state['refusals']),(0,0,0,0));self.assertFalse(state['loads']);self.assertFalse(state['requests'])
 def test_failed_refusal_phase_halts_positive_before_browse(self):
  state,result=self.case('refusal-fail');self.assertEqual(result['status'],'FAIL')
  self.assertEqual(state['refusals'],1);self.assertEqual(state['commands'],0)
  self.assertFalse(state['loads']);self.assertFalse(state['plays']);self.assertFalse(state['selections']);self.assertFalse(state['requests'])
  self.assertEqual(result['admission_refusals']['status'],'FAIL')
 def test_early_ownership_binding_and_probe_guards(self):
  for mode,text in [('bad-callback','callback must be callable'),('bad-debug','explicit boolean'),('native-package','package/source closure mismatch'),('native-source','package/source closure mismatch'),('owner','identity changed'),('bad-container','owned disposable'),('header','binding mismatch'),('position','binding mismatch'),('probe-hash','hash/size mismatch'),('driver','driver/receiver'),('receiver','driver/receiver'),('unsafe-usb','USB mount')]:
   with self.subTest(mode=mode):
    state,_=self.case(mode,(ValueError,text));self.assertEqual(state['staged'],0);self.assertEqual(state['public'],0)
 def test_usb_mount_and_exact_four_file_hashes(self):
  for mode,error,text in [('usb-refused',RuntimeError,'mounted writable'),('stage-hash',ValueError,'staged package hashes'),('stage-extra',ValueError,'staged package hashes')]:
   with self.subTest(mode=mode):
    state,_=self.case(mode,(error,text));self.assertEqual(state['public'],0);self.assertEqual(state['flags'],0)
 def test_separate_startup_command_gates_required(self):
  state,_=self.case('flag',(ValueError,'startup gates'));self.assertEqual(state['public'],0)
 def test_selection_and_real_mapping_before_commands(self):
  for mode,text in [('selection','current independently observed'),('unmapped','normal loader mapping absent'),('mapped-hash','loaded probe hash mismatch')]:
   with self.subTest(mode=mode):
    state,_=self.case(mode,(Exception,text));self.assertFalse(state['loads']);self.assertFalse(state['requests'])
 def test_load_timeout_restored_including_exception_and_title_failure(self):
  self.case('load-error',(loop.MissingEvidence,'fixture load timeout'))
  self.case('wrong-title',(loop.MissingEvidence,'exact loaded test title'))
 def test_failed_health_capture_prevents_browse_or_positive_commands(self):
  state,_=self.case('health-fail',(ValueError,'identity/health changed'))
  self.assertEqual(state['captures'],1);self.assertEqual(state['commands'],0)
  self.assertFalse(state['selections']);self.assertFalse(state['loads']);self.assertFalse(state['requests'])

 def test_setup_play_requires_observed_source_motion(self):
  for mode,reason in [('stationary','source did not advance'),('missing-position','position unavailable before PLAY')]:
   with self.subTest(mode=mode):
    state,_=self.case(mode,(loop.MissingEvidence,reason))
    self.assertEqual(state['loads'],[(1,180)]);self.assertEqual(state['plays'],[(1,'play',17)])
    self.assertFalse(state['motions']);self.assertFalse(state['requests']);self.assertEqual(state['controller'].timeout,17)
 def test_motion_wait_uses_real_bound_history_and_half_second_advance(self):
  baseline=position_packet([44100]*6)
  before=dict(positions=baseline,boottime_ns=baseline['native_boottime_ns'])
  def execute(value):
   def wait(predicate,label):
    if not predicate(value):raise loop.MissingEvidence('source motion not qualified')
    return value
   controller=SimpleNamespace(pid='200',position_binding=BINDING,wait=wait)
   return loop.wait_playback_motion(controller,1,before)
  advanced=motion_snapshot();self.assertIs(execute(advanced),advanced)
  with self.assertRaises(loop.MissingEvidence):execute(motion_snapshot(False))
  small=motion_snapshot();packet=position_packet([44100+1000*i for i in range(1,7)],start_ns=2000000000)
  small.update(positions=packet,boottime_ns=packet['native_boottime_ns'])
  with self.assertRaises(loop.MissingEvidence):execute(small)
  stale=motion_snapshot();stale['boottime_ns']+=2000000000
  with self.assertRaises(loop.MissingEvidence):execute(stale)
  # Sparse positive advances do not need the cadence used to exclude stationary-loop aliasing.
  for frames in ([44100+4410*i for i in range(1,7)],[48510,57330,70560]):
   packet=position_packet(frames,start_ns=2000000000,step_ns=500000000)
   sparse=dict(positions=packet,boottime_ns=packet['native_boottime_ns'])
   proof=loop.position_motion(packet,1,'200',BINDING,after_ns=baseline['native_monotonic_ns'],now_boottime_ns=sparse['boottime_ns'])
   self.assertNotEqual(proof['state'],'MOVING');self.assertTrue(proof['binding_verified']);self.assertTrue(proof['fresh'])
   self.assertIs(execute(sparse),sparse)
  changed=motion_snapshot();changed['positions']['samples'][-1]['decks'][0]['title']='different track'
  with self.assertRaises(loop.MissingEvidence):execute(changed)
  backwards=position_packet([48510,52920,48510,61740,66150,70560],start_ns=2000000000,step_ns=500000000)
  with self.assertRaises(loop.MissingEvidence):execute(dict(positions=backwards,boottime_ns=backwards['native_boottime_ns']))



 def test_motion_wait_rejects_nonfinite_baseline_and_positive_samples(self):
  def reject_wait(predicate,label):
   if not predicate(candidate):raise loop.MissingEvidence('nonfinite source position rejected')
   self.fail('nonfinite source position was accepted')
  controller=SimpleNamespace(pid='200',position_binding=BINDING,wait=reject_wait)
  for bad in (float('nan'),float('inf'),float('-inf')):
   with self.subTest(value=bad,where='baseline'):
    packet=position_packet([44100]*6);packet['samples'][-1]['decks'][0]['pos_s']=bad
    before=dict(positions=packet,boottime_ns=packet['native_boottime_ns'])
    with self.assertRaises(loop.MissingEvidence):loop.wait_playback_motion(controller,1,before)
   packet=position_packet([44100]*6);before=dict(positions=packet,boottime_ns=packet['native_boottime_ns'])
   candidate=motion_snapshot();candidate['positions']['samples'][-1]['decks'][0]['pos_s']=bad
   with self.subTest(value=bad,where='native_classifier'),self.assertRaises(ValueError):
    loop.wait_playback_motion(controller,1,before)
   # Keep the manual sparse-history fallback independently guarded even if
   # the shared classifier returns a generic fresh/bound UNKNOWN result.
   with self.subTest(value=bad,where='sparse_fallback'),patch.object(loop,'position_motion',return_value=dict(state='UNKNOWN',binding_verified=True,fresh=True)):
    with self.assertRaises(loop.MissingEvidence):loop.wait_playback_motion(controller,1,before)
 def test_failure_callback_only_runs_for_nonpassing_result(self):
  state,result=self.case('pass-callback');self.assertEqual(result['status'],'PASS');self.assertFalse(state['failures'])
  for mode,status in [('refusal-fail-callback','FAIL'),('setup-fail-callback','INCOMPLETE'),('command-fail-callback','FAIL')]:
   with self.subTest(mode=mode):
    state,result=self.case(mode);self.assertEqual(result['status'],status);self.assertEqual(state['failures'],[status])

 def test_quantize_setup_observed_before_any_load_or_sdk_request(self):
  for mode in ('quantize-on','quantize-string-on','quantize-warm-off'):
   state,result=self.case(mode);self.assertEqual(result['status'],'PASS');self.assertEqual(state['quantize'],[17])
  for mode in ('quantize-mixed','quantize-no-transition'):
   with self.subTest(mode=mode):
    state,_=self.case(mode,(loop.MissingEvidence,'Quantize'))
    self.assertFalse(state['selections']);self.assertFalse(state['loads']);self.assertFalse(state['plays']);self.assertFalse(state['requests'])
    self.assertEqual(len(state['quantize']),0 if mode=='quantize-mixed' else 1)
 def test_quantize_helper_requires_uniform_state_and_all_four_newer_off_receipts(self):
  class QuantizeController:
   _stream=staticmethod(RealController._stream)
   _newer=staticmethod(RealController._newer)
   def __init__(self,before,after):self.before=before;self.after=after;self.inputs=0;self.waits=0
   def observe(self):return self.before
   def quantize(self):self.inputs+=1
   def wait(self,predicate,label,**options):
    self.waits+=1
    # Actual allow_partial returns the latest observation even if unqualified.
    value=self.after if self.after is not None else self.before
    if not predicate(value) and not options.get('allow_partial'):raise loop.MissingEvidence('Quantize not observed')
    return value
  for off,on in (([0]*4,[1]*4),(['0']*4,['1']*4),([0,'0',0,'0'],[1,'1',1,'1'])):
   already=QuantizeController(quantize_snapshot(off),None);loop.ensure_quantize_off(already)
   self.assertEqual((already.inputs,already.waits),(0,0))
   changed=QuantizeController(quantize_snapshot(on),quantize_snapshot(off,2));loop.ensure_quantize_off(changed)
   self.assertEqual((changed.inputs,changed.waits),(1,1))
  for values in ([0,1,0,1],[True]*4,[False]*4,[0.0]*4,['01']*4,['on']*4,['false']*4,[2]*4):
   unknown=QuantizeController(quantize_snapshot(values),None)
   with self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(unknown)
   self.assertEqual(unknown.inputs,0)
  one_stale=quantize_snapshot([0]*4,2);one_stale['decks'][3]['streams']['quantize']['revision']='1'
  for after in (quantize_snapshot([1]*4,2),quantize_snapshot([0]*4,1),quantize_snapshot([0,0,0,1],2),one_stale,quantize_snapshot(['on']*4,2)):
   failed=QuantizeController(quantize_snapshot([1]*4),after)
   with self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(failed)
   self.assertEqual(failed.inputs,1)

 def test_unknown_quantize_establishes_once_then_observes_actual_value(self):
  class WarmController:
   _stream=staticmethod(RealController._stream)
   _newer=staticmethod(RealController._newer)
   _native_now=staticmethod(lambda snapshot,deck:snapshot.get('native_now'))
   def __init__(self,before,receipts):self.before=before;self.receipts=receipts;self.inputs=0;self.waits=0;self.setup_attempts=set()
   def observe(self):return self.before
   def quantize(self):self.inputs+=1
   def wait(self,predicate,label,**options):
    value=self.receipts[min(self.waits,len(self.receipts)-1)];self.waits+=1
    if not predicate(value) and not options.get('allow_partial'):raise loop.MissingEvidence('first receipt missing')
    return value
  first_off=quantize_snapshot(['0']*4,2,1100000000,1200000000)
  first_on=quantize_snapshot(['1']*4,2,1100000000,1200000000)
  next_off=quantize_snapshot(['0']*4,3,1300000000,1400000000)
  for initial,receipts,count in (([None]*4,[first_off],1),([None]*4,[first_on,next_off],2),([None,1,None,1],[first_off],1),([0,None,0,None],[first_on,next_off],2)):
   with self.subTest(initial=initial,count=count):
    controller=WarmController(quantize_snapshot(initial),receipts);loop.ensure_quantize_off(controller)
    self.assertEqual(controller.inputs,count);self.assertIn((0,'quantize'),controller.setup_attempts)
  for values in ([0,1,None,None],['on',None,None,None],[True,None,None,None]):
   controller=WarmController(quantize_snapshot(values),[first_off])
   with self.subTest(initial=values),self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(controller)
   self.assertEqual(controller.inputs,0)
  no_clock=quantize_snapshot([None]*4);no_clock['native_now']=None
  controller=WarmController(no_clock,[first_off])
  with self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(controller)
  self.assertEqual(controller.inputs,0)
  missing_unit=quantize_snapshot([None]*4)
  for deck in missing_unit['decks']:deck['streams'].pop('loop')
  # A fresh process-bound clock and first actual Q receipts establish units;
  # an unchanged warm-start loop observer need not have published anything.
  controller=WarmController(missing_unit,[first_off]);loop.ensure_quantize_off(controller)
  self.assertEqual(controller.inputs,1)
  first_units=quantize_snapshot(['1']*4,2,1100000000,1200000000)
  later_off=quantize_snapshot(['0']*4,3,1300000000,1400000000)
  for index in range(4):
   first_units['decks'][index]['streams']['quantize']['unit']=str(501+index)
   later_off['decks'][index]['streams']['quantize']['unit']=str(501+index)
  controller=WarmController(missing_unit,[first_units,later_off]);loop.ensure_quantize_off(controller)
  self.assertEqual(controller.inputs,2)
  for bad in ('0',0,-1,True,1.0,'01','nan',2**64):
   malformed=quantize_snapshot(['0']*4,2,1100000000,1200000000);malformed['decks'][0]['streams']['quantize']['unit']=bad
   controller=WarmController(missing_unit,[malformed])
   with self.subTest(unit=bad),self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(controller)
   self.assertEqual(controller.inputs,1)
   known=quantize_snapshot([0,None,None,None]);known['decks'][0]['streams']['quantize']['unit']=bad
   controller=WarmController(known,[first_off])
   with self.subTest(known_unit=bad),self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(controller)
   self.assertEqual(controller.inputs,0)
  changed=quantize_snapshot(['0']*4,2,1100000000,1200000000);changed['decks'][2]['streams']['quantize']['unit']='999'
  future=quantize_snapshot(['0']*4,2,1210000001,1200000000)
  for received,error in ((quantize_snapshot([None]*4),loop.MissingEvidence),(quantize_snapshot(['0',None,'0','0'],2,1100000000,1200000000),loop.MissingEvidence),(quantize_snapshot(['0']*4,0,1100000000,1200000000),loop.MissingEvidence),(quantize_snapshot(['0']*4,2,1000000000,1200000000),loop.MissingEvidence),(quantize_snapshot(['0']*4,2,900000000,1200000000),loop.MissingEvidence),(changed,ValueError),(future,ValueError)):
   initial=quantize_snapshot([None,None,0,None] if received is changed else [None]*4)
   controller=WarmController(initial,[received])
   with self.subTest(received=received,error=error),self.assertRaises(error):loop.ensure_quantize_off(controller)
   self.assertEqual(controller.inputs,1)
   with self.assertRaises(loop.MissingEvidence):loop.ensure_quantize_off(controller)
   self.assertEqual(controller.inputs,1)

 def test_quantize_input_uses_fresh_load_receiver_before_selection(self):
  for mode in ('quantize-on','quantize-warm-off'):
   with self.subTest(mode=mode):
    state,result=self.case(mode);self.assertEqual(result['status'],'PASS')
    trace=state['order'];renew=trace.index('renew:load-deck1');pressed=trace.index('quantize');selected=trace.index('selection:1')
    self.assertLess(trace.index('renew:navigation'),renew);self.assertLess(renew,pressed);self.assertLess(pressed,selected)
    self.assertEqual(len(state['quantize']),1)
 def test_real_usb_data_dir_no_ram_fallback(self):
  state,_=self.case('unsafe-data',(ValueError,'probe data_dir differs from staged USB owner'));self.assertFalse(state['requests'])
if __name__=='__main__':unittest.main()
