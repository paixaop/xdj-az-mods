#!/usr/bin/env python3
from pathlib import Path
import sys,unittest,math,tempfile,copy
from unittest.mock import patch,Mock
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
import sdk_emulator_cue_runtime as cue

def base(**changes):
 r={k:0 for k in cue.BASE};r.update(v=1,seq=1,event='snapshot',query_ns=100,engaged=-1,outcome=-1,cues_rc=0,load_valid=1,native_load=7,load_unit=11,load_ns=1,cues_load=7,cues_rev=9);r.update(changes)
 return 'SDK_CUE '+' '.join(k+'='+str(r[k]) for k in sorted(r))+'\n'
def entry(**changes):
 r={k:0 for k in cue.ENTRY};r.update(v=1,snapshot_seq=1,load=7,revision=9,classification=1,entry_id=1,valid=1,type=1,start=1.25,comment_hex='-');r.update(changes)
 return 'SDK_CUE_ENTRY '+' '.join(k+'='+str(r[k]) for k in sorted(r))+'\n'

class ActiveLoopFixture:
 def __init__(self,create=False,partial=False,missing=False,active=False):
  def row(identity,start,kind=2,classification=2,enabled=0):
   return dict(v=1,snapshot_seq=1,deck=0,load=7,revision=10,classification=classification,entry_id=identity,slot=identity-1 if classification==1 else 0,type=kind,valid=63,start=start,end=start+1 if kind==2 else 0.,rgb=0,comment='original cue ✓',saved=1,modified=0,auto_active=enabled)
  entries=[row(1,1.25,1,1),row(2,4.25,1,1)]
  entries+=([row(i+1,20.+i,1) for i in range(8)] if create else [row(1,2.,enabled=int(active)),row(2,4.),row(3,20.,1)])
  self.state=dict(native_load=7,cues_rev=10,entries=entries,engaged=0,loop_valid=0,loop_in=0.,loop_out=0.)
  self.original=copy.deepcopy(self.state);self.calls=[];self.rows=[];self.position=0.;self.paused=False;self.autos=0;self.partial=partial;self.missing=missing
 def cues(self,deck,predicate=lambda state:True):
  value=copy.deepcopy(self.state)
  if not predicate(value):raise cue.MissingEvidence('native effect missing')
  return value
 def submit(self,deck,op,**args):
  if args.get('load',7)!=7:raise AssertionError('stale native load')
  self.calls.append((op,args));state=self.state
  if op in (13,15,16):
   if args['revision']!=state['cues_rev']:raise AssertionError('stale revision')
   selected=[e for e in state['entries'] if e['classification']==2 and e['entry_id']==args['entry_id']]
   if len(selected)!=1:raise AssertionError('stale entry ID')
   selected=selected[0]
  if op==16:
   self.autos+=1
   if self.missing:return
   if args['position'] and not (self.partial and self.autos==2):
    for row in state['entries']:
     if row['classification']==2 and row['type']==2:row['auto_active']=0
   selected['auto_active']=args['position']
  elif op==13:
   state.update(loop_valid=1,engaged=1,loop_in=selected['start'],loop_out=selected['end']);self.position=selected['start'];self.paused=True
  elif op==15:state['entries'].remove(selected)
  elif op==17:self.position=next(e['start'] for e in state['entries'] if e['classification']==1 and e['entry_id']==args['entry_id']);self.paused=False
  elif op==1:state['loop_in']=self.position
  elif op==2:state.update(loop_valid=1,engaged=1,loop_out=self.position+.25)
  elif op==4:state['engaged']=0
  elif op==14:
   row=copy.deepcopy(state['entries'][-1]);row.update(entry_id=99,type=2,start=cue.native_saved_position(state['loop_in'],44100),end=cue.native_saved_position(state['loop_out'],44100),auto_active=0,comment='')
   state['entries'].append(row)
  else:raise AssertionError('unrelated SDK family '+str(op))
  if op in (14,15,16):
   state['cues_rev']+=1
   for index,row in enumerate(e for e in state['entries'] if e['classification']==2):row['entry_id']=state['cues_rev']*20+index
  return {'receipt_ns':0}
 def positions(self,deck):
  if not self.paused:self.position+=.1
  return [(1,self.position if self.paused else self.position-.01),(2,self.position)]

