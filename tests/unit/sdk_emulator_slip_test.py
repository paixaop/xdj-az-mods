"""Raw callback diagnostics; no enabled/deck/active field projection."""
from pathlib import Path
import sys,unittest,copy,tempfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
from sdk_emulator_controls import MissingEvidence,Controller as RealController
from sdk_emulator_slip import parse_capture,run_slip_trial
B=dict(profile='2.00',target=0x1d02e48,caller=0x21232c8)
def text(raw=4294967295):
 return ('SDK_SLIP_CAPTURE_READY v=1 profile=2.00 target=0x1d02e48 caller=0x21232c8 max_records=512 ring=128 semantics=raw_no_deck\n'
 'SDK_SLIP_CAPTURE v=1 seq=1 observed_ns=100 raw=%d receiver=0x8000 caller=0x21232c8\n'
 'SDK_SLIP_CAPTURE_STATS v=1 saved=1 emitted=1 dropped=0 rejected=0 missed=0 reason=running\n')%raw
class Slip(unittest.TestCase):
 def test_full_w1_is_preserved_without_boolean_or_deck_projection(self):
  r=parse_capture(text(),B,200);self.assertEqual(r['records'][0]['raw'],2**32-1)
  self.assertNotIn('enabled',r);self.assertNotIn('deck',r)
 def test_exact_ready_record_and_stats_guards(self):
  for old,new in [('profile=2.00','profile=1.30'),('target=0x1d02e48','target=0x1d02e40'),('caller=0x21232c8','caller=0x21232c0'),('max_records=512','max_records=513'),('ring=128','ring=129'),('seq=1','seq=2'),('observed_ns=100','observed_ns=20000001'),('raw=4294967295','raw=4294967296'),('receiver=0x8000','receiver=0x0'),('saved=1','saved=0'),('emitted=1','emitted=2'),('reason=running','reason=unknown')]:
   with self.subTest(old=old),self.assertRaises(ValueError):parse_capture(text().replace(old,new),B,200)
  with self.assertRaises(ValueError):parse_capture(text()+text(),B,200)
 def test_missing_ready_or_stats_does_not_qualify(self):
  for prefix in ('SDK_SLIP_CAPTURE_READY','SDK_SLIP_CAPTURE_STATS'):
   with self.subTest(prefix=prefix),self.assertRaises(MissingEvidence):parse_capture('\n'.join(l for l in text().splitlines() if not l.startswith(prefix)),B,200)
 def test_record_before_ready_is_allowed_by_actual_worker_publication_order(self):
  lines=text().splitlines();self.assertEqual(len(parse_capture('\n'.join([lines[1],lines[0],lines[2]]),B,200)['records']),1)
 def test_loss_and_failed_install_remain_unqualified(self):
  for change in ('dropped=1','rejected=1','missed=1','reason=install_failed'):
   old=change.split('=')[0]+'='+('running' if change.startswith('reason') else '0')
   with self.subTest(change=change),self.assertRaises(MissingEvidence):parse_capture(text().replace(old,change),B,200)
 def trial(self,mode='ok'):
  calls=[];n=[100];native=[1000];events=[];alive=[True];trace=[]
  class C:
   timeout=30;configured_control_timeout=30;interval=.2
   def _now(self):return n[0]
   def _owner(self):pass
   def sleep(self,x):n[0]+=x;native[0]+=int(x*1e9)
   def load(self,deck):
    calls.append(('load',deck));native[0]+=10
    if mode!='no-load-event':events.append(dict(seq=1,observed_ns=native[0],raw=0,receiver=0x8000,caller=B['caller']))
   def wait(self,p,label,**kw):
    s={'decks':[dict(loaded=True,title='track')]};assert p(s);return s
   def button(self,deck,name):
    calls.append((name,deck));trace.append(('button',name));native[0]+=10
    if mode!='no-event':events.append(dict(seq=len(events)+1,observed_ns=native[0],raw=1 if len(calls)==2 else 0,receiver=0x8000,caller=B['caller']))
    if mode=='changed':alive[0]=False
   def screenshot(self,name):
    trace.append(('ui-read',name));return RealController.screenshot(self,name)
   def _qmp(self,command):
    self._owner()
    if mode=='shot-identity' or (mode=='shot-identity-after-input' and len(calls)>1):alive[0]=False;raise OSError('QMP disconnected')
    if mode=='no-shot':return {}
    path=Path(command['arguments']['filename']).name
    (self.work_dir/path).write_bytes(b'P6\n1 1\n255\n\0\0\0');return {}
  def read():
   trace.append(('candidate-read',None));native[0]+=10
   return dict(records=copy.deepcopy(events),native_now_ns=native[0],stats={'emitted':len(events)})
  def prepare(c):c.timeout=180;return dict(status='PASS',selected_title='track',evidence='native selected row')
  def verify():
   if mode=='false-identity':return False
   if not alive[0]:raise ValueError('pinned native changed')
   return True
  c=C();c.trace=trace;temporary=tempfile.TemporaryDirectory();self.addCleanup(temporary.cleanup)
  c.work_dir=Path(temporary.name);c.output=c.work_dir/'controls';c.output.mkdir()
  if mode=='expired':n[0]=200
  def readiness(controller):
   self.assertIs(controller,c)
   if mode=='pending-load-ui':return False
   if mode=='pending-after-one' and calls.count(('slip',1)):return False
   if mode=='unbound-jog':
    import struct
    from sdk_emulator_jog import read_frame
    path=c.work_dir/'jog.shm';data=bytearray(1<<20)
    struct.pack_into('<IIHHI',data,0,0x31474f4a,2,320,240,1);path.write_bytes(data)
    return read_frame(path,0,verify)
   return True
  return lambda:run_slip_trial(c,prepare,['track'],read,verify,150,ready_for_slip=None if mode=='no-readiness' else readiness),calls,c
 def test_actual_one_deck_executor_load_then_two_presses_and_raw_only_result(self):
  run,calls,c=self.trial();r=run();self.assertEqual(calls,[('load',1),('slip',1),('slip',1)]);self.assertEqual(r['status'],'PASS');self.assertEqual(c.timeout,30)
  self.assertEqual([s['label'] for s in r['stages']],['documented_OFF_after_LOAD','documented_ON_after_SLIP','documented_OFF_after_SLIP'])
  self.assertNotIn('enabled',r)
 def test_supporting_screenshot_artifact_loss_does_not_gate_raw_actions(self):
  run,calls,c=self.trial('no-shot');r=run()
  self.assertEqual(calls,[('load',1),('slip',1),('slip',1)])
  self.assertEqual(r['status'],'PASS')
  self.assertTrue(all(stage.get('screenshot_missing') for stage in r['stages']))
 def test_identity_change_during_optional_screenshot_still_stops_controls(self):
  run,calls,c=self.trial('shot-identity')
  with self.assertRaisesRegex(ValueError,'pinned native changed'):run()
  self.assertEqual(calls,[('load',1)])
 def test_documented_load_off_needs_no_unchanged_callback(self):
  run,calls,c=self.trial('no-load-event');r=run()
  self.assertEqual(r['stages'][0]['records'],[])
  self.assertEqual(calls,[('load',1),('slip',1),('slip',1)])
  self.assertEqual(r['status'],'PASS')
 def test_changed_identity_prevents_second_press(self):
  run,calls,c=self.trial('changed')
  with self.assertRaisesRegex(ValueError,'pinned native changed'):run()
  self.assertEqual(calls,[('load',1),('slip',1)])
 def test_missing_callback_never_replays_or_blindly_sends_second_press(self):
  run,calls,c=self.trial('no-event')
  with self.assertRaises(MissingEvidence):run()
  self.assertEqual(calls,[('load',1),('slip',1)]);self.assertEqual(c.timeout,30)
 def test_actual_ui_read_precedes_candidate_wait_after_each_slip_input(self):
  run,calls,c=self.trial();run()
  for i,event in enumerate(c.trace):
   if event==('button','slip'):self.assertEqual(c.trace[i+1][0],'ui-read')
 def test_ui_read_survives_candidate_callback_timeout(self):
  run,calls,c=self.trial('no-event')
  with self.assertRaises(MissingEvidence):run()
  i=c.trace.index(('button','slip'));self.assertEqual(c.trace[i+1][0],'ui-read')
  self.assertTrue((c.output/'slip-documented-on-after-slip.ppm').is_file())
  self.assertEqual(calls,[('load',1),('slip',1)])
 def test_identity_changed_in_post_input_ui_read_stops_before_candidate_or_second_press(self):
  run,calls,c=self.trial('shot-identity-after-input')
  with self.assertRaisesRegex(ValueError,'pinned native changed'):run()
  self.assertEqual(calls,[('load',1),('slip',1)])
 def test_known_pending_native_ui_refuses_slip_despite_loaded_title(self):
  # Retained run: WAVEFORM/title present while NOW LOADING remained visible.
  run,calls,c=self.trial('pending-load-ui')
  with self.assertRaisesRegex(MissingEvidence,'readiness'):run()
  self.assertEqual(calls,[('load',1)])
 def test_missing_independent_readiness_never_defaults_to_ready(self):
  run,calls,c=self.trial('no-readiness')
  with self.assertRaisesRegex(MissingEvidence,'readiness'):run()
  self.assertEqual(calls,[('load',1)])
 def test_actual_stable_jog_frame_without_native_deck_or_indicator_binding_cannot_authorize_slip(self):
  run,calls,c=self.trial('unbound-jog')
  with self.assertRaisesRegex(MissingEvidence,'readiness'):run()
  self.assertEqual(calls,[('load',1)])
 def test_readiness_is_rechecked_before_the_second_distinct_press(self):
  run,calls,c=self.trial('pending-after-one')
  with self.assertRaisesRegex(MissingEvidence,'readiness'):run()
  self.assertEqual(calls,[('load',1),('slip',1)])
 def test_false_identity_never_sends_input(self):
  run,calls,c=self.trial('false-identity')
  with self.assertRaises(ValueError):run()
  self.assertEqual(calls,[])
 def test_expired_total_budget_sends_nothing(self):
  run,calls,c=self.trial('expired')
  with self.assertRaises(MissingEvidence):run()
  self.assertEqual(calls,[])
if __name__=='__main__':unittest.main()
