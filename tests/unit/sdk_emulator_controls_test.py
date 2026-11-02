#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/integration'))
SPEC=importlib.util.spec_from_file_location('controls',str(ROOT/'tests/integration/sdk_emulator_controls.py'))
controls=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(controls)
DRIVER=Path(os.environ.get('AZSDK_TEST_PANEL_DRIVER','/private/tmp/az-sdk-emulator/tools/az-drive.py'))

# Portable frame-builder interface fixture. When the emulator checkout exists,
# the same tests use its exact, digest-bound az-drive.py implementation instead.
PORTABLE='''import azemu
import time
ERP_IDLE=[0]*152
BUS_ERP=0
BUS_MIXER=2
LOAD_MASK={1:8,2:4,3:2,4:1}
def _mixer(off=None,mask=0,pressed=False):
 frame=[0]*152
 if pressed and off is not None:frame[off]=mask
 return frame
def press(side,name,hold=.1):
 offset={1:0,2:8}[side]
 bits={'play':(10,2),'sync':(8,2),'slip':(8,4),'loopin':(8,64),'loopout':(8,32),'reloop':(9,8),'mastertempo':(8,8),
       'decksel1':(8,1),'decksel2':(8,1),'decksel3':(9,1),'decksel4':(9,1),'cue':(10,4)}
 byte,mask=(10,1) if name=='quantize' else bits[name]
 if name!='quantize':byte+=offset
 frame=ERP_IDLE[:];frame[byte]|=mask
 send(BUS_ERP,frame);time.sleep(hold);send(BUS_ERP,ERP_IDLE)
def load(deck,hold=.1):
 frame=_mixer();frame[33]={1:8,2:4,3:2,4:1}[deck]
 send(BUS_MIXER,frame);time.sleep(hold);send(BUS_MIXER,_mixer())
'''

class Clock:
    def __init__(self):self.value=100.0
    def now(self):return self.value
    def sleep(self,n):self.value+=n

POSITION_BINDING={'provider_build_id':'b'*40,'header_sha256':'a'*64,'engine_rate':44100,'abi_size':296}
def packet(values,deck=1,rate=44100):
    result=dict(POSITION_BINDING)
    result.update(schema=1,pid=705,status='ok',sequence=len(values),native_monotonic_ns=10**9+len(values)*100000000,native_boottime_ns=6*10**9+len(values)*100000000,samples=[])
    result['engine_rate']=rate
    for i,value in enumerate(values,1):
        ds=[]
        for d in range(4):
            v=value if d==deck-1 else 0
            ds.append({'deck':d,'read_rc':0,'loaded':1,'title':'fixture','playhead':v,'pos_s':v/rate})
        result['samples'].append({'sequence':i,'native_monotonic_ns':10**9+i*100000000,'decks':ds})
    return result

class MotionTests(unittest.TestCase):
    def test_capture_renewal_requires_matching_nonce_and_fresh_bound_history(self):
        for fault in ('none','old-nonce','stale'):
            with self.subTest(fault=fault):
                clock=Clock();sent=[]
                def snapshot():
                    p=packet([0,4410,8820,13230,17640]);p['capture_nonce']=sent[-1] if sent and fault!='old-nonce' else ''
                    return dict(positions=p,boottime_ns=p['native_boottime_ns']+(2000000000 if fault=='stale' else 0))
                if fault=='none':
                    result=controls.renew_position_capture(snapshot,sent.append,1,'705',POSITION_BINDING,clock=clock.now,sleep=clock.sleep)
                    self.assertEqual(result['capture_nonce'],sent[0])
                else:
                    with self.assertRaises(controls.MissingEvidence):controls.renew_position_capture(snapshot,sent.append,1,'705',POSITION_BINDING,clock=clock.now,sleep=clock.sleep)
                self.assertEqual(len(sent),1)

    def test_old_producer_is_refused_before_a_renewal_write(self):
        sent=[]
        with self.assertRaises(controls.MissingEvidence):controls.renew_position_capture(lambda:dict(positions=packet([0]*5)),sent.append,1,'705',POSITION_BINDING)
        self.assertEqual(sent,[])

    def witness(self,p,deck=1,**kwargs):
        kwargs.setdefault('now_boottime_ns',p['native_boottime_ns']+1000000)
        return controls.position_motion(p,deck,'705',POSITION_BINDING,**kwargs)
    def test_all_decks_source_rate_movement_and_stationarity(self):
        for d in range(1,5):
            with self.subTest(deck=d):
                self.assertEqual(self.witness(packet([0,4410,8820,13230,17640],d),d)['state'],'MOVING')
                self.assertEqual(self.witness(packet([100]*5,d),d)['state'],'STATIONARY')
    def test_short_loop_equal_phase_is_unknown(self):
        loop={'engaged':'1','bounds':'1','in_s':'0','out_s':'.1'}
        self.assertEqual(self.witness(packet([100]*5),loop=loop,max_source_rate=4)['state'],'UNKNOWN')
    def test_long_loop_stationary_requires_explicit_safe_rate_bound(self):
        loop={'engaged':'1','bounds':'1','in_s':'0','out_s':'2'}
        self.assertEqual(self.witness(packet([100]*5),loop=loop)['state'],'UNKNOWN')
        self.assertEqual(self.witness(packet([100]*5),loop=loop,max_source_rate=4)['state'],'STATIONARY')
    def test_loop_wrap_is_movement_and_unexplained_seek_is_not(self):
        loop={'engaged':'1','bounds':'1','in_s':'1','out_s':'2'}
        p=packet([74970,79380,83790,44100,48510])
        self.assertEqual(self.witness(p,loop=loop,max_source_rate=4)['state'],'MOVING')
        self.assertEqual(self.witness(p,max_source_rate=4)['state'],'UNKNOWN')
        self.assertEqual(self.witness(packet([0,0,4410,4410,4410]))['state'],'UNKNOWN')
    def test_only_post_receipt_history_can_qualify_a_transition(self):
        p=packet([0,4410,8820,13230,17640,22050])
        self.assertEqual(self.witness(p,after_ns=1400000000)['state'],'UNKNOWN')
        self.assertEqual(self.witness(p,after_ns=1100000000)['state'],'MOVING')
    def test_pid_provider_header_rate_and_sequence_integrity(self):
        for field,value in [('pid',706),('provider_build_id','c'*40),('header_sha256','c'*64),('engine_rate',48000),('abi_size',128)]:
            p=packet([0]*5);p[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):self.witness(p)
        p=packet([0]*5);p['samples'][-1]['sequence']=1
        with self.assertRaises(ValueError):self.witness(p)
    def test_failed_read_null_position_and_large_sampling_gap_are_unknown(self):
        for mode in ['read','null','gap']:
            p=packet([100]*5)
            if mode=='read':p['samples'][-1]['decks'][0]['read_rc']=-1
            elif mode=='null':p['samples'][-1]['decks'][0].update(playhead=None,pos_s=None)
            else:
                p['samples'][-1]['native_monotonic_ns']+=400000000
                p['native_monotonic_ns']+=400000000
            with self.subTest(mode=mode):self.assertEqual(self.witness(p)['state'],'UNKNOWN')
    def test_frozen_position_packet_cannot_establish_current_transport(self):
        p=packet([100]*5)
        self.assertEqual(self.witness(p,now_boottime_ns=p['native_boottime_ns']+2*10**9)['state'],'UNKNOWN')
        self.assertEqual(self.witness(p,now_boottime_ns=p['native_boottime_ns']-20000000)['state'],'UNKNOWN')
        self.assertEqual(self.witness(p,now_boottime_ns=p['native_boottime_ns']-9000000)['state'],'STATIONARY')