class CueTests(unittest.TestCase):
 def test_appended_creation_requests_preserve_public_arguments(self):
  for op in (27,28):
   line=cue.request(723,'submit',identity=7,deck=0,op=op,load=1,revision=4,timeout=10000000000,flags=7,classification=2,position=0,end=2 if op==28 else 0,rgb=0x00c3ff,comment='Native creation ✓')
   fields=dict(item.split('=',1) for item in line.split())
   self.assertEqual(int(fields['op']),op)
   self.assertEqual(int(fields['flags']),7)
   self.assertEqual(int(fields['revision']),4)
   self.assertEqual(cue.compare_timestamps(float(fields['position']),0,units_per_second=1,sample_rate=48000),0)
   self.assertEqual(bytes.fromhex(fields['comment_hex']).decode(),'Native creation ✓')
  for op in (29,2**32,-1):
   with self.subTest(op=op),self.assertRaises(ValueError):
    cue.request(723,'submit',identity=7,op=op)
 def test_capture_bracket_waits_for_post_render_source_publication(self):
  for rate in (44100,48000):
   with self.subTest(rate=rate):
    rows=[(1141986510154,154400/rate),(1142879487264,154400/rate),(1143120268164,154528/rate),(1143460062545,154784/rate)]
    bracket=cue.source_capture_bracket(rows,1142584839508,1142792103983,rate)
    self.assertEqual(len(bracket),3)
    self.assertEqual(cue.compare_timestamps(bracket[-1][0],rows[2][0]),0)
    self.assertTrue(cue.source_between(154464/rate,bracket[0][1],bracket[-1][1],rate))
    self.assertFalse(cue.source_between(154398/rate,bracket[0][1],bracket[-1][1],rate))
    self.assertFalse(cue.source_between(154530/rate,bracket[0][1],bracket[-1][1],rate))
    self.assertFalse(cue.source_between(154464/rate,rows[0][1],rows[1][1],rate))
 def test_capture_bracket_requires_observed_advance_and_continuous_motion(self):
  for rows,message in (([(100,1.),(300,1.)],'advance'),([(300,2.)],'before')):
   with self.subTest(rows=rows),self.assertRaisesRegex(cue.MissingEvidence,message):
    cue.source_capture_bracket(rows,150,250,44100)
  with self.assertRaisesRegex(ValueError,'backwards'):
   cue.source_capture_bracket([(100,1.),(200,.5),(300,2.)],150,250,44100)
  with self.assertRaisesRegex(ValueError,'clock'):
   cue.source_capture_bracket([(100,1.),(100,1.1),(300,2.)],150,250,44100)
 def test_selected_comment_observes_utf8_preserves_other_cues_and_restores(self):
  for fault in ('none','missing','other-cue','restore'):
   with self.subTest(fault=fault):
    class Fixture:
     def __init__(self):
      self.state=dict(native_load=7,cues_rev=10,entries=[dict(classification=1,entry_id=1,type=1,valid=5,start=1.,end=0.,saved=1,comment='keep'),dict(classification=2,entry_id=2,type=1,valid=5,start=2.,end=0.,saved=1,comment='original')]);self.calls=[]
     def cues(self,deck,predicate=lambda state:True):
      if not predicate(self.state):raise cue.MissingEvidence('native comment effect missing')
      return copy.deepcopy(self.state)
     def submit(self,deck,op,**arguments):
      if op!=25:raise AssertionError('unrelated SDK family')
      self.calls.append(op)
      if arguments['entry_id']!=self.state['entries'][1]['entry_id'] or arguments['revision']!=self.state['cues_rev']:raise AssertionError('stale comment selection')
      if not (fault=='missing' or fault=='restore' and len(self.calls)==2):self.state['entries'][1]['comment']=arguments['comment']
      if fault=='other-cue':self.state['entries'][0]['comment']='changed'
      self.state['cues_rev']+=1;self.state['entries'][1]['entry_id']+=1
    fixture=Fixture()
    with patch.object(cue,'CueClient',return_value=fixture):
     report=cue.run_memory_comment(None,None,None,200,decks=(1,),sample_rate=44100,entry_id=2)
    self.assertEqual(report['status'],'PASS' if fault=='none' else 'FAIL' if fault=='other-cue' else 'INCOMPLETE',report)
    self.assertEqual(fixture.calls,[25,25] if fault in ('none','restore') else [25])
    if fault=='none':self.assertEqual(report['decks']['1']['operations'],[25]);self.assertEqual(fixture.state['entries'][1]['comment'],'original')
 def test_selected_comment_dispatch_cannot_claim_the_full_group(self):
  for operations in ([25],list(cue.OPS)):
   report=dict(status='PASS',decks={'1':dict(status='PASS',operations=operations)})
   with patch.object(cue,'run_memory_comment',return_value=report) as selected,patch.object(cue,'run_commands') as group:
    if operations==[25]:self.assertEqual(cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,decks=(1,),selection='memory-comment',memory_entry_id=2)['requested_operations'],[25])
    else:
     with self.assertRaisesRegex(ValueError,'cannot claim complete'):cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,decks=(1,),selection='memory-comment',memory_entry_id=2)
   selected.assert_called_once();group.assert_not_called()
 def test_comment_entry_selection_refuses_before_client_activity(self):
  with patch.object(cue,'CueClient') as client:
   for value in (0,-1,2**32,True):
    with self.subTest(value=value),self.assertRaisesRegex(ValueError,'uint32'):
     cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='memory-comment',memory_entry_id=value)
   client.assert_not_called()
 def test_selected_memory_set_checks_native_point_other_cues_and_cleanup(self):
  for fault in ('none','wrong-point','other-cue','not-saved','cleanup'):
   with self.subTest(fault=fault):
    class Fixture:
     def __init__(self):
      self.state=dict(native_load=7,cues_rev=10,main_valid=0,main_s=0.,entries=[dict(classification=1,entry_id=1,type=1,valid=5,start=1.,end=0.,saved=1,comment='original')]);self.calls=[]
     def cues(self,deck,predicate=lambda state:True):
      if not predicate(self.state):raise cue.MissingEvidence('native Memory effect missing')
      return copy.deepcopy(self.state)
     def submit(self,deck,op,**arguments):
      self.calls.append(op)
      if op==21:
       self.state['entries'].append(dict(classification=2,entry_id=2,type=1,valid=5,start=2.5+(1. if fault=='wrong-point' else 0.),end=0.,saved=int(fault!='not-saved'),comment=''))
       if fault=='other-cue':self.state['entries'][0]['comment']='changed'
      elif op==22:
       if arguments['revision']!=self.state['cues_rev']:raise AssertionError('stale Memory deletion revision')
       self.state['entries'].pop()
       if fault=='cleanup':self.state['entries'][0]['comment']='changed'
      else:raise AssertionError('unrelated SDK family')
      self.state['cues_rev']+=1
    fixture=Fixture()
    with patch.object(cue,'CueClient',return_value=fixture),patch.object(cue,'main_cue_roundtrip'):
     report=cue.run_memory_set(None,None,None,200,setup_deck=lambda deck:None,read_positions=lambda deck:[(1,2.5),(2,2.5)],sample_rate=44100)
    self.assertEqual(report['status'],'PASS' if fault=='none' else 'FAIL',report)
    self.assertEqual(fixture.calls,[21,22] if fault in ('none','cleanup') else [21])
    if fault=='none':self.assertEqual(report['decks']['0']['operations'],[21])
 def test_selected_memory_set_dispatch_cannot_run_or_claim_full_group(self):
  for operations in ([21],list(cue.OPS)):
   result=dict(status='PASS',decks={'0':dict(status='PASS',operations=operations)})
   with patch.object(cue,'run_memory_set',return_value=result) as setter,patch.object(cue,'run_commands') as group:
    if operations==[21]:
     report=cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='memory-set')
     self.assertEqual(report['requested_operations'],[21])
    else:
     with self.assertRaisesRegex(ValueError,'cannot claim complete'):cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='memory-set')
   setter.assert_called_once();group.assert_not_called()
 def test_refusal_phase_checks_all26_without_positive_setup_or_native_claims(self):
  calls=[]
  fake=type('Client',(),{'submit':lambda self,deck,op,**args:calls.append((deck,op,args))})()
  with patch.object(cue,'CueClient',return_value=fake),patch.object(cue,'run_commands') as positive:
   report=cue.run_test_phase('refusals',None,None,None,200,setup_deck=lambda _:self.fail('positive fixture executed'),read_positions=None,decks=(0,3))
  positive.assert_not_called()
  self.assertEqual([(deck,op) for deck,op,_ in calls],[(deck,op) for deck in (0,3) for op in range(1,27)])
  self.assertTrue(all(args['load']==2**64-1 for _,_,args in calls))
  self.assertEqual(report['decks']['3']['operations'],list(range(1,27)))
  self.assertIn('no native application',report['scope'])
 def test_full_command_coverage_cannot_borrow_receipts_from_other_decks(self):
  rows=[dict(operation=operation,terminal=dict(deck=0)) for operation in cue.OPS]
  rows+=[dict(operation=23,terminal=dict(deck=1)),dict(operation=24,terminal=dict(deck=1))]
  self.assertEqual(cue.completed_cue_operations(rows,0),list(cue.OPS))
  self.assertEqual(cue.completed_cue_operations(rows,1),[23,24])
  self.assertEqual(cue.completed_cue_operations(rows,2),[])
 def test_selected_hot_clear_checks_storage_other_cues_and_transport(self):
  for fault in ('none','still-stored','other-cue','paused','no-fixture'):
   with self.subTest(fault=fault):
    class Fixture:
     def __init__(self):
      self.state=dict(native_load=7,cues_rev=10,entries=[dict(classification=1,entry_id=1,valid=1,type=1,start=1.,end=0.,comment='keep'),dict(classification=1,entry_id=8,valid=1,type=1,start=8.,end=0.,comment='clear')]);self.calls=[];self.position=10.;self.now=0.
      if fault=='no-fixture':self.state['entries'].pop()
     def cues(self,deck,predicate=lambda state:True):
      if not predicate(self.state):raise cue.MissingEvidence('native stored deletion missing')
      return copy.deepcopy(self.state)
     def submit(self,deck,op,**arguments):
      self.calls.append((op,arguments))
      if op!=19:raise AssertionError('unrelated SDK family')
      self.state['cues_rev']+=1
      if fault!='still-stored':self.state['entries'].pop()
      if fault=='other-cue':self.state['entries'][0]['start']+=1.
      return {'receipt_ns':0}
     def positions(self,deck):
      self.now+=1
      if fault!='paused':self.position+=.1
      return [(1,self.position if fault=='paused' else self.position-.01),(2,self.position)]
    fixture=Fixture()
    with patch.object(cue,'CueClient',return_value=fixture):
     report=cue.run_hot_clear(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,sample_rate=44100,clock=lambda:fixture.now,sleep=lambda _:None)
    self.assertEqual(report['status'],'PASS' if fault=='none' else 'FAIL' if fault=='other-cue' else 'INCOMPLETE',report)
    self.assertEqual(fixture.calls,[] if fault=='no-fixture' else [(19,dict(flags=3,classification=1,entry_id=8))])
    if fault=='none':self.assertEqual(report['decks']['0']['operations'],[19])

 def test_selected_hot_clear_dispatch_cannot_run_or_claim_all14(self):
  for operations in ([19],list(cue.OPS)):
   result=dict(status='PASS',decks={'0':dict(status='PASS',operations=operations)})
   with patch.object(cue,'run_hot_clear',return_value=result) as clear,patch.object(cue,'run_commands') as full,patch.object(cue,'run_refusals') as refusals,patch.object(cue,'run_memory_color') as memory:
    if operations==[19]:
     report=cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='hot-clear')
     self.assertEqual(report['requested_operations'],[19])
    else:
     with self.assertRaisesRegex(ValueError,'cannot claim complete'):cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='hot-clear')
   clear.assert_called_once();full.assert_not_called();refusals.assert_not_called();memory.assert_not_called()

 def test_selected_hot_set_dispatch_runs_only_its_case_and_coverage(self):
  result=dict(status='PASS',decks={'0':dict(status='PASS',operations=[18])})
  with patch.object(cue,'run_hot_set',return_value=result) as setter,patch.object(cue,'run_hot_cues') as group,patch.object(cue,'run_commands') as full,patch.object(cue,'run_refusals') as refusals:
   report=cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='hot-set')
  setter.assert_called_once();group.assert_not_called();full.assert_not_called();refusals.assert_not_called()
  self.assertEqual(report['requested_operations'],[18])

 def test_hot_group_checks_capture_metadata_seek_start_and_delete(self):
  for fault in ('none','wrong-point','wrong-trigger','still-stored'):
   with self.subTest(fault=fault):
    class Fixture:
     def __init__(self):
      self.state=dict(native_load=7,cues_rev=10,entries=[dict(classification=1,entry_id=1,type=1,valid=5,start=1.,end=0.,rgb=0,comment='original')]);self.calls=[];self.position=10.;self.main=0.;self.playing=True;self.now=0.
     def cues(self,deck,predicate=lambda state:True):
      if not predicate(self.state):raise cue.MissingEvidence('native effect missing')
      return copy.deepcopy(self.state)
     def submit(self,deck,op,**arguments):
      self.calls.append(op)
      if op==23:self.main=self.position
      elif op==24:self.position=self.main;self.playing=False
      elif op==17:
       self.position=next(row['start'] for row in self.state['entries'] if row['entry_id']==arguments['entry_id']);self.playing=True
       if fault=='wrong-trigger' and arguments['entry_id']!=1:self.position+=2.
      elif op==18:self.state['entries'].append(dict(classification=1,entry_id=arguments['entry_id'],type=1,valid=5,start=self.position+(1. if fault=='wrong-point' else 0.),end=0.,rgb=arguments['rgb'],comment=''))
      elif op in (25,26):
       if arguments['revision']!=self.state['cues_rev']:raise AssertionError('stale metadata revision')
       row=next(row for row in self.state['entries'] if row['entry_id']==arguments['entry_id'])
       row['comment' if op==25 else 'rgb']=arguments['comment' if op==25 else 'rgb']
      elif op==19:
       if fault!='still-stored':self.state['entries']=[row for row in self.state['entries'] if row['entry_id']!=arguments['entry_id']]
      elif op!=4:raise AssertionError('unrelated SDK family '+str(op))
      if op in (18,19,25,26):self.state['cues_rev']+=1
      return {'receipt_ns':0}
     def positions(self,deck):
      self.now+=1
      if self.playing:self.position+=.1
      return [(1,self.position-.01 if self.playing else self.position),(2,self.position)]
    fixture=Fixture()
    with patch.object(cue,'CueClient',return_value=fixture):
     report=cue.run_hot_cues(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,sample_rate=44100,clock=lambda:fixture.now,sleep=lambda _:None)
    self.assertEqual(report['status'],'PASS' if fault=='none' else 'FAIL' if fault=='wrong-point' else 'INCOMPLETE',report)
    self.assertTrue(set(fixture.calls)<={4,17,18,19,23,24,25,26})
    if fault=='none':self.assertEqual(report['decks']['0']['operations'],[17,18,19,25,26])

 def test_selected_saved_recall_requires_observed_pause_without_running_writes(self):
  for fault in ('none','moving','wrong-point'):
   with self.subTest(fault=fault):
    class Fixture(ActiveLoopFixture):
     def submit(self,deck,op,**arguments):
      result=super().submit(deck,op,**arguments)
      if op==13 and fault=='moving':self.paused=False
      if op==13 and fault=='wrong-point':self.position+=.25
      return result
    fixture=Fixture();ticks=iter(range(1000))
    with patch.object(cue,'CueClient',return_value=fixture):
     report=cue.run_saved_recall(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,decks=(0,),sample_rate=44100,clock=lambda:next(ticks),sleep=lambda _:None)
    self.assertEqual(report['status'],'PASS' if fault=='none' else 'INCOMPLETE')
    self.assertEqual([op for op,args in fixture.calls],[13])

 def test_selected_saved_loop_checks_stored_bounds_and_restores_original_cues(self):
  fixture=ActiveLoopFixture()
  with patch.object(cue,'CueClient',return_value=fixture):
   report=cue.run_test_phase('selected',None,None,None,200,setup_deck=lambda deck:fixture.submit(deck,17,flags=2,classification=1,entry_id=1),read_positions=fixture.positions,decks=(0,),selection='saved-loop',sample_rate=44100)
  self.assertEqual(report['status'],'PASS',report)
  self.assertEqual(report['requested_operations'],[13,14,15,16])
  self.assertTrue(cue.cue_values_equal(cue.stored_cue_contents(fixture.state['entries']),cue.stored_cue_contents(fixture.original['entries']),44100))

 def test_saved_projection_uses_native_rounding_without_widening_sample_equality(self):
  for rate,working,stored in ((44100,145801,145800),(48000,48004,48003),(96000,96008,96007),(192000,192016,192015)):
   with self.subTest(rate=rate):
    projected=cue.native_saved_position(working/rate,rate)
    self.assertEqual(cue.compare_timestamps(projected,stored/rate,units_per_second=1,sample_rate=rate),0)
    self.assertNotEqual(cue.compare_timestamps(projected,(stored+2)/rate,units_per_second=1,sample_rate=rate),0)
  self.assertEqual(cue.compare_timestamps(cue.native_saved_position(2.8021315192743765,44100),2.8021315192743765,units_per_second=1,sample_rate=44100),0)

 def test_missing_startup_identity_is_incomplete_and_observed_mismatch_fails(self):
  health=[dict(pid='200',service=dict(NRestarts='0',ActiveState='active',SubState='running'),local_monotonic=t) for t in (0.,120.)]
  manifest=dict(sha256={'azmod.so':'a'*64,'azstem.so':'b'*64})
  identity=dict(pid='200',mapped=dict(manifest['sha256']),firmware_sha256='c'*64)
  validate=lambda actual:cue.validate_cue_observation('', '', health, actual, manifest, 'c'*64, dict(status='PASS'))
  missing=validate({})
  self.assertEqual(missing['status'],'INCOMPLETE');self.assertEqual(missing['errors'],[])
  self.assertIn('mapped native process identity not captured',missing['missing'])
  self.assertEqual(validate(identity)['status'],'PASS')
  wrong=validate(dict(identity,mapped={'azmod.so':'d'*64,'azstem.so':'b'*64}))
  self.assertEqual(wrong['status'],'FAIL');self.assertIn('mapped core/diagnostic hash mismatch azmod.so',wrong['errors'])

 def test_cue_collection_matching_does_not_sort_or_greedily_pair_timestamps(self):
  rate=128
  first=[dict(start=.75/rate,end=1.,comment='same'),dict(start=0.,end=1.,comment='same')]
  second=[dict(start=.25/rate,end=1.,comment='same'),dict(start=1./rate,end=1.,comment='same')]
  self.assertTrue(cue.cue_values_equal(first,second,rate))
  self.assertTrue(cue.cue_values_equal(second,first,rate))
  self.assertFalse(cue.cue_values_equal(first,second[:1],rate))
  self.assertFalse(cue.cue_values_equal(first,[second[0],dict(second[1],comment='changed')],rate))

 def test_selected_main_cue_requires_actual_seek_and_paused_sample_window(self):
  for rate in (44100,48000,96000):
   for fault in ('none','wrong-seek','moving','sample-jitter'):
    with self.subTest(rate=rate,fault=fault):
     class Fixture:
      now=0.;stamp=0;position=10.;playing=True;captured=None
      def __init__(self):self.calls=[]
      def submit(self,deck,op,**arguments):
       self.calls.append(op)
       if op==23:self.captured=self.position
       elif op==24:
        self.position=self.captured+(1 if fault=='wrong-seek' else 0);self.playing=fault=='moving'
       else:raise AssertionError('unrelated command')
       return {'receipt_ns':self.stamp}
      def read(self,deck):
       self.now+=.1;self.stamp+=100000000
       if self.playing:
        self.position+=.2;points=(self.position-.1,self.position)
       else:
        jitter=(1.1 if fault=='sample-jitter' else .1)/rate
        points=(self.position-jitter,self.position+jitter)
       return [(self.stamp-100000000,points[0]),(self.stamp,points[1])]
      def sleep(self,seconds):self.now+=seconds
     fixture=Fixture();setup=Mock()
     with patch.object(cue,'CueClient',return_value=fixture):
      report=cue.run_main_cue(None,None,None,200,setup_deck=setup,read_positions=fixture.read,sample_rate=rate,clock=lambda:fixture.now,sleep=fixture.sleep)
     self.assertEqual(report['status'],'PASS' if fault=='none' else 'INCOMPLETE')
     self.assertEqual(fixture.calls,[23,24]);setup.assert_called_once_with(0)
     if fault=='none':self.assertEqual(report['decks']['0']['operations'],[23,24])

 def test_cold_boot_requires_retained_ui_trust_and_a_new_empty_native_boot(self):
  owned=dict(container='az-sdk-automation-200-'+'a'*12,token='a'*32)
  old='11111111-1111-1111-1111-111111111111';new='22222222-2222-2222-2222-222222222222';sha='b'*64
  manifest=dict(sha256={'azmod.so':'c'*64},position_binding={'engine_rate':44100})
  prior=dict(status='PASS',errors=[],missing=[],ui_allow=True,ui_reload=True,reload_request='apply',approved_sha256=sha,artifact_sha256=manifest['sha256'],position_binding=manifest['position_binding'])
  cold=dict(owned,approval=prior,previous_boot_id=old,boot_id=new,stopped_identity=dict(pid='11096',birth='1000'))
  actual=dict(boot_id=new,native_pid='',trust=cue.ID+' '+sha+'\n',normal_probe_sha=sha)
  self.assertEqual(cue.validate_cold_cue_approval(cold,owned,manifest,sha,**actual),cold['stopped_identity'])
  cold_allow=dict(cold,approval=dict(prior,ui_reload=False,reload_request=None,activation='cold_boot',approved_in_boot_id=old))
  self.assertEqual(cue.validate_cold_cue_approval(cold_allow,owned,manifest,sha,**actual),cold['stopped_identity'])
  retained=dict(status='PASS',boot_id=old,pid='11096',birth='1000',mapped_probe_sha256=sha)
  chained=dict(cold_allow,approval=dict(cold_allow['approval'],approved_in_boot_id='33333333-3333-3333-3333-333333333333'),previous_activation=retained)
  self.assertEqual(cue.validate_cold_cue_approval(chained,owned,manifest,sha,**actual),cold['stopped_identity'])
  with self.assertRaises(ValueError):cue.validate_cold_cue_approval(dict(chained,previous_activation=None),owned,manifest,sha,**actual)
  for key,value in (('status','INCOMPLETE'),('boot_id',new),('pid','99'),('birth','99'),('mapped_probe_sha256','d'*64)):
   with self.subTest(retained_activation=key),self.assertRaises(ValueError):
    cue.validate_cold_cue_approval(dict(chained,previous_activation=dict(retained,**{key:value})),owned,manifest,sha,**actual)
  for change in (dict(ui_allow=False),dict(approved_in_boot_id=new),dict(activation='warm_restart'),dict(approved_sha256='d'*64)):
   with self.subTest(cold_allow=change),self.assertRaises(ValueError):
    cue.validate_cold_cue_approval(dict(cold_allow,approval=dict(cold_allow['approval'],**change)),owned,manifest,sha,**actual)
  with self.assertRaises(ValueError):cue.validate_cold_cue_approval(cold_allow,owned,manifest,sha,**dict(actual,trust=''))
  with self.assertRaises(ValueError):cue.validate_cold_cue_approval(cold_allow,owned,manifest,sha,**dict(actual,native_pid='11096'))
  candidate=dict(manifest,sha256={'azmod.so':'e'*64})
  with self.assertRaises(ValueError):cue.validate_cold_cue_approval(cold,owned,candidate,sha,**actual)
  self.assertEqual(cue.validate_cold_cue_approval(dict(cold,updated_core=True),owned,candidate,sha,**actual),cold['stopped_identity'])
  with self.assertRaises(ValueError):cue.validate_cold_cue_approval(dict(cold,updated_core=True),owned,dict(candidate,position_binding={'engine_rate':48000}),sha,**actual)
  with self.assertRaises(ValueError):cue.validate_cold_cue_approval(dict(cold,updated_core=1),owned,candidate,sha,**actual)
  for changes in (dict(boot_id=old),dict(native_pid='11096'),dict(trust=''),dict(normal_probe_sha='d'*64)):
   with self.subTest(changes=changes),self.assertRaises(ValueError):
    cue.validate_cold_cue_approval(cold,owned,manifest,sha,**dict(actual,**changes))
  for key,value in (('token','d'*32),('previous_boot_id',new),('stopped_identity',{})):
   with self.subTest(key=key),self.assertRaises(ValueError):
    cue.validate_cold_cue_approval(dict(cold,**{key:value}),owned,manifest,sha,**actual)
  for key,value in (('status','INCOMPLETE'),('ui_allow',False),('ui_reload',False),('approved_sha256','d'*64),('artifact_sha256',{}),('position_binding',{})):
   with self.subTest(approval_key=key),self.assertRaises(ValueError):
    cue.validate_cold_cue_approval(dict(cold,approval=dict(prior,**{key:value})),owned,manifest,sha,**actual)

 def test_exact_fields_and_hot_ids_all8(self):
  text=base(cue_count=8)+''.join(entry(entry_id=i,slot=i-1) for i in range(1,9));rows,entries=cue.records(text);self.assertEqual([e['entry_id'] for e in entries],list(range(1,9)))
  for i in (0,9):
   with self.assertRaisesRegex(ValueError,'Hot IDs'):cue.records(base(cue_count=1)+entry(entry_id=i))
 def test_complete_malformed_nan_and_comment_utf8_reject(self):
  for text in ('SDK_CUE v=1 broken=yes\n',base(main_s='nan'),base()+entry(comment_hex='ff'),base()+entry(comment_hex='00')):
   with self.assertRaises(ValueError):cue.records(text)
  self.assertEqual(cue.records(base()+entry(comment_hex='e29c93'))[1][0]['comment'],'✓')
 def test_generic_requests_and_flag_contract_all14(self):
  persistent={14,15,16,18,19,21,22,25,26};selected={13,15,16,20,22,25,26}
  for op in cue.OPS:
   args=cue.arguments(op);self.assertEqual(bool(args['flags']&1),op in persistent);self.assertEqual(bool(args['flags']&4),op in selected)
   line=cue.request(200,'submit',1,0,op,7,timeout=200,comment='cue ✓',**args);self.assertIn('comment_hex=63756520e29c93',line);self.assertLessEqual(len(line),1023)
 def test_input_bounds(self):
  for kwargs in (dict(comment='x'*128),dict(comment='a\0b'),dict(rgb=0x1000000),dict(position=math.nan),dict(flags=8),dict(classification=3)):
   with self.assertRaises(ValueError):cue.request(200,'submit',1,op=23,load=7,timeout=100,**kwargs)
  for pid in (True,0,'0200','1;bad'):
   with self.assertRaises(ValueError):cue.request(pid,'snapshot')
 def test_ready_pid_and_data_ownership(self):
  t='SDK_CUE_READY v=1 pid=200 commands=0 data=/real/usb/data\nSDK_CUE_READY v=1 pid=200 commands=12 data=/real/usb/data\n';self.assertEqual(cue.ready(t,'200')['commands'],12)
  with self.assertRaises(ValueError):cue.ready(t,201)
  with self.assertRaises(ValueError):cue.ready(t+'SDK_CUE_READY v=1 pid=200 commands=12 data=/fake\n',200)
 def test_client_requires_actual_sdk_result_and_release(self):
  transcript=[''];counter=[0]
  def send(line):
   values=dict(p.split('=',1) for p in line.split());counter[0]+=1
   if values['action'] in ('snapshot','state'):transcript[0]+=base(seq=counter[0],cue_count=0,query_ns=100);return
   common=dict(id=int(values['id']),op=int(values['op']),target_load=7,flags=2,classification=0,entry_id=0,rc=0,reason=0,revision=3,receipt_ns=200,query_ns=201,timeout_ns=10000000000,submitted_ns=200,deadline_ns=10000000200)
   transcript[0]+=base(seq=counter[0],event='submit',outcome=0,**common);counter[0]+=1
   transcript[0]+=base(seq=counter[0],event='poll',outcome=1,**common);counter[0]+=1
   transcript[0]+=base(seq=counter[0],event='release',outcome=-1,**common)
  c=cue.CueClient(lambda:transcript[0],send,lambda:True,200);r=c.submit(0,23,flags=2);self.assertEqual(r['outcome'],1);self.assertEqual(len(c.evidence),1)
 def test_native_submit_clock_survives_seven_second_transport_and_release_busy(self):
  transcript=[''];counter=[0];reads=[0];submitted=[False];release_added=[False];request_seen=[]
  def send(line):
   values=dict(p.split('=',1) for p in line.split());counter[0]+=1
   if values['action'] in ('snapshot','state'):
    transcript[0]+=base(seq=counter[0],cue_count=0,query_ns=100);return
   request_seen.append(values);submitted[0]=True
   args=dict(id=int(values['id']),op=int(values['op']),target_load=7,flags=2,classification=0,entry_id=0,rc=0,reason=0,revision=3,receipt_ns=15000000000,query_ns=15000000001,timeout_ns=10000000000,submitted_ns=8000000000,deadline_ns=18000000000)
   transcript[0]+=base(seq=counter[0],event='submit',outcome=0,**args);counter[0]+=1;transcript[0]+=base(seq=counter[0],event='poll',outcome=1,**args);counter[0]+=1;transcript[0]+=base(seq=counter[0],event='release',outcome=-1,**dict(args,rc=-5))
  def read():
   reads[0]+=1
   if submitted[0] and reads[0]>=4 and not release_added[0]:
    counter[0]+=1;transcript[0]+=base(seq=counter[0],event='release',id=1,op=23,rc=0);release_added[0]=True
   return transcript[0]
  c=cue.CueClient(read,send,lambda:True,200);r=c.submit(0,23,flags=2)
  self.assertEqual(request_seen[0]['timeout_ns'],'10000000000');self.assertNotIn('deadline_ns',request_seen[0]);self.assertEqual(r['deadline_ns']-r['submitted_ns'],10000000000);self.assertTrue(release_added[0])
  # A terminal receipt never turns a retained, BUSY release into success.
  transcript[0]='';counter[0]=0;submitted[0]=False;clock=[0]
  def tick():clock[0]+=1;return clock[0]
  c=cue.CueClient(lambda:transcript[0],send,lambda:True,200,sleep=lambda _:None,clock=tick)
  with self.assertRaises(cue.MissingEvidence):c.submit(0,23,flags=2)
  self.assertEqual(c.evidence,[])
 def test_late_native_failure_reports_outcome_but_late_applied_is_rejected(self):
  for outcome,reason,expected in ((4,-4,'rc=0 outcome=4 reason=-4'),(2,-4,'rc=0 outcome=2 reason=-4'),(3,-6,'rc=0 outcome=3 reason=-6'),(1,0,'APPLIED receipt outside native request deadline')):
   transcript=[''];counter=[0]
   def send(line):
    value=dict(p.split('=',1) for p in line.split());counter[0]+=1
    if value['action'] in ('snapshot','state'):transcript[0]+=base(seq=counter[0],cue_count=0,query_ns=100);return
    common=dict(id=1,op=23,target_load=7,flags=2,rc=0,reason=0,revision=3,receipt_ns=200,query_ns=201,timeout_ns=10000000000,submitted_ns=200,deadline_ns=10000000200)
    transcript[0]+=base(seq=counter[0],event='submit',outcome=0,**common);counter[0]+=1
    transcript[0]+=base(seq=counter[0],event='poll',outcome=outcome,**dict(common,reason=reason,receipt_ns=10050000200,query_ns=10050000201))
   client=cue.CueClient(lambda:transcript[0],send,lambda:True,200)
   with self.assertRaisesRegex(ValueError,expected):client.submit(0,23,flags=2)
   self.assertEqual(client.evidence,[])
 def test_unsupported_rejection_requires_exact_reason_and_retained_release(self):
  for immediate in (True,False):
   transcript=[''];counter=[0]
   def send(line):
    value=dict(p.split('=',1) for p in line.split());counter[0]+=1
    if value['action'] in ('snapshot','state'):transcript[0]+=base(seq=counter[0],cue_count=0,query_ns=100);return
    common=dict(id=1,op=26,target_load=7,flags=7,classification=2,entry_id=2,rgb=0x2BA6D9,target_revision=9,rc=0,reason=0,revision=0,receipt_ns=200,query_ns=201,timeout_ns=10000000000,submitted_ns=200,deadline_ns=10000000200)
    transcript[0]+=base(seq=counter[0],event='submit',outcome=2 if immediate else 0,**dict(common,rc=-2 if immediate else 0,reason=-2 if immediate else 0));counter[0]+=1
    if not immediate:
     transcript[0]+=base(seq=counter[0],event='poll',outcome=2,**dict(common,reason=-2));counter[0]+=1;transcript[0]+=base(seq=counter[0],event='release',outcome=-1,**common)
   client=cue.CueClient(lambda:transcript[0],send,lambda:True,200)
   result=client.submit(0,26,flags=7,classification=2,entry_id=2,revision=9,rgb=0x2BA6D9,expect_unsupported=True)
   self.assertEqual((result['outcome'],result['reason']),(2,-2));self.assertEqual(client.evidence,[])
 def test_memory_color_rejects_unsupported_without_mutation_and_preserves_utf8(self):
  row=dict(snapshot_seq=1,classification=2,entry_id=2,type=1,valid=5,start=1.25,end=0.,rgb=0,comment='emulator cue ✓')
  class Client:
   def __init__(self,mutate=False,erase=False,change=False):self.state=dict(native_load=7,cues_rev=10,entries=[dict(row)]);self.calls=[];self.mutate=mutate;self.erase=erase;self.change=change
   def submit(self,deck,op,**arguments):
    self.calls.append(arguments)
    if arguments.get('expect_unsupported'):
     if self.mutate:self.state['cues_rev']+=1
     if self.change:self.state['entries'][0]['end']=2.5
    else:
     self.state['cues_rev']+=1;self.state['entries'][0]['rgb']=arguments['rgb']
     if self.erase:self.state['entries'][0]['comment']=''
   def cues(self,deck,predicate=lambda state:True):
    if not predicate(self.state):raise cue.MissingEvidence('Memory color readback')
    self.state['entries'][0]['snapshot_seq']+=1
    return dict(self.state,entries=[dict(e) for e in self.state['entries']])
  client=Client();state,after=cue.memory_color_roundtrip(client,0,dict(client.state,entries=[dict(row)]),dict(row),sample_rate=44100);self.assertEqual(after['rgb'],0x00C3FF);self.assertEqual(after['comment'],row['comment']);self.assertTrue(client.calls[0]['expect_unsupported']);self.assertEqual(client.calls[1]['revision'],10)
  with self.assertRaisesRegex(ValueError,'mutated'):cue.memory_color_roundtrip(Client(mutate=True),0,dict(native_load=7,cues_rev=10,entries=[dict(row)]),dict(row),sample_rate=44100)
  with self.assertRaisesRegex(ValueError,'mutated'):cue.memory_color_roundtrip(Client(change=True),0,dict(native_load=7,cues_rev=10,entries=[dict(row)]),dict(row),sample_rate=44100)
  with self.assertRaisesRegex(ValueError,'preserve'):cue.memory_color_roundtrip(Client(erase=True),0,dict(native_load=7,cues_rev=10,entries=[dict(row)]),dict(row),sample_rate=44100)
 def test_memory_persistence_checks_complete_contents_with_revision_scoped_ids(self):
  entry=dict(v=1,snapshot_seq=1,deck=0,load=7,revision=9,classification=2,entry_id=2,slot=0,type=1,valid=61,start=1.25,end=0.,rgb=0x00C3FF,color_id=0,saved=1,modified=0,auto_active=0,comment='cue café ✓ 🎵')
  expected=cue.cue_contents([entry])[0]
  reloaded=dict(entry,snapshot_seq=22,load=8,revision=12,entry_id=4)
  self.assertEqual(cue.require_memory_contents(dict(entries=[reloaded]),expected,sample_rate=44100),reloaded)
  for change in (dict(rgb=0),dict(comment=''),dict(start=1.5),dict(type=2),dict(auto_active=1),dict(saved=0),dict(valid=1)):
   with self.subTest(change=change),self.assertRaisesRegex(ValueError,'persisted Memory cue contents'):
    cue.require_memory_contents(dict(entries=[dict(reloaded,**change)]),expected,sample_rate=44100)
  for entries in ([],[reloaded,dict(reloaded,entry_id=5)]):
   with self.assertRaisesRegex(ValueError,'persisted Memory cue contents'):cue.require_memory_contents(dict(entries=entries),expected,sample_rate=44100)
 def test_missing_saved_cue_after_fresh_reload_is_behavior_failure(self):
  point=dict(classification=2,entry_id=2,type=1,valid=61,start=1.25,end=0.,rgb=0x00C3FF,comment='')
  commented=dict(point,comment='emulator cue café ✓ 🎵')
  snapshots=[dict(native_load=7,cues_rev=1,entries=[]),dict(native_load=7,cues_rev=2,entries=[point]),dict(native_load=7,cues_rev=3,entries=[commented]),dict(native_load=8,cues_rev=4,entries=[])]
  class Client:
   def submit(self,*args,**kwargs):pass
   def cues(self,deck,predicate=lambda state:True):
    value=snapshots.pop(0)
    if not predicate(value):raise cue.MissingEvidence('snapshot predicate did not match')
    return value
  with patch.object(cue,'CueClient',return_value=Client()),patch.object(cue,'memory_color_roundtrip',side_effect=lambda client,deck,snapshot,point,**kwargs:(snapshot,point)):
   report=cue.run_memory_color(None,None,None,200,setup_deck=lambda deck:None,reload_deck=lambda deck:None,sample_rate=44100)
  self.assertEqual(report['status'],'FAIL');self.assertIn('persisted Memory cue contents',report['errors'][0]);self.assertEqual(snapshots,[])
 def test_pending_poll_cannot_move_the_original_native_deadline(self):
  transcript=[''];counter=[0]
  def send(line):
   value=dict(p.split('=',1) for p in line.split());counter[0]+=1
   if value['action'] in ('snapshot','state'):transcript[0]+=base(seq=counter[0],cue_count=0,query_ns=100);return
   common=dict(id=1,op=23,target_load=7,flags=2,rc=0,reason=0,revision=3,receipt_ns=200,query_ns=201,timeout_ns=10000000000,submitted_ns=200,deadline_ns=10000000200)
   transcript[0]+=base(seq=counter[0],event='submit',outcome=0,**common);counter[0]+=1
   transcript[0]+=base(seq=counter[0],event='poll',outcome=1,**dict(common,submitted_ns=210,deadline_ns=10000000210))
  c=cue.CueClient(lambda:transcript[0],send,lambda:True,200)
  with self.assertRaisesRegex(ValueError,'deadline changed'):c.submit(0,23,flags=2)
 def test_source_uses_real_qualifier_string_pid(self):
  binding=dict(provider_build_id='b'*40,header_sha256='c'*64,engine_rate=44100,abi_size=296);samples=[]
  for i in range(1,4):samples.append(dict(sequence=i,native_monotonic_ns=i*100000000,decks=[dict(deck=d,loaded=1,read_rc=0,title='track',playhead=i*4410,pos_s=i*.1) for d in range(4)]))
  packet=dict(binding,schema=1,pid=200,status='ok',sequence=3,native_monotonic_ns=300000000,native_boottime_ns=300000000,samples=samples)
  self.assertEqual(len(cue.source_rows(dict(positions=packet,boottime_ns=300000000),0,200,binding,['track'])),3)
  with self.assertRaises(cue.StaleCueSource):cue.source_rows(dict(positions=packet,boottime_ns=1408000000),0,200,binding,['track'])
  with self.assertRaisesRegex(ValueError,'binding mismatch'):cue.source_rows(dict(positions=dict(packet,provider_build_id='d'*40),boottime_ns=1408000000),0,200,binding,['track'])
  with self.assertRaises(ValueError):cue.source_rows(dict(positions=packet,boottime_ns=300000000),0,201,binding,['track'])
 def test_stale_source_retry_is_bounded_and_never_retries_binding_errors(self):
  count=[0];clock=[0]
  def tick():clock[0]+=1;return clock[0]
  def reader(deck):
   count[0]+=1
   if count[0]==1:raise cue.StaleCueSource('stale')
   return [(10,1.5)]
  self.assertEqual(cue.fresh_cue_positions(reader,0,sleep=lambda _:None,clock=tick),[(10,1.5)]);self.assertEqual(count[0],2)
  clock[0]=0
  def stale(deck):raise cue.StaleCueSource('still stale')
  with self.assertRaises(cue.StaleCueSource):cue.fresh_cue_positions(stale,0,sleep=lambda _:None,clock=tick)
  self.assertLessEqual(clock[0],6)
  count[0]=0
  def mismatch(deck):count[0]+=1;raise ValueError('position binding mismatch')
  with self.assertRaisesRegex(ValueError,'binding mismatch'):cue.fresh_cue_positions(mismatch,0,sleep=lambda _:None,clock=tick)
  self.assertEqual(count[0],1)
 def test_framework_files_are_temporary_on_success_and_failure(self):
  for fail in (False,True):
   with tempfile.TemporaryDirectory() as root:
    captured=[]
    def operation(*args):
     path=Path(args[3]);captured.append(path);path.mkdir();(path/'internal.txt').write_text('temporary')
     if fail:raise RuntimeError('fixture failure')
     return {'status':'PASS'}
    with patch.object(cue,'_run_cue_runtime',side_effect=operation):
     if fail:
      with self.assertRaisesRegex(RuntimeError,'fixture failure'):cue.run_cue_runtime(None,None,None,Path(root)/'out',None,None,None,None)
     else:self.assertEqual(cue.run_cue_runtime(None,None,None,Path(root)/'out',None,None,None,None)['status'],'PASS')
    self.assertFalse(captured[0].parent.exists());self.assertEqual(list(Path(root).iterdir()),[])
 def test_positive_phase_never_fakes_admission_pass(self):
  with patch.object(cue,'run_refusals') as negative,patch.object(cue,'run_commands',return_value=dict(status='PASS')) as positive:
   r=cue.run_test_phase('positive',None,None,None,200,setup_deck=None,read_positions=None)
  negative.assert_not_called();positive.assert_called_once();self.assertEqual(r['admission_refusals']['status'],'NOT_RUN')
 def test_full_phase_refusal_failure_halts_positive(self):
  with patch.object(cue,'run_refusals',return_value=dict(status='FAIL',errors=['bad SDK rejection'])) as negative,patch.object(cue,'run_commands') as positive:
   r=cue.run_test_phase('full',None,None,None,200,setup_deck=None,read_positions=None)
  negative.assert_called_once();positive.assert_not_called();self.assertEqual(r['status'],'FAIL')
  with self.assertRaises(ValueError):cue.run_test_phase('bad',None,None,None,200,setup_deck=None,read_positions=None)
 def test_selected_case_never_runs_other_behavioral_families_or_claims_all14(self):
  result=dict(status='PASS',decks={'0':dict(status='PASS',operations=[16])})
  with patch.object(cue,'run_active_loop_switch',return_value=result) as active,patch.object(cue,'run_commands') as full,patch.object(cue,'run_refusals') as refusals,patch.object(cue,'run_memory_color') as memory:
   report=cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='active-loop-switch')
  active.assert_called_once();full.assert_not_called();refusals.assert_not_called();memory.assert_not_called()
  self.assertEqual(report['requested_operations'],[16]);self.assertEqual(report['coverage'],'selected case only');self.assertNotIn('admission_refusals',report)
  with patch.object(cue,'run_active_loop_switch',return_value=dict(status='PASS',decks={'0':dict(operations=list(cue.OPS))})):
   with self.assertRaisesRegex(ValueError,'cannot claim complete'):cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='active-loop-switch')
 def test_malformed_selection_refuses_before_ssh_or_panel_input(self):
  for phase,selection in (('selected',None),('selected','unknown'),('selected',['active-loop-switch']),('full','active-loop-switch'),('bad',None)):
   with self.subTest(phase=phase,selection=selection),patch.object(cue,'run_observed_runtime') as observed:
    with self.assertRaises(ValueError):cue._run_cue_runtime(None,None,None,None,None,None,None,dict(test_phase=phase,selection=selection))
    observed.assert_not_called()
 def test_selected_missing_deck_or_native_effect_cannot_pass(self):
  for result in (dict(status='PASS',decks={}),dict(status='INCOMPLETE',missing=['native effect missing'],decks={}),dict(status='FAIL',errors=['partial write'],decks={})):
   with self.subTest(result=result),patch.object(cue,'run_active_loop_switch',return_value=result):
    report=cue.run_test_phase('selected',None,None,None,200,setup_deck=None,read_positions=None,selection='active-loop-switch')
   self.assertNotEqual(report['status'],'PASS')
  with patch.object(cue,'run_refusals',return_value=dict(status='PASS')),patch.object(cue,'run_commands',return_value=dict(status='INCOMPLETE',missing=['all14 native cue operations required'])) as full:
   report=cue.run_test_phase('full',None,None,None,200,setup_deck=None,read_positions=None)
  full.assert_called_once();self.assertEqual(report['status'],'INCOMPLETE');self.assertNotIn('selection',report)
 def test_active_loop_reuses_fixtures_and_restores_original_auto_by_fresh_ids(self):
  fixture=ActiveLoopFixture(active=True)
  with patch.object(cue,'CueClient',return_value=fixture):
   report=cue.run_active_loop_switch(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,sample_rate=44100)
  self.assertEqual(report['status'],'PASS');self.assertEqual([op for op,args in fixture.calls],[16,16,16,16]);self.assertEqual(report['decks']['0']['operations'],[16])
  self.assertEqual(cue.stored_cue_contents(fixture.state['entries']),cue.stored_cue_contents(fixture.original['entries']))
 def test_active_loop_creates_only_missing_fixtures_then_removes_them(self):
  fixture=ActiveLoopFixture(create=True)
  with patch.object(cue,'CueClient',return_value=fixture):
   report=cue.run_active_loop_switch(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,sleep=lambda _:None,sample_rate=44100)
  self.assertEqual(report['status'],'PASS');operations=[op for op,args in fixture.calls]
  self.assertEqual(operations.count(14),2);self.assertEqual(operations.count(15),2);self.assertEqual(operations.count(16),3);self.assertTrue(set(operations)<={1,2,4,14,15,16,17})
  self.assertEqual(cue.stored_cue_contents(fixture.state['entries']),cue.stored_cue_contents(fixture.original['entries']))
 def test_active_loop_partial_switch_fails_and_restores_fixture(self):
  fixture=ActiveLoopFixture(partial=True)
  with patch.object(cue,'CueClient',return_value=fixture):
   report=cue.run_active_loop_switch(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,sample_rate=44100)
  self.assertEqual(report['status'],'FAIL');self.assertEqual(report['failure_stage'],'switch');self.assertIn('expected sole active',report['errors'][0]);self.assertEqual(report['cleanup']['0']['status'],'PASS')
  self.assertEqual(cue.stored_cue_contents(fixture.state['entries']),cue.stored_cue_contents(fixture.original['entries']))
 def test_active_loop_missing_effect_or_invalid_baseline_never_passes(self):
  for invalid in (False,True):
   fixture=ActiveLoopFixture(missing=not invalid)
   if invalid:
    for row in fixture.state['entries']:
     if row['classification']==2 and row['type']==2:row['auto_active']=1
   with patch.object(cue,'CueClient',return_value=fixture):
    report=cue.run_active_loop_switch(None,None,None,200,setup_deck=lambda deck:None,read_positions=fixture.positions,sample_rate=44100)
   self.assertEqual(report['status'],'INCOMPLETE')
   if invalid:self.assertEqual(fixture.calls,[]);self.assertEqual(report['errors'],[])
 def test_cue_finalization_requires_neutral_close_and_health(self):
  events=[];controller=type('Controller',(),{'panel_inputs_neutral':lambda self:True})();sessions=type('Sessions',(),{'close':lambda self:events.append('close')})()
  result=cue.finalize_cue_session(controller,sessions,lambda:events.append('health') or True)
  self.assertEqual(events,['close','health']);self.assertTrue(result['panel_closed'])
  controller.panel_inputs_neutral=lambda:False
  with self.assertRaises(cue.MissingEvidence):cue.finalize_cue_session(controller,sessions,lambda:True)
  self.assertEqual(events,['close','health'])
  controller.panel_inputs_neutral=lambda:True
  sessions.close=lambda:(_ for _ in ()).throw(RuntimeError('close failed'))
  with self.assertRaisesRegex(RuntimeError,'close failed'):cue.finalize_cue_session(controller,sessions,lambda:True)
  sessions.close=lambda:None
  with self.assertRaises(cue.MissingEvidence):cue.finalize_cue_session(controller,sessions,lambda:False)
 def test_reload_reopens_browser_and_renews_after_slow_selection_before_load(self):
  class Controller:
   def __init__(self):
    self.page='waveform';self.title=None;self.generation=1;self.neutral=True;self.live=True;self.loads=0;self.driver=self;self.BUS_MIXER=2
   def panel_inputs_neutral(self):return self.neutral
   def panel(self,name,hold):
    self.assert_live();self.neutral=False
    if name=='browse':self.page='browse'
   def assert_live(self):
    if not self.live:raise RuntimeError('expired receiver')
   def _release(self,bus):self.assert_live();self.neutral=True
   def load(self,deck):
    self.assert_live();self.loads+=1
    if self.page=='browse' and self.title=='track':self.generation+=1
  controller=Controller();controller.load(1);self.assertEqual(controller.generation,1)
  class Sessions:
   def __init__(self):self.phases=[]
   def renew(self,label,neutral):
    if not neutral:raise ValueError('neutral boundary required')
    self.phases.append(label);controller.live=True
  sessions=Sessions();controller.live=False
  def select(current,logical):
   self.assertEqual((current.page,logical),('browse',1));current.title='track';current.live=False
   return dict(status='PASS',selected_title='track',evidence='/observed-selection.ppm')
  cue.reload_selected_cue_track(controller,sessions,select,0,['track'])
  self.assertEqual(controller.generation,2);self.assertEqual(sessions.phases,['browse-reload-deck1','reload-deck1']);self.assertTrue(controller.neutral)
  before=controller.loads
  with self.assertRaisesRegex(cue.MissingEvidence,'reload selection'):
   cue.reload_selected_cue_track(controller,sessions,lambda *args:dict(status='PASS',selected_title='wrong',evidence='/wrong.ppm'),0,['track'])
  self.assertEqual(controller.loads,before)
 def test_restart_requires_empty_native_units_and_invalidated_sdk_load(self):
  class Controller:
   def __init__(self,loaded=False):self.loaded=loaded
   def wait(self,predicate,label):
    value=dict(mask='LEEE' if self.loaded else 'EEEE',decks=[dict(loaded=self.loaded)]+[dict(loaded=False) for _ in range(3)])
    if not predicate(value):raise cue.MissingEvidence(label)
    return value
  class Client:
   def __init__(self,valid=False):self.valid=valid;self.calls=[]
   def snapshot(self,deck,entries):self.calls.append((deck,entries));return dict(load_valid=self.valid)
  client=Client();self.assertTrue(cue.require_empty_cue_restart(Controller(),client,(0,))['decks_unloaded']);self.assertEqual(client.calls,[(0,False)])
  with self.assertRaisesRegex(cue.MissingEvidence,'all native decks unloaded'):cue.require_empty_cue_restart(Controller(True),Client(),(0,))
  with self.assertRaisesRegex(cue.MissingEvidence,'SDK load still valid'):cue.require_empty_cue_restart(Controller(),Client(True),(0,))
 def test_empty_hot_overlay_fixture_requires_fresh_clear(self):
  full=dict(native_load=7,cues_rev=4,entries=[dict(classification=1,entry_id=i,valid=1) for i in (7,8)])
  empty=dict(native_load=7,cues_rev=5,entries=[dict(classification=1,entry_id=7,valid=1),dict(classification=1,entry_id=8,valid=0)])
  class Client:
   def __init__(self,last):self.calls=[];self.last=last;self.index=0
   def cues(self,deck,predicate=None):self.index+=1;return full if self.index==1 else self.last
   def submit(self,*a,**kw):self.calls.append((a,kw))
  client=Client(empty);self.assertEqual(cue.empty_hot_fixture(client,0),8);self.assertEqual(client.calls,[((0,19),dict(flags=3,classification=1,entry_id=8))])
  for bad in (full,dict(empty,cues_rev=4),dict(empty,native_load=8)):
   with self.assertRaises(cue.MissingEvidence):cue.empty_hot_fixture(Client(bad),0)
  client=Client(empty);client.cues=lambda *a,**kw:empty;self.assertEqual(cue.empty_hot_fixture(client,0),8);self.assertEqual(client.calls,[])
 def test_hot_target_uses_early_qualified_history_not_latest_only(self):
  self.assertTrue(cue.cue_target_seen([(100,1.87),(110,1.89),(120,1.94)],1.407,sample_rate=44100))
  self.assertFalse(cue.cue_target_seen([(100,1.95),(120,2.1)],1.407,sample_rate=44100))
  self.assertFalse(cue.cue_target_seen([],1.407,sample_rate=44100))
  self.assertFalse(cue.cue_target_seen([(99,1.5),(101,2.1)],1.407,100,sample_rate=44100))
 def test_cue_timestamps_use_actual_sample_window_in_both_directions(self):
  for rate in (44100,48000,96000):
   original=dict(start=1.,end=2.,rgb=0x00C3FF,comment='kept')
   equivalent=dict(original,start=1.+.5/rate,end=2.-.5/rate)
   self.assertTrue(cue.cue_values_equal(original,equivalent,rate))
   self.assertTrue(cue.cue_values_equal(equivalent,original,rate))
   self.assertFalse(cue.cue_values_equal(original,dict(original,start=1.+1.5/rate),rate))
 def test_missing_source_rate_blocks_before_cue_input(self):
  setup=Mock()
  with self.assertRaisesRegex(cue.MissingEvidence,'sample rate'):
   cue.run_active_loop_switch(None,None,None,200,setup_deck=setup,read_positions=None)
  setup.assert_not_called()
 def test_hot_comment_and_color_use_fresh_revision_and_preserve_source_point(self):
  row=dict(classification=1,entry_id=8,type=1,valid=5,start=1.25,end=0.,rgb=0x2BA6D9,comment='')
  class Client:
   def __init__(self,shift=False):self.state=dict(native_load=7,cues_rev=10,entries=[dict(row)]);self.calls=[];self.shift=shift
   def submit(self,deck,op,**arguments):
    self.calls.append((deck,op,arguments));self.state['cues_rev']+=1;current=self.state['entries'][0]
    if op==25:current['comment']=arguments['comment']
    if op==26:current['rgb']=arguments['rgb']
    if self.shift:current['start']+=.001
   def cues(self,deck,predicate):
    if not predicate(self.state):raise cue.MissingEvidence('readback missing')
    return self.state
  client=Client();state,after=cue.hot_metadata_roundtrip(client,0,dict(client.state,entries=[dict(row)]),dict(row),sample_rate=44100)
  self.assertEqual([call[1] for call in client.calls],[25,26]);self.assertEqual([call[2]['revision'] for call in client.calls],[10,11]);self.assertTrue(all(call[2]['flags']==7 and call[2]['classification']==1 and call[2]['entry_id']==8 for call in client.calls));self.assertEqual((after['start'],after['comment'],after['rgb']),(1.25,'emulator hot ✓',0xD9842B))
  client=Client(shift=True)
  with self.assertRaisesRegex(ValueError,'source point'):cue.hot_metadata_roundtrip(client,0,dict(client.state,entries=[dict(row)]),dict(row),sample_rate=44100)
 def test_saved_auto_is_sole_active_and_restores_original_by_fresh_revision(self):
  original=dict(classification=2,type=2,start=2.,end=3.,auto_active=1,entry_id=4)
  created=dict(classification=2,type=2,start=4.,end=5.,auto_active=1,entry_id=5)
  before=dict(cues_rev=10,entries=[dict(original)])
  baseline=cue.saved_auto_baseline(before,sample_rate=44100)
  self.assertFalse(cue.saved_auto_matches(dict(entries=[original,created]),(4.,5.),sample_rate=44100))
  self.assertTrue(cue.saved_auto_matches(dict(entries=[dict(original,auto_active=0),created]),(4.,5.),sample_rate=44100))
  after=dict(cues_rev=12,entries=[dict(original,entry_id=3,auto_active=0)])
  class Client:
   def __init__(self):self.calls=[]
   def submit(self,*args,**kw):self.calls.append((args,kw))
   def cues(self,deck,predicate):
    state=dict(cues_rev=13,entries=[dict(original,entry_id=3)])
    if not predicate(state):raise cue.MissingEvidence('auto restore readback')
    return state
  client=Client();restored=cue.restore_saved_auto(client,0,after,baseline,sample_rate=44100)
  self.assertEqual(client.calls,[((0,16),dict(flags=7,classification=2,entry_id=3,revision=12,position=1))]);self.assertEqual(cue.saved_auto_baseline(restored,sample_rate=44100),baseline)
  client=Client();cue.restore_saved_auto(client,0,before,baseline,sample_rate=44100);self.assertEqual(client.calls,[])
  with self.assertRaisesRegex(ValueError,'fixture changed'):cue.restore_saved_auto(client,0,dict(cues_rev=12,entries=[]),baseline,sample_rate=44100)
  with self.assertRaises(cue.MissingEvidence):cue.saved_auto_baseline(dict(entries=[original,created]),sample_rate=44100)
 def test_all_panel_setup_and_neutral_close_precede_sdk_only_deck_checks(self):
  events=[]
  ready=cue.prepare_cue_decks_before_sdk((0,1),lambda deck:events.append(('load-play-motion',deck)),lambda:events.append('neutral-close'),lambda deck:events.append(('bound-source',deck)))
  ready(0);events.append('sdk-deck0');ready(1);events.append('sdk-deck1')
  self.assertEqual(events,[('load-play-motion',0),('load-play-motion',1),'neutral-close',('bound-source',0),'sdk-deck0',('bound-source',1),'sdk-deck1'])
  with self.assertRaises(ValueError):ready(2)
  events=[]
  def fail_close():raise RuntimeError('neutral close failed')
  ready=cue.prepare_cue_decks_before_sdk((0,),lambda deck:events.append('setup'),fail_close,lambda deck:events.append('sdk-source'))
  with self.assertRaisesRegex(RuntimeError,'neutral close failed'):ready(0)
  self.assertEqual(events,['setup'])
 def test_missing_real_hot_fixture_halts_before_mutation(self):
  fake=type('Fake',(),{'cues':lambda self,*a,**k:dict(entries=[],native_load=7)})()
  with patch.object(cue,'CueClient',return_value=fake):r=cue.run_commands(None,None,None,200,setup_deck=lambda d:None,read_positions=lambda d:[],sample_rate=44100)
  self.assertEqual(r['status'],'INCOMPLETE');self.assertIn('fixture',r['missing'][0])