class Model:
    """Asynchronous-observation fixture; no native play semantics are inferred."""
    def __init__(self,clock):
        self.clock=clock;self.pid='705';self.selected={1:1,2:2};self.previous=[0]*152
        self.loaded=[False]*4;self.playing=[False]*4;self.names=['']*4
        self.quantize=[0]*4;self.key_lock=[0]*4;self.engaged=[0]*4
        self.semantic=True;self.events=[];self.rows={};self.revisions={};self.history=[];self.quantize_decks=list(range(4))
        for d in range(4):
            self.publish(d,'loop');self.publish(d,'play');self.publish(d,'quantize');self.publish(d,'key_lock')
    def publish(self,d,stream):
        key=(d,stream);r=self.revisions.get(key,0)+1;self.revisions[key]=r
        ns=int((self.clock.now()+1)*10**9)
        if stream=='loop':
            on=self.engaged[d]
            row='deck=%d unit=%d revision=%d received_ns=%d engaged=%d bounds=%d in_s=%s out_s=%s mask=15'%(d,10+d,r,ns,on,on,'1.0' if on else '0','2.0' if on else '0')
        else:
            mode,pause,aux=(0,int(self.playing[d]),0) if stream=='play' else (-1,-1,-1)
            q=self.quantize[d] if stream=='quantize' else -1;k=self.key_lock[d] if stream=='key_lock' else -1
            row='live_deck=%d unit=%d stream=%s revision=%d received_ns=%d age_ns=0 valid=1 mode_is_2=%d play_pause=%d play_aux=%d quantize=%d key_lock=%d mask=15'%(d,100+d,stream,r,ns,mode,pause,aux,q,k)
        self.rows[key]=row;self.history.append(row)
    def send(self,payload):
        self.events.append(payload)
        bus,f=payload[0],list(payload[1:])
        if bus==2:
            for d,mask in enumerate([8,4,2,1]):
                if f[33]&mask:self.loaded[d]=True;self.names[d]='fixture'
            return
        if bus!=0:return
        rising=[new&~old for new,old in zip(f,self.previous)];self.previous=f
        for side,offset in [(1,0),(2,8)]:
            if rising[8+offset]&1:self.selected[side]=side
            if rising[9+offset]&1:self.selected[side]=side+2
            d=self.selected[side]-1
            if rising[10+offset]&2:
                self.playing[d]=not self.playing[d];self.publish(d,'play')
            if rising[8+offset]&8:
                self.key_lock[d]^=1;self.publish(d,'key_lock')
            if rising[8+offset]&32:
                self.engaged[d]=1;self.publish(d,'loop')
            if rising[9+offset]&8:
                self.engaged[d]^=1;self.publish(d,'loop')
                if self.engaged[d]:self.playing[d]=True;self.publish(d,'play')
        if rising[10]&1:
            for d in self.quantize_decks:self.quantize[d]^=1;self.publish(d,'quantize')
    def observation(self):
        semantic={'titles':self.names[:],'title_source':'fixture UI',
                  'playing':self.playing[:],'playing_source':'fixture independent transport'} if self.semantic else None
        return {'pid':self.pid,'heartbeat':'azdecks 1\npid '+self.pid+'\nup '+str(self.clock.now())+'\ndecks '+''.join('L' if x else 'E' for x in self.loaded)+'\n',
                'uptime':self.clock.now(),'loop_log':'pid='+self.pid+' exports=1,1\n'+'\n'.join(self.history)+'\n','semantic':semantic}

class PositionModel(Model):
    """Native-shaped copied position history; the controller sees no play flags."""
    def __init__(self,clock):
        super().__init__(clock)
        self.semantic=False;self.positions=[0]*4;self.samples=[];self.sequence=0
        self.sample()
    def sample(self):
        self.sequence+=1
        self.samples.append({'sequence':self.sequence,'native_monotonic_ns':int((self.clock.now()+1)*1e9),
            'decks':[{'deck':d,'read_rc':0,'loaded':int(self.loaded[d]),'title':self.names[d],
                      'playhead':self.positions[d],'pos_s':self.positions[d]/44100} for d in range(4)]})
        self.samples=self.samples[-12:]
    def sleep(self,seconds):
        remaining=seconds
        while remaining>1e-9:
            step=min(.1,remaining);remaining-=step;self.clock.sleep(step)
            for d in range(4):
                if self.playing[d]:
                    self.positions[d]+=round(step*44100)
                    if self.engaged[d]:self.positions[d]=44100+(self.positions[d]-44100)%44100
            self.sample()
    def observation(self):
        value=super().observation();publisher=dict(POSITION_BINDING)
        publisher.update(schema=1,pid=705,status='ok',sequence=self.sequence,
            native_monotonic_ns=int((self.clock.now()+1)*1e9),native_boottime_ns=int(self.clock.now()*1e9),samples=self.samples[:])
        value['positions']=publisher
        return value

class PanelTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.path=DRIVER
        if not self.path.is_file():
            self.path=Path(self.tmp.name)/'driver.py';self.path.write_text(PORTABLE)
        self.frames=[];self.owner='a'*32;self.clock=Clock();self.model=Model(self.clock);self.qmp=[]
        def send(payload):self.frames.append(payload);self.model.send(payload)
        def qmp(command):self.qmp.append(command);return {'return':{}}
        self.c=controls.Controller(self.path,hashlib.sha256(self.path.read_bytes()).hexdigest(),
            Path(self.tmp.name)/'controls','a'*32,lambda:self.owner,send,qmp_execute=qmp,
            read_observation=self.model.observation,pid='705',clock=self.clock.now,sleep=self.clock.sleep,timeout=2,interval=.1)
    def prepare_deck_load(self,controller,deck,selected):
        self.assertIs(controller,self.c)
        return dict(selected,evidence='fixture-selection-deck%d'%deck)
    def test_gui_load_timeout_override_does_not_leak_into_receipt_or_motion_waits(self):
        original=self.c.timeout;seen=[]
        def prepare(controller):
            controller.timeout=180
            return dict(status='PASS',selected_title='fixture',evidence='first-screen')
        def scalar(deck,stream):
            self.assertEqual(self.c.timeout,original)
            seen.append((deck,stream))
        self.c.raw_play_roundtrip=lambda deck:scalar(deck,'play')
        self.c.key_lock_roundtrip=lambda deck:scalar(deck,'key_lock')
        self.c.quantize_roundtrip=lambda:set(range(1,5))
        def motion(deck,want):
            self.assertEqual(self.c.timeout,original)
            raise controls.MissingEvidence('independent fixture motion unavailable')
        self.c.ensure_playing=motion
        self.c.manual_loop=lambda deck:(_ for _ in ()).throw(controls.MissingEvidence('independent fixture pause unavailable'))
        def setup(controller):
            self.assertEqual(controller.timeout,original)
            return dict(status='PASS')
        result=self.c.run_selected_track(prepare,['fixture'],prepare_deck_load=self.prepare_deck_load,prepare_controls=setup)
        self.assertEqual(result['status'],'INCOMPLETE')
        self.assertEqual(seen,[(deck,stream) for deck in range(1,5) for stream in ('play','key_lock')])
        self.assertEqual(self.c.timeout,original)
        self.assertEqual(result['wait_budgets'],{'control_seconds':original,'loads':{str(deck):180 for deck in range(1,5)}})
    def test_failed_gui_preparation_restores_the_original_control_timeout(self):
        original=self.c.timeout
        def prepare(controller):
            controller.timeout=180
            raise controls.MissingEvidence('selected track unavailable')
        result=self.c.run_selected_track(prepare,['fixture'])
        self.assertEqual(result['status'],'INCOMPLETE')
        self.assertEqual(self.c.timeout,original)
    def test_exact_constructor_wait_survives_a_load_override_before_the_sequence(self):
        for configured in (1.5,7,30,180):
            with self.subTest(configured=configured):
                clock=Clock();model=Model(clock)
                c=controls.Controller(self.path,hashlib.sha256(self.path.read_bytes()).hexdigest(),
                    Path(self.tmp.name)/('constructor-'+str(configured)),'a'*32,lambda:self.owner,model.send,
                    read_observation=model.observation,pid='705',clock=clock.now,sleep=clock.sleep,timeout=configured,interval=.1)
                c.timeout=180
                def check(deck):self.assertEqual(c.timeout,configured)
                c.raw_play_roundtrip=check;c.key_lock_roundtrip=check;c.quantize_roundtrip=lambda:set(range(1,5))
                def unavailable(*args):
                    self.assertEqual(c.timeout,configured)
                    raise controls.MissingEvidence('independent fixture transport unavailable')
                c.ensure_playing=unavailable;c.manual_loop=unavailable
                result=c.run_selected_track(lambda c:dict(status='PASS',selected_title='fixture',evidence='first-screen'),
                    ['fixture'],prepare_deck_load=lambda c,d,s:dict(s,evidence='deck%d'%d))
                self.assertEqual(result['status'],'INCOMPLETE')
                self.assertEqual(c.timeout,configured)
                self.assertEqual(result['wait_budgets']['control_seconds'],configured)
    def test_slip_uses_existing_bit_and_selects_each_logical_deck_once(self):
        for deck,side,selector in ((1,1,8),(2,2,8),(3,1,9),(4,2,9)):
            with self.subTest(deck=deck):
                self.frames.clear();self.c.events.clear();self.c.button(deck,'slip')
                offset=0 if side==1 else 8
                self.assertEqual(self.frames[0][1+selector+offset]&1,1)
                pressed=[frame for frame in self.frames if frame[0]==0 and frame[1+8+offset]&4]
                self.assertEqual(len(pressed),1)
                self.assertEqual(self.frames[-1],bytes([0]+self.c.driver.ERP_IDLE))
                self.assertEqual(self.c.events,[{'action':'slip','deck':deck}])

    def test_sync_routes_one_setting_button_to_each_selected_logical_deck(self):
        for deck in range(1,5):
            self.frames.clear();self.c.button(deck,'sync')
            offset=0 if deck in (1,3) else 8
            self.assertTrue(any(frame[0]==0 and frame[1+8+offset]&2 for frame in self.frames))
            self.assertEqual(self.frames[-1],bytes([0]+self.c.driver.ERP_IDLE))
    def test_available_scalar_roundtrips_finish_on_all_decks_before_unavailable_transport_phase(self):
        calls=[]
        self.c.raw_play_roundtrip=lambda deck:calls.append(('play',deck))
        self.c.key_lock_roundtrip=lambda deck:calls.append(('key_lock',deck))
        self.c.quantize_roundtrip=lambda:set(range(1,5))
        def unavailable(deck,want):
            self.assertEqual(calls,[(stream,d) for d in range(1,5) for stream in ('play','key_lock')])
            raise controls.MissingEvidence('independent fixture transport unavailable')
        self.c.ensure_playing=unavailable
        self.c.manual_loop=lambda deck:(_ for _ in ()).throw(controls.MissingEvidence('independent fixture pause unavailable'))
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'first-screen'},
                                         ['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(calls,[(stream,deck) for deck in range(1,5) for stream in ('play','key_lock')])
        self.assertEqual(result['status'],'INCOMPLETE')
        for deck in range(1,5):
            for name in ('raw_play_notifications','key_lock','quantize_state'):
                self.assertEqual(result['decks'][str(deck)][name],'PASS')
            self.assertEqual(result['decks'][str(deck)]['play_pause'],'INCOMPLETE')
    def test_receiver_renewal_boundary_needs_acknowledged_releases_on_both_panel_buses(self):
        self.assertFalse(self.c.panel_inputs_neutral())
        self.c._release(self.c.driver.BUS_ERP)
        self.assertFalse(self.c.panel_inputs_neutral())
        self.c._release(self.c.driver.BUS_MIXER)
        self.assertTrue(self.c.panel_inputs_neutral())
        frame=self.c.driver.ERP_IDLE[:];frame[10]|=2
        self.c._send(self.c.driver.BUS_ERP,frame)
        self.assertFalse(self.c.panel_inputs_neutral())
        self.c._release(self.c.driver.BUS_ERP)
        self.assertTrue(self.c.panel_inputs_neutral())
    def test_failed_or_rejected_stateful_frame_never_becomes_a_neutral_renewal_boundary(self):
        for mode in ('raise','false'):
            c=controls.Controller(self.path,hashlib.sha256(self.path.read_bytes()).hexdigest(),
                Path(self.tmp.name)/('failure-'+mode),'a'*32,lambda:self.owner,lambda p:None,
                clock=self.clock.now,sleep=self.clock.sleep)
            c._release(c.driver.BUS_ERP);c._release(c.driver.BUS_MIXER)
            def fail(payload):
                if mode=='raise':raise BrokenPipeError('ambiguous release')
                return False
            c.send_control=fail
            with self.assertRaises((RuntimeError,OSError)):c._release(c.driver.BUS_ERP)
            self.assertFalse(c.panel_inputs_neutral())
            c.send_control=lambda payload:None
            c._release(c.driver.BUS_ERP)
            self.assertFalse(c.panel_inputs_neutral(),'a failed write cannot be made renewable by a later frame')
    def without_initial_live_receipts(self):
        self.model=PositionModel(self.clock);self.model.semantic=True
        self.model.history=[row for row in self.model.history if 'stream=play ' not in row and 'stream=key_lock ' not in row]
        self.c.read_observation=self.model.observation
        self.c.sleep=self.model.sleep;self.c.driver.time.sleep=self.model.sleep
        self.c.position_binding=POSITION_BINDING
    def test_missing_initial_receipts_still_refuse_without_explicit_setup(self):
        self.without_initial_live_receipts()
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'first-screen'},
                                         ['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'INCOMPLETE',result)
        self.assertFalse(any(e['action'] in ('play','mastertempo') for e in result['events']))
    def test_explicit_setup_obtains_real_receipts_before_known_state_verification(self):
        self.without_initial_live_receipts()
        def setup(controller):
            rows=[controller.establish_live_receipt(d,s) for d in range(1,5) for s in ('play','key_lock')]
            return {'status':'PASS','scope':'observation setup only','streams':rows}
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'first-screen'},
                                         ['fixture'],prepare_deck_load=self.prepare_deck_load,prepare_controls=setup)
        self.assertEqual(result['status'],'PASS',result)
        self.assertEqual(len(result['observation_setup']['streams']),8)
        inputs=[e for e in result['events'] if e['action']=='observation_setup_input']
        self.assertEqual(len(inputs),8)
        receipts=[e for e in result['events'] if e['action']=='observation_setup_receipt']
        self.assertEqual(len(receipts),8)
        for row in receipts:
            self.assertGreater(row['received_ns'],row['before_ns'])
            self.assertEqual(row['unit'],100+row['deck']-1)
        self.assertTrue(all(row['initial_value'] is None for row in result['observation_setup']['streams']))
    def test_failed_observation_setup_is_one_shot_and_never_retried(self):
        self.without_initial_live_receipts()
        self.model.loaded=[True]*4;self.model.names=['fixture']*4
        self.model.sleep(.5);self.c.require_loaded=True
        self.c.send_control=self.frames.append
        with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(1,'play')
        count=len(self.frames)
        with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(1,'play')
        self.assertEqual(len(self.frames),count)
        self.assertEqual(sum(e['action']=='play' for e in self.c.events),1)
    def test_observed_initial_stream_needs_no_setup_input(self):
        self.without_initial_live_receipts()
        self.model.loaded=[True]*4;self.model.names=['fixture']*4
        self.model.sleep(.5);self.c.require_loaded=True;self.model.publish(0,'play')
        result=self.c.establish_live_receipt(1,'play')
        self.assertFalse(result['input_sent']);self.assertIsNone(result['initial_value'])
        self.assertFalse(self.frames)
    def test_observation_setup_requires_loaded_identity_and_fresh_native_clock_before_input(self):
        for mutation in ('unloaded','clock','unit_witness'):
            with self.subTest(mutation=mutation):
                self.without_initial_live_receipts()
                self.model.loaded=[True]*4;self.model.names=['fixture']*4
                self.model.sleep(.5);self.c.require_loaded=True
                if mutation=='unloaded':self.c.require_loaded=False
                if mutation=='clock':
                    read=self.c.read_observation
                    def stale():
                        value=read();value['positions']['native_boottime_ns']-=2000000000;return value
                    self.c.read_observation=stale
                if mutation=='unit_witness':self.model.history=[r for r in self.model.history if 'stream=quantize ' not in r]
                self.frames.clear()
                with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(1,'play')
                self.assertFalse(self.frames)
    def test_observation_setup_rejects_changed_native_unit_and_future_receipt_without_retry(self):
        for mutation in ('unit','future'):
            with self.subTest(mutation=mutation):
                self.without_initial_live_receipts()
                self.model.loaded=[True]*4;self.model.names=['fixture']*4
                self.model.sleep(.5);self.c.require_loaded=True;self.c.setup_attempts.clear()
                send=self.model.send
                def changed(payload):
                    send(payload)
                    if payload[0]==0 and payload[11]&2:
                        row=self.model.history[-1]
                        if mutation=='unit':row=row.replace('unit=100 ','unit=999 ')
                        else:
                            fields=row.split();fields=[('received_ns=999000000000' if p.startswith('received_ns=') else p) for p in fields];row=' '.join(fields)
                        self.model.history[-1]=row
                self.c.send_control=changed
                with self.assertRaisesRegex(ValueError,'unit|future'):self.c.establish_live_receipt(1,'play')
                before=len(self.c.events)
                with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(1,'play')
                self.assertEqual(len(self.c.events),before)
    def test_setup_waits_through_transient_post_input_clock_unavailability_on_all_decks(self):
        for deck in range(1,5):
            for stream in ('play','key_lock'):
                with self.subTest(deck=deck,stream=stream):
                    self.without_initial_live_receipts();self.model.loaded=[True]*4;self.model.names=['fixture']*4
                    self.model.sleep(.5);self.c.require_loaded=True;self.c.setup_attempts.clear();self.c.events.clear()
                    self.c.send_control=self.model.send;read=self.c.read_observation;missing=[2]
                    def transient():
                        value=read()
                        if (deck,stream) in self.c.setup_attempts and missing[0]:
                            missing[0]-=1;value['positions']['native_boottime_ns']-=2000000000
                        return value
                    self.c.read_observation=transient
                    try:result=self.c.establish_live_receipt(deck,stream)
                    except controls.MissingEvidence as error:self.fail('transient clock aborted the existing finite wait: '+str(error))
                    self.assertTrue(result['input_sent']);self.assertEqual(result['receipt']['unit'],str(99+deck))
                    self.assertEqual(sum(e['action']=='observation_setup_input' for e in self.c.events),1)
                    self.assertEqual(sum(e['action']=='observation_setup_receipt' for e in self.c.events),1)
    def test_setup_clock_unavailable_until_deadline_stays_incomplete_without_replay(self):
        self.without_initial_live_receipts();self.model.loaded=[True]*4;self.model.names=['fixture']*4
        self.model.sleep(.5);self.c.require_loaded=True;self.c.send_control=self.model.send;read=self.c.read_observation
        def stale():
            value=read()
            if self.c.setup_attempts:value['positions']['native_boottime_ns']-=2000000000
            return value
        self.c.read_observation=stale;start=self.clock.now()
        with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(3,'key_lock')
        self.assertGreaterEqual(self.clock.now()-start,self.c.timeout)
        count=len(self.c.events)
        with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(3,'key_lock')
        self.assertEqual(len(self.c.events),count)
        self.assertEqual(sum(e['action']=='mastertempo' for e in self.c.events),1)
    def test_setup_changed_unit_refuses_even_when_post_input_clock_is_unavailable(self):
        self.without_initial_live_receipts();self.model.loaded=[True]*4;self.model.names=['fixture']*4
        self.model.sleep(.5);self.c.require_loaded=True;self.c.send_control=self.model.send;read=self.c.read_observation
        def changed():
            value=read()
            if self.c.setup_attempts:
                value['positions']['native_boottime_ns']-=2000000000
                value['loop_log']=value['loop_log'].replace('live_deck=0 unit=100 stream=play','live_deck=0 unit=999 stream=play')
            return value
        self.c.read_observation=changed
        with self.assertRaisesRegex(ValueError,'unit'):self.c.establish_live_receipt(1,'play')
        self.assertEqual(sum(e['action']=='play' for e in self.c.events),1)
    def test_setup_fresh_receipt_at_exact_read_deadline_cannot_pass(self):
        self.without_initial_live_receipts();self.model.loaded=[True]*4;self.model.names=['fixture']*4
        self.model.sleep(.5);self.c.require_loaded=True;self.c.send_control=self.model.send;read=self.c.read_observation
        def late():
            if self.c.setup_attempts:self.model.sleep(self.c.timeout)
            return read()
        self.c.read_observation=late
        with self.assertRaises(controls.MissingEvidence):self.c.establish_live_receipt(4,'play')
        self.assertEqual(sum(e['action']=='play' for e in self.c.events),1)
    def test_all_four_logical_decks_route_through_physical_pair(self):
        for deck,offset,selector in [(1,0,8),(2,8,8),(3,0,9),(4,8,9)]:
            with self.subTest(deck=deck):
                self.frames=[];self.c.send_control=self.frames.append
                self.c.button(deck,'mastertempo')
                self.assertEqual(self.frames[0][1+selector+offset]&1,1)
                self.assertTrue(any(f[1+8+offset]&8 for f in self.frames))
                self.assertEqual(self.frames[-1],bytes([0]+self.c.driver.ERP_IDLE))
    def test_changed_owner_prevents_all_control_writes(self):
        self.owner='b'*32
        with self.assertRaisesRegex(ValueError,'ownership'):self.c.button(1,'play')
        self.assertEqual(self.frames,[])
    def test_failure_still_releases_pressed_bus(self):
        calls=[]
        def send(payload):
            calls.append(payload)
            if payload[11]&2:raise RuntimeError('injected write failure')
        self.c.send_control=send
        with self.assertRaisesRegex(RuntimeError,'write failure'):self.c.button(1,'play')
        self.assertEqual(calls[-1],bytes([0]+self.c.driver.ERP_IDLE))
    def test_load_targets_all_four_logical_load_masks(self):
        for deck,mask in enumerate([8,4,2,1],1):
            self.frames.clear();self.c.load(deck)
            self.assertEqual(self.frames[0][0],2);self.assertEqual(self.frames[0][34],mask)
            self.assertEqual(self.frames[-1],bytes([2]+self.c.driver._mixer()))
    def test_load_survives_first_mixer_pulse_not_being_sampled(self):
        missed=[]
        def send(payload):
            self.frames.append(payload)
            if payload[0]==2 and payload[34]&4 and not missed:
                missed.append(payload)
                return
            self.model.send(payload)
        self.c.send_control=send
        self.c.load(2)
        self.assertTrue(missed)
        self.assertEqual(self.model.loaded,[False,True,False,False])
        self.assertEqual(self.frames[-1],bytes([2]+self.c.driver._mixer()))
    def test_load_survives_slow_transport_and_a_missed_first_pulse(self):
        pressed=[]
        def send(payload):
            self.frames.append(payload)
            if payload[0]==2 and payload[34]&4:
                pressed.append(payload);self.clock.sleep(.2)
                if len(pressed)==1:return
            self.model.send(payload)
        self.c.send_control=send
        self.c.load(2)
        self.assertEqual(self.model.loaded,[False,True,False,False])
        self.assertGreaterEqual(len(pressed),2)
        self.assertEqual(self.frames[-1],bytes([2]+self.c.driver._mixer()))
    def test_quantize_is_one_global_input(self):
        self.c.quantize()
        self.assertEqual(self.frames[0][11]&1,1)
        self.assertEqual(self.frames[0][19]&1,0)
        self.assertEqual(self.c.events[-1]['scope'],'unit input; affected decks require observations')
    def test_no_global_transport_fallback_exists(self):
        with self.assertRaisesRegex(RuntimeError,'fallback forbidden'):self.c.driver.azemu.ssh('not allowed')
        self.assertEqual(self.c.driver.azemu.SOCK_DIR,str(self.c.output))
        self.assertIsNone(self.c.driver.azemu.QMP)
    def test_touch_uses_screenshot_coordinates_and_always_releases(self):
        self.c.tap_pixel(640,400)
        events=self.qmp[0]['arguments']['events']
        self.assertEqual(events[0]['data']['value'],round(.5*0x7fff))
        self.assertEqual(events[1]['data']['value'],round(.5*0x7fff))
        self.assertFalse(self.qmp[-1]['arguments']['events'][0]['data']['down'])
    def test_failed_touch_still_sends_release(self):
        def bad(command):
            self.qmp.append(command)
            return {'error':{}} if len(self.qmp)==1 else {'return':{}}
        self.c.qmp_execute=bad
        with self.assertRaisesRegex(RuntimeError,'QMP'):self.c.tap_pixel(640,400)
        self.assertFalse(self.qmp[-1]['arguments']['events'][0]['data']['down'])
    def test_invalid_deck_button_frame_and_touch_never_write(self):
        for deck in [0,5,True,'1']:
            with self.assertRaises(ValueError):self.c.button(deck,'play')
        with self.assertRaises(ValueError):self.c.button(1,'arbitrary')
        for xy in [(-1,0),(1280,0),(0,800),(float('nan'),0)]:
            with self.assertRaises(ValueError):self.c.tap_pixel(*xy)
        for bus,frame in [(4,[0]*152),(0,[0]*151),(0,[256]*152)]:
            with self.assertRaises(ValueError):self.c._send(bus,frame)
        self.assertEqual(self.frames,[]);self.assertEqual(self.qmp,[])
    def test_stale_heartbeat_pid_change_and_finished_capture_are_rejected(self):
        value=self.model.observation();value['uptime']+=6
        with self.assertRaisesRegex(ValueError,'heartbeat'):controls.decode_observation(**value)
        value=self.model.observation();value['pid']='706'
        with self.assertRaisesRegex(ValueError,'identity'):controls.decode_observation(**value)
        value=self.model.observation();value['loop_log']+='complete errors=0\n'
        with self.assertRaisesRegex(ValueError,'completed'):controls.decode_observation(**value)
    def test_raw_play_flags_never_supply_playing_or_title(self):
        self.model.semantic=False
        value=controls.decode_observation(**self.model.observation())
        self.assertTrue(all(d['playing'] is None and d['title'] is None for d in value['decks']))
        with self.assertRaises(controls.MissingEvidence):self.c.ensure_playing(1,True)
    def test_later_deck_load_requires_independent_current_gui_selection(self):
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'first-screen'},['fixture'])
        self.assertEqual(result['status'],'INCOMPLETE',result)
        self.assertEqual([e['deck'] for e in result['events'] if e['action']=='load'],[1])
        self.assertTrue(any('deck2' in reason and 'selection' in reason for reason in result['missing']),result)
    def test_all_deck_gui_callback_can_restore_context_after_each_loaded_track(self):
        ready=[True];visited=[];send=self.model.send
        def contextual_send(payload):
            loading=payload[0]==2 and bool(payload[34]&15)
            if loading and not ready[0]:return
            send(payload)
            if loading:ready[0]=False
        def prepare(controller,deck,selected):
            self.assertIs(controller,self.c);self.assertEqual(self.model.loaded[:deck-1],[True]*(deck-1))
            visited.append(deck);ready[0]=True
            return dict(selected,evidence='current-deck%d-screen'%deck)
        self.c.send_control=contextual_send
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'first-screen'},['fixture'],prepare_deck_load=prepare)
        self.assertEqual(result['status'],'PASS',result)
        self.assertEqual(visited,[2,3,4]);self.assertEqual(self.model.loaded,[True]*4)
        self.assertEqual(set(result['load_selections']),{'1','2','3','4'})
    def test_reused_gui_selection_evidence_refuses_before_next_load(self):
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'first-screen'},['fixture'],
            prepare_deck_load=lambda controller,deck,selected:selected)
        self.assertEqual(result['status'],'INCOMPLETE',result)
        self.assertEqual([e['deck'] for e in result['events'] if e['action']=='load'],[1])
        self.assertTrue(any('selection' in reason for reason in result['missing']),result)

    def test_full_all_deck_sequence_with_independent_witnesses(self):
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen.ppm'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'PASS',result)
        for deck in range(1,5):self.assertEqual(set(result['decks'][str(deck)].values()),{'PASS'})
        self.assertEqual(self.model.playing,[False]*4);self.assertEqual(self.model.engaged,[0]*4)
        self.assertEqual(self.model.key_lock,[0]*4);self.assertEqual(self.model.quantize,[0]*4)
        self.assertTrue((self.c.output/'report.json').is_file())
        self.assertEqual(result['panel_source']['sha256'],hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertEqual(result['semantic_sources']['playing_source'],['fixture independent transport'])
        self.assertEqual(result['owner_nonce'],'a'*32)
    def motion_transition_fixture(self,deck,label,timeout=2):
        clock=Clock();model=PositionModel(clock)
        model.loaded=[True]*4;model.names=['fixture']*4;model.sleep(1.5)
        c=controls.Controller(self.path,hashlib.sha256(self.path.read_bytes()).hexdigest(),
            Path(self.tmp.name)/('motion-'+label),'a'*32,lambda:self.owner,model.send,
            read_observation=model.observation,pid='705',clock=clock.now,sleep=model.sleep,
            timeout=timeout,interval=.1,position_binding=POSITION_BINDING)
        return c,model,clock
    def test_combined_play_receipt_consumes_early_qualified_motion_on_every_deck(self):
        for deck in range(1,5):
            c,model,clock=self.motion_transition_fixture(deck,'early%d'%deck)
            read=model.observation;post=[0]
            def limited_window():
                if model.playing[deck-1]:
                    post[0]+=1;model.sleep(.6 if post[0]==1 else .1)
                value=read()
                # Only the two windows seen while the old code waited for the
                # raw receipt qualify. A later separate motion wait must fail.
                if post[0]>2:value['positions']['status']='unavailable'
                return value
            c.read_observation=limited_window
            with self.subTest(deck=deck):
                c.ensure_playing(deck,True)
                self.assertEqual(post[0],2)
                self.assertEqual([e['action'] for e in c.events].count('play'),1)
                self.assertEqual(c.motion_evidence[deck]['state'],'MOVING')
    def test_combined_play_motion_still_requires_two_consecutive_qualified_windows(self):
        c,model,clock=self.motion_transition_fixture(1,'consecutive');read=model.observation;post=[0]
        def interrupted():
            if model.playing[0]:post[0]+=1;model.sleep(.6 if post[0]==1 else .1)
            value=read()
            if post[0]==2:value['positions']['status']='unavailable'
            return value
        c.read_observation=interrupted;c.ensure_playing(1,True)
        self.assertEqual(post[0],4)
        self.assertEqual([e['action'] for e in c.events].count('play'),1)
    def test_combined_play_motion_rejects_old_pre_stimulus_stale_and_mismatched_evidence(self):
        for mode in ('old_stamp','pre_stimulus','pre_history','stale','gap','future','unit','provider'):
            c,model,clock=self.motion_transition_fixture(1,mode);read=model.observation
            old_stamp=int(model.rows[(0,'play')].split('received_ns=')[1].split()[0])
            fence=int((clock.now()+1)*1e9);post=[0]
            def invalid():
                if model.playing[0]:post[0]+=1;model.sleep(.6 if post[0]==1 else .1)
                value=json.loads(json.dumps(read()))
                if post[0]:
                    row=model.rows[(0,'play')];stamp=int(row.split('received_ns=')[1].split()[0])
                    if mode in ('old_stamp','pre_stimulus','future'):
                        replacement=old_stamp if mode=='old_stamp' else fence-1 if mode=='pre_stimulus' else int((clock.now()+1)*1e9)+20000000
                        value['loop_log']=value['loop_log'].replace(row,row.replace('received_ns='+str(stamp),'received_ns='+str(replacement)))
                    elif mode=='unit':value['loop_log']=value['loop_log'].replace(row,row.replace('unit=100 ','unit=999 '))
                    elif mode=='pre_history':
                        # A valid packet can retain only samples older than the
                        # post-input receipt. Their motion cannot prove change.
                        for sample in value['positions']['samples']:sample['native_monotonic_ns']-=2000000000
                    elif mode=='stale':value['positions']['native_boottime_ns']-=2000000000
                    elif mode=='gap':
                        value['positions']['samples'][-1]['native_monotonic_ns']+=300000000
                        value['positions']['native_monotonic_ns']+=300000000
                    elif mode=='provider':value['positions']['provider_build_id']='c'*40
                return value
            c.read_observation=invalid
            with self.subTest(mode=mode):
                if mode in ('future','unit','provider'):
                    with self.assertRaises(ValueError):c.ensure_playing(1,True)
                else:
                    with self.assertRaises(controls.MissingEvidence):c.ensure_playing(1,True)
                self.assertEqual([e['action'] for e in c.events].count('play'),1)
    def test_combined_play_motion_counts_read_time_and_rejects_exact_deadline(self):
        for offset in (-.001,0,.001):
            c,model,clock=self.motion_transition_fixture(1,'deadline'+str(offset));read=model.observation;post=[0];start=[None]
            def timed():
                if model.playing[0]:
                    post[0]+=1
                    if post[0]==1:start[0]=clock.now();model.sleep(.6)
                    elif post[0]==2:
                        target=start[0]+c.timeout+offset
                        model.sleep(max(0,target-clock.now()));clock.value=target
                    else:model.sleep(.1)
                return read()
            c.read_observation=timed
            with self.subTest(offset=offset):
                if offset<0:c.ensure_playing(1,True)
                else:
                    with self.assertRaises(controls.MissingEvidence):c.ensure_playing(1,True)
                self.assertEqual(post[0],2)
                self.assertEqual([e['action'] for e in c.events].count('play'),1)
    def test_missing_loop_rate_proof_refuses_before_inputs_on_every_deck(self):
        for deck in range(1,5):
            with self.subTest(deck=deck):
                self.model=PositionModel(self.clock)
                self.model.loaded=[True]*4;self.model.names=['fixture']*4
                def send(payload):self.frames.append(payload);self.model.send(payload)
                self.c.send_control=send;self.c.read_observation=self.model.observation
                self.c.sleep=self.model.sleep;self.c.driver.time.sleep=self.model.sleep
                self.c.position_binding=POSITION_BINDING
                self.model.sleep(1.5)
                self.frames.clear()
                with self.assertRaises(controls.MissingEvidence) as caught:
                    self.c.manual_loop(deck)
                self.assertEqual(self.frames,[])
                self.assertIn('rate bound',str(caught.exception))
    def test_missing_loop_rate_proof_preserves_other_all_deck_checks(self):
        self.model=PositionModel(self.clock)
        self.c.send_control=self.model.send;self.c.read_observation=self.model.observation
        self.c.sleep=self.model.sleep;self.c.driver.time.sleep=self.model.sleep
        self.c.position_binding=POSITION_BINDING
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'INCOMPLETE',result)
        for deck in range(1,5):
            checks=result['decks'][str(deck)]
            self.assertEqual(checks['manual_loop_paused_exit_reloop'],'INCOMPLETE')
            self.assertTrue(all(value=='PASS' for name,value in checks.items() if name!='manual_loop_paused_exit_reloop'),checks)
        self.assertEqual(self.model.engaged,[0]*4)
        self.assertEqual(self.model.playing,[False]*4)
    def test_missing_semantic_witness_is_incomplete_not_a_transport_pass(self):
        self.model.semantic=False
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen.ppm'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'INCOMPLETE',result)
        for deck in range(1,5):
            self.assertEqual(result['decks'][str(deck)]['raw_play_notifications'],'PASS')
            self.assertEqual(result['decks'][str(deck)]['play_pause'],'INCOMPLETE')
            self.assertEqual(result['decks'][str(deck)]['manual_loop_paused_exit_reloop'],'INCOMPLETE')
    def test_unverified_track_selection_never_loads(self):
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'other','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'INCOMPLETE');self.assertEqual(self.model.loaded,[False]*4)
    def test_timeout_and_backward_clock_are_bounded(self):
        with self.assertRaisesRegex(RuntimeError,'unobserved'):self.c.wait(lambda s:False,'missing')
        self.clock.value-=20
        with self.assertRaisesRegex(ValueError,'clock'):self.c.wait(lambda s:True,'backward')
    def test_native_unknown_heartbeat_is_unavailable_for_each_logical_slot(self):
        for deck in range(1,5):
            value=self.model.observation();mask=list('EEEE');mask[deck-1]='U'
            value['heartbeat']=value['heartbeat'].replace('decks EEEE','decks '+''.join(mask))
            with self.subTest(deck=deck),self.assertRaisesRegex(controls.MissingEvidence,'unknown'):
                controls.decode_observation(**value)
    def test_unknown_heartbeat_resets_consecutive_wait_without_control_replay(self):
        reads=[0];predicates=[]
        def read():
            reads[0]+=1;value=self.model.observation()
            if reads[0]==2:value['heartbeat']=value['heartbeat'].replace('decks EEEE','decks UEEE')
            return value
        self.c.read_observation=read
        self.c.wait(lambda value:predicates.append(value) or True,'fresh known heartbeat')
        self.assertEqual(reads[0],4);self.assertEqual(len(predicates),3)
        self.assertEqual(self.frames,[])
        self.assertEqual([e['action'] for e in self.c.events],['observation_rejected'])
        self.assertIn('unknown',self.c.events[0]['reason'])
    def test_persistent_unknown_heartbeat_never_returns_prior_partial_or_extends_deadline(self):
        reads=[0]
        def read():
            reads[0]+=1;value=self.model.observation()
            if reads[0]>1:value['heartbeat']=value['heartbeat'].replace('decks EEEE','decks UEEE')
            return value
        self.c.read_observation=read;start=self.clock.now()
        with self.assertRaisesRegex(controls.MissingEvidence,'unknown'):
            self.c.wait(lambda value:False,'known heartbeat',allow_partial=True)
        self.assertLessEqual(self.clock.now()-start,self.c.timeout+self.c.interval)
        self.assertEqual(self.frames,[])
    def test_unknown_heartbeat_never_hides_changed_identity_future_or_malformed_state(self):
        for mutation in ('identity','both_identities','future','format'):
            reads=[0]
            def read():
                reads[0]+=1;value=self.model.observation()
                value['heartbeat']=value['heartbeat'].replace('decks EEEE','decks UEEE')
                if mutation in ('identity','both_identities'):
                    value['heartbeat']=value['heartbeat'].replace('pid 705','pid 706')
                    if mutation=='both_identities':value['pid']='706'
                elif mutation=='future':value['uptime']-=1
                else:value['heartbeat']=value['heartbeat'].replace('decks UEEE','decks UXEE')
                return value
            self.c.read_observation=read
            with self.subTest(mutation=mutation),self.assertRaises(ValueError) as caught:
                self.c.wait(lambda value:True,'invalid unknown heartbeat')
            self.assertNotIsInstance(caught.exception,controls.MissingEvidence)
            self.assertEqual(reads[0],1)
        self.assertEqual(self.frames,[])
    def test_native_unknown_during_each_load_wait_keeps_exactly_one_load_per_deck(self):
        read=self.model.observation;unknown=set()
        def first_zero():
            value=read()
            for d,loaded in enumerate(self.model.loaded):
                if loaded and d not in unknown:
                    unknown.add(d);lines=value['heartbeat'].splitlines();mask=list(lines[3][6:]);mask[d]='U'
                    lines[3]='decks '+''.join(mask);value['heartbeat']='\n'.join(lines)+'\n';break
            return value
        self.c.read_observation=first_zero
        result=self.c.run_selected_track(lambda c:dict(status='PASS',selected_title='fixture',evidence='first-screen'),
                                         ['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'PASS',result)
        self.assertEqual([e['deck'] for e in result['events'] if e['action']=='load'],[1,2,3,4])
        self.assertEqual(len([e for e in result['events'] if e['action']=='observation_rejected']),4)
    def test_transient_stale_heartbeat_is_discarded_before_stable_observations(self):
        reads=[0];predicates=[]
        def read():
            reads[0]+=1;value=self.model.observation()
            if reads[0]==2:value['uptime']+=6
            return value
        self.c.read_observation=read
        self.c.wait(lambda value:predicates.append(value) or True,'fresh observations')
        self.assertEqual(reads[0],4)
        self.assertEqual(len(predicates),3)
        self.assertEqual(self.frames,[])
        self.assertEqual([event['action'] for event in self.c.events],['observation_rejected'])
    def test_persistently_stale_heartbeat_cannot_return_old_partial_state(self):
        reads=[0]
        def read():
            reads[0]+=1;value=self.model.observation()
            if reads[0]>1:value['uptime']+=6
            return value
        self.c.read_observation=read;start=self.clock.now()
        with self.assertRaises(controls.MissingEvidence):
            self.c.wait(lambda value:False,'fresh observation',allow_partial=True)
        self.assertLessEqual(self.clock.now()-start,self.c.timeout+2*self.c.interval)
        self.assertEqual(self.frames,[])
    def test_late_positive_observation_cannot_pass_after_the_wait_budget(self):
        for partial in (False,True):
            calls=[0]
            def read():
                calls[0]+=1
                self.clock.sleep(.1 if calls[0]==1 else self.c.timeout+1)
                return self.model.observation()
            self.c.read_observation=read
            with self.subTest(partial=partial),self.assertRaises(controls.MissingEvidence):
                self.c.wait(lambda value:True,'late positive observation',allow_partial=partial)
        self.assertEqual(self.frames,[])
    def test_future_or_wrong_identity_observation_is_not_retried(self):
        for mutation in ('future','identity'):
            calls=[0]
            def read():
                calls[0]+=1;value=self.model.observation()
                if mutation=='future':value['uptime']-=1
                else:value['pid']='706'
                return value
            self.c.read_observation=read
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):
                self.c.wait(lambda value:True,'invalid observation')
            self.assertEqual(calls[0],1)
        self.assertEqual(self.frames,[])
    def test_unchanged_receipt_cannot_satisfy_button_transition(self):
        self.c.send_control=lambda payload:None
        with self.assertRaisesRegex(RuntimeError,'unobserved'):self.c.key_lock_roundtrip(1)
    def test_raw_two_press_notifications_need_not_restore_cueing_tuple(self):
        original=self.model.send;counter=[0]
        def change(payload):
            original(payload)
            if payload[0]==0 and payload[11]&2:
                counter[0]+=1
                if counter[0]==2:
                    # A paused state can legitimately differ from an initial
                    # cueing tuple; no semantic equality may be invented here.
                    row=self.model.history[-1].replace('play_aux=0','play_aux=1')
                    self.model.history[-1]=row
        self.c.send_control=change
        self.c.raw_play_roundtrip(1)
    def test_loaded_title_can_follow_loaded_heartbeat(self):
        read=self.model.observation;counts=[0]*4
        def lagging():
            value=read()
            for d in range(4):
                if self.model.loaded[d] and counts[d]<5:
                    counts[d]+=1;value['semantic']['titles'][d]='pending'
            return value
        self.c.read_observation=lagging
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'PASS',result)
    def test_missing_title_can_arrive_after_loaded_heartbeat(self):
        read=self.model.observation;counts=[0]*4
        def lagging():
            value=read()
            for d in range(4):
                if self.model.loaded[d] and counts[d]<5:
                    counts[d]+=1;value['semantic']['titles'][d]=None
            return value
        self.c.read_observation=lagging
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'PASS',result)
    def test_all_four_decks_use_copied_source_positions_without_playing_semantics(self):
        self.model=PositionModel(self.clock)
        self.c.send_control=self.model.send;self.c.read_observation=self.model.observation
        self.c.sleep=self.model.sleep;self.c.driver.time.sleep=self.model.sleep
        self.c.position_binding=POSITION_BINDING;self.c.max_source_rate=4
        self.c.rate_bound_source='fixture transport speed <= 1x; conservative 4x test bound'
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'PASS',result)
        for deck in range(1,5):self.assertEqual(set(result['decks'][str(deck)].values()),{'PASS'})
        self.assertNotIn('playing_source',result['semantic_sources'])
        witnesses=[event['position_evidence'] for event in result['events'] if event['action']=='transport_observed']
        self.assertTrue(witnesses)
        self.assertTrue(all(witness['samples']>=5 and witness['engine_rate']==44100 for witness in witnesses))
        loop_witnesses=[event for event in result['events'] if event['action']=='loop_transition_observed']
        self.assertEqual(len(loop_witnesses),12)
        self.assertEqual({event['deck'] for event in loop_witnesses},{1,2,3,4})
        self.assertTrue(all(event['position_evidence']['window_start_ns']>=event['receipt_ns'] for event in loop_witnesses))
        self.assertEqual(self.model.playing,[False]*4)
    def test_ambiguous_source_positions_after_loop_receipt_are_incomplete(self):
        self.model=PositionModel(self.clock)
        self.model.loaded=[True]*4;self.model.names=['fixture']*4
        self.model.engaged[0]=1;self.model.publish(0,'loop')
        self.c.send_control=self.model.send;self.c.read_observation=self.model.observation
        self.c.sleep=self.model.sleep;self.c.driver.time.sleep=self.model.sleep
        self.c.position_binding=POSITION_BINDING  # No independently established speed bound.
        self.model.sleep(1.5)
        before=dict(self.c._stream(self.c.observe(),1,'loop'));before['revision']='0'
        with self.assertRaises(controls.MissingEvidence):self.c._wait_loop_transition(1,before,'1',False,'ambiguous loop state')
    def test_quantize_collects_late_deck_notifications(self):
        self.model.quantize_decks=[0];read=self.model.observation;count=[0];published=[False]
        def lagging():
            if self.model.quantize[0]==1 and not published[0]:
                count[0]+=1
                if count[0]>=4:
                    for d in [1,2,3]:self.model.quantize[d]=1;self.model.publish(d,'quantize')
                    self.model.quantize_decks=list(range(4));published[0]=True
            return read()
        self.c.read_observation=lagging
        self.assertEqual(self.c.quantize_roundtrip(),[1,2,3,4])
        self.assertEqual(self.model.quantize,[0]*4)
    def test_missing_global_deck_transition_is_incomplete(self):
        self.model.quantize_decks=[0]
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'INCOMPLETE',result)
        self.assertEqual(result['decks']['1']['quantize_state'],'PASS')
        self.assertEqual(result['decks']['4']['quantize_state'],'INCOMPLETE')
    def test_semantic_fields_need_explicit_provenance(self):
        value=self.model.observation();del value['semantic']['playing_source']
        with self.assertRaisesRegex(ValueError,'witness'):controls.decode_observation(**value)
    def test_screenshot_uses_container_work_path_and_retains_host_evidence(self):
        work=Path(self.tmp.name)/'work';work.mkdir();self.c.work_dir=work
        def qmp(command):
            self.qmp.append(command)
            name=command['arguments']['filename']
            self.assertEqual(name,'/work/sdk-controls-ready.ppm')
            (work/Path(name).name).write_bytes(b'P6\n1 1\n255\n123')
            return {'return':{}}
        self.c.qmp_execute=qmp
        path=Path(self.c.screenshot('ready'))
        self.assertEqual(path.read_bytes(),b'P6\n1 1\n255\n123')
        with self.assertRaisesRegex(ValueError,'safe screenshot'):self.c.screenshot('../escape')
    def test_unit_replacement_cannot_satisfy_transition(self):
        before={'unit':'1','revision':'9'};after={'unit':'2','revision':'10'}
        with self.assertRaisesRegex(ValueError,'unit changed'):self.c._newer(before,after)
    def test_malformed_observation_and_source_hash_are_rejected(self):
        self.c.read_observation=lambda:{'pid':'705'}
        with self.assertRaisesRegex(ValueError,'mapping'):self.c.observe()
        for sha in [None,'b'*64]:
            with self.assertRaisesRegex(ValueError,'hash mismatch'):
                controls.load_driver(self.path,sha,self.c.output,lambda bus,frame:None)
    def test_deck_unload_during_controls_cannot_establish_a_loaded_pass(self):
        read=self.model.observation
        def unload():
            if any(payload[0]==0 and (payload[11]&2 or payload[19]&2) for payload in self.model.events):
                self.model.loaded[3]=False
            return read()
        self.c.read_observation=unload
        result=self.c.run_selected_track(lambda c:{'status':'PASS','selected_title':'fixture','evidence':'screen'},['fixture'],prepare_deck_load=self.prepare_deck_load)
        self.assertEqual(result['status'],'FAIL',result)
        self.assertTrue(any('loaded decks changed' in error for error in result['errors']),result)

if __name__=='__main__':unittest.main()