class LoadedSessionTests(unittest.TestCase):
 def test_ordinary_memory_color_cannot_trigger_a_reload_callback(self):
  for phase,selection in (('memory-color',None),('selected','memory-color')):
   reload=Mock(side_effect=AssertionError('unexpected GUI reload'))
   with patch.object(cue,'run_memory_color',return_value=dict(status='PASS',decks={'0':dict(status='PASS')})) as run:
    result=cue.run_test_phase(phase,None,None,None,'123',setup_deck=None,read_positions=None,selection=selection,reload_deck=reload,sample_rate=44100)
   self.assertEqual(result['status'],'PASS');self.assertIsNone(run.call_args[1]['reload_deck']);reload.assert_not_called()

 def test_reload_persistence_is_explicit_and_requires_a_real_callback(self):
  for phase,selection in (('memory-color-reload',None),('selected','memory-color-reload')):
   with self.assertRaisesRegex(ValueError,'reload callback'):
    cue.run_test_phase(phase,None,None,None,'123',setup_deck=None,read_positions=None,selection=selection)
   reload=Mock()
   with patch.object(cue,'run_memory_color',return_value=dict(status='PASS',decks={'0':dict(status='PASS')})) as run:
    cue.run_test_phase(phase,None,None,None,'123',setup_deck=None,read_positions=None,selection=selection,reload_deck=reload,sample_rate=44100)
   self.assertIs(run.call_args[1]['reload_deck'],reload)

 def memory_fixture(self):
  class Fixture:
   def __init__(self):
    self.load=7;self.revision=10;self.calls=[];self.reloads=0
    self.entries=[dict(classification=1,entry_id=1,slot=0,type=1,valid=61,start=1.,end=0.,rgb=0x00C3FF,color_id=0,comment='original hot',saved=1,modified=0,auto_active=0),dict(classification=2,entry_id=2,slot=0,type=1,valid=61,start=2.,end=0.,rgb=0,color_id=0,comment='original memory',saved=1,modified=0,auto_active=0)]
   def cues(self,deck,predicate=lambda state:True):
    value=dict(native_load=self.load,cues_rev=self.revision,entries=copy.deepcopy(self.entries))
    if not predicate(value):raise cue.MissingEvidence('fixture observation unavailable')
    return value
   def renumber(self):
    self.revision+=1
    for index,row in enumerate(self.entries):
     if row['classification']==2:row['entry_id']=self.revision*10+index
   def submit(self,deck,op,**args):
    self.calls.append(op)
    if op in (23,24):return
    if op==21:
     self.entries.append(dict(self.entries[1],entry_id=99,start=3.,comment=''));self.renumber();return
    if op not in (22,25,26):raise AssertionError('unrelated command '+str(op))
    if args['revision']!=self.revision:raise AssertionError('stale fixture revision')
    row=next(row for row in self.entries if row['classification']==2 and row['entry_id']==args['entry_id'])
    if args.get('expect_unsupported'):return
    if op==22:self.entries.remove(row)
    elif op==25:row['comment']=args['comment']
    else:row.update(rgb=args['rgb'],color_id=6)
    if op==22:self.renumber()
    else:self.revision+=1
   def reload(self,deck):self.load+=1;self.reloads+=1;self.renumber()
  return Fixture()

 def test_memory_color_reuses_load_and_restores_original_cues_by_fresh_ids(self):
  for persistence in (False,True):
   with self.subTest(persistence=persistence):
    fixture=self.memory_fixture();original=cue.stored_cue_contents(fixture.entries)
    setup=Mock()
    with patch.object(cue,'CueClient',return_value=fixture):
     result=cue.run_memory_color(None,None,None,'123',setup_deck=setup,reload_deck=fixture.reload if persistence else None,sample_rate=44100)
    self.assertEqual(result['status'],'PASS',result);setup.assert_called_once_with(0)
    self.assertEqual(fixture.reloads,int(persistence));self.assertEqual(fixture.load,8 if persistence else 7)
    self.assertEqual(result['reload_persistence'],'PASS' if persistence else 'NOT_RUN')
    self.assertTrue(result['decks']['0']['fixture_restored'])
    self.assertTrue(cue.cue_values_equal(cue.stored_cue_contents(fixture.entries),original,44100))
    self.assertEqual(fixture.calls,[23,24,21,25,26,26,22])

 def identity(self,deck):return dict(pid='123',birth='456',native_load=deck+7,load_unit=deck+11,title='same test track',sample_rate=44100)
 def test_memory_color_refuses_a_load_change_before_cleanup(self):
  fixture=self.memory_fixture();submit=fixture.submit
  def change_load(deck,operation,**arguments):
   submit(deck,operation,**arguments)
   if operation==21:fixture.load+=1
  fixture.submit=change_load
  with patch.object(cue,'CueClient',return_value=fixture):
   result=cue.run_memory_color(None,None,None,'123',setup_deck=lambda deck:None,sample_rate=44100)
  self.assertEqual(result['status'],'FAIL',result);self.assertIn('changed the loaded track',result['errors'][0]);self.assertNotIn(22,fixture.calls)

 def group(self,deck,operations):return dict(status='PASS',errors=[],missing=[],decks={str(deck):dict(status='PASS',operations=list(operations))})

 def test_all26_compose_existing_checks_on_the_same_loaded_track(self):
  calls=[]
  def loops(deck):calls.append(('loops',deck));return self.group(deck,range(1,13))
  def cues(deck):calls.append(('cues',deck));return self.group(deck,range(13,27))
  result=cue.run_loaded_command_groups(loops,cues,self.identity,decks=(0,1))
  self.assertEqual(result['status'],'PASS',result)
  self.assertEqual(calls,[('loops',0),('cues',0),('loops',1),('cues',1)])
  for deck in (0,1):self.assertEqual(result['decks'][str(deck)]['operations'],list(range(1,27)))

 def test_failure_or_partial_group_stops_before_later_commands(self):
  for status,operations in (('FAIL',list(range(1,13))),('INCOMPLETE',[]),('PASS',list(range(1,12)))):
   loops=Mock(return_value=dict(self.group(0,operations),status=status));cues=Mock()
   result=cue.run_loaded_command_groups(loops,cues,self.identity,decks=(0,1))
   self.assertNotEqual(result['status'],'PASS');loops.assert_called_once_with(0);cues.assert_not_called()

 def test_changed_track_process_rate_or_load_stops_without_gui_recovery(self):
  for field,value in (('pid','999'),('birth','999'),('title','different track'),('sample_rate',48000),('native_load',8),('load_unit',22)):
   identity=self.identity(0)
   def loops(deck):identity[field]=value;return self.group(deck,range(1,13))
   cues=Mock()
   result=cue.run_loaded_command_groups(loops,cues,lambda deck:dict(identity))
   self.assertEqual(result['status'],'FAIL',result);self.assertIn('changed during loops',result['errors'][0]);cues.assert_not_called()

 def test_missing_fixture_prevents_every_command_group(self):
  loops=Mock();cues=Mock()
  result=cue.run_loaded_command_groups(loops,cues,lambda deck:dict(self.identity(deck),native_load=0))
  self.assertEqual(result['status'],'INCOMPLETE');loops.assert_not_called();cues.assert_not_called()

 def test_existing_case_must_exit_successfully_and_supply_a_passing_report(self):
  import json
  with tempfile.TemporaryDirectory() as directory:
   path=Path(directory)/'report.json';path.write_text(json.dumps(dict(status='PASS',operations=[1])))
   code='print('+repr('CURRENT_NATIVE_RESULT '+str(path))+')'
   self.assertEqual(cue.run_logged_case([sys.executable,'-c',code])['operations'],[1])
   with self.assertRaisesRegex(RuntimeError,'case failed'):cue.run_logged_case([sys.executable,'-c',code+'; raise SystemExit(1)'])
   path.write_text(json.dumps(dict(status='FAIL',errors=['native effect missing'])))
   with self.assertRaisesRegex(ValueError,'did not pass'):cue.run_logged_case([sys.executable,'-c',code])

if __name__=='__main__':unittest.main()
