#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Observed panel controls for a nonce-owned emulator, with no global transport.

All I/O is injected. ``verify_owner()`` must read the live owned-VM label and
return ``owner_token``; ``send_control(bytes)`` must perform one checked write
to that VM's /dev/subucom_ctrl through the caller's strict SSH connection.
``qmp_execute(command)`` operates on that same owned VM. Each callback must have
its own bounded I/O timeout. Nothing here imports the normal azemu connection.

``read_observation()`` returns arguments for decode_observation(). Existing
heartbeat and loop/live probe logs provide loaded flags, loops and raw streams.
They do NOT provide titles or semantic playing/paused state. Those require
explicit independent witnesses, e.g. a read-only SDK position client or inspected
UI, supplied as ``semantic``. Raw play bytes are never translated into transport
semantics. Missing witnesses yield INCOMPLETE coverage, never an invented pass.

Integration plan, deliberately not applied to the existing runtime executor:
call run_selected_track() during its operator capture, before stopping probes;
collect this report separately and validate the final heartbeat against LLLL.
The current empty-deck runtime's EEEE assertion must not be reused for that run.
No grid-quantization, native SDK commands, saved loops or audio output is proved.
"""
from sdk_timestamps import compare_timestamps
import ast
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import sys
import time
import types
import uuid

try:
    from sdk_runtime_check import validate_transcript
except ImportError:
    # The path is this module's sibling directory, never an emulator-global path.
    sys.path.insert(0,str(Path(__file__).resolve().parent))
    from sdk_runtime_check import validate_transcript

SCOPE='owned emulator panel inputs and independently observed all-deck state transitions'
SIDES={1:(1,'decksel1'),2:(2,'decksel2'),3:(1,'decksel3'),4:(2,'decksel4')}
BUTTONS={'play','cue','loopin','loopout','reloop','mastertempo','sync','slip'}

class MissingEvidence(Exception):pass
class UnavailableObservation(MissingEvidence,ValueError):pass
class StaleObservation(UnavailableObservation):pass

def load_driver(path, expected_sha256, state, send, clock=time.monotonic, sleep=time.sleep):
    """Reuse exact az-drive builders in a private namespace with forbidden fallback.

    Remove only its global azemu import and sys.path insertion. Every frame
    builder remains the selected, digest-bound source text. Its state directory
    and send() are injected; its unused SSH fallback always raises.
    """
    path=Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size>131072:
        raise ValueError('invalid panel driver source')
    raw=Path(path).read_bytes()
    if not isinstance(expected_sha256,str) or not re.fullmatch('[0-9a-f]{64}',expected_sha256) or hashlib.sha256(raw).hexdigest()!=expected_sha256:
        raise ValueError('panel driver source hash mismatch')
    tree=ast.parse(raw.decode(),str(path))
    if sum(isinstance(node,ast.Import) and any(a.name=='azemu' for a in node.names) for node in tree.body)!=1:
        raise ValueError('one explicit azemu import required for isolation')
    tree.body=[node for node in tree.body if not
               (isinstance(node,ast.Import) and any(a.name=='azemu' for a in node.names)) and not
               (isinstance(node,ast.Expr) and isinstance(node.value,ast.Call) and
                isinstance(node.value.func,ast.Attribute) and node.value.func.attr=='insert' and
                isinstance(node.value.func.value,ast.Attribute) and node.value.func.value.attr=='path' and
                isinstance(node.value.func.value.value,ast.Name) and node.value.func.value.value.id=='sys')]
    def forbidden(*args,**kwargs):raise RuntimeError('global emulator fallback forbidden')
    azemu=types.SimpleNamespace(SOCK_DIR=str(state),CTRL_SOCK='FORBIDDEN',WIN=False,QMP=None,ssh=forbidden)
    module=types.ModuleType('owned_panel_driver')
    module.__dict__.update(__file__=str(path),azemu=azemu)
    exec(compile(tree,str(path),'exec'),module.__dict__)
    module.send=send
    module.time=types.SimpleNamespace(time=clock,sleep=sleep)
    return module

def _fields(line):
    pairs=[part.split('=',1) for part in line.split()]
    if any(len(pair)!=2 for pair in pairs) or len({p[0] for p in pairs})!=len(pairs):
        raise ValueError('malformed or duplicate observation field')
    return dict(pairs)

def position_motion(packet,deck,pid,binding,after_ns=0,loop=None,max_source_rate=None,now_boottime_ns=None):
    """Qualify movement/stationarity of source position, not a native pause flag.

    A frozen short-loop phase can alias moving playback. Stationarity inside a
    loop therefore needs an explicit source-rate bound and a loop longer than
    the maximum possible travel between samples. No bound is guessed from the
    WAV rate. Caller-supplied bounds must have independent provenance.
    """
    result={'state':'UNKNOWN','reason':'position history unavailable'}
    def unknown(reason):
        result['reason']=reason;return result
    def unsigned(value,name,positive=False):
        if type(value) is not int or not (1 if positive else 0)<=value<2**64:raise ValueError('invalid position '+name)
        return value
    if type(deck) is not int or deck not in SIDES:raise ValueError('position deck must be 1..4')
    if packet is None:return result
    if not isinstance(packet,dict) or packet.get('schema')!=1 or str(packet.get('pid'))!=pid:
        raise ValueError('position packet schema/process mismatch')
    if packet.get('status') not in ('ok','unavailable','stopped'):raise ValueError('unknown position publisher status')
    if packet['status']!='ok':return unknown('position publisher '+packet['status'])
    if binding is None:return unknown('position provider/header binding unavailable')
    for key in ('provider_build_id','header_sha256','engine_rate','abi_size'):
        if key not in binding or packet.get(key)!=binding[key]:raise ValueError('position binding mismatch '+key)
    if not isinstance(binding['provider_build_id'],str) or not re.fullmatch('[0-9a-f]{8,128}',binding['provider_build_id']) or not isinstance(binding['header_sha256'],str) or not re.fullmatch('[0-9a-f]{64}',binding['header_sha256']):
        raise ValueError('invalid position binding identity')
    rate=binding['engine_rate']
    if type(rate) is not int or not 1<=rate<2**31 or type(binding['abi_size']) is not int or not 40<=binding['abi_size']<=4096:
        raise ValueError('invalid position ABI/clock binding')
    if max_source_rate is not None and (type(max_source_rate) not in (int,float) or not math.isfinite(max_source_rate) or not 0<max_source_rate<=8):
        raise ValueError('invalid explicit source-rate bound')
    floor=unsigned(after_ns,'receipt barrier')
    latest=unsigned(packet.get('sequence'),'sequence')
    published=unsigned(packet.get('native_monotonic_ns'),'publisher clock',True)
    if now_boottime_ns is None or 'native_boottime_ns' not in packet:return unknown('independent position freshness clock unavailable')
    age=unsigned(now_boottime_ns,'current boottime')-unsigned(packet['native_boottime_ns'],'publisher boottime',True)
    # /proc/uptime truncates to centiseconds. It is CLOCK_BOOTTIME, which can
    # differ from CLOCK_MONOTONIC after suspend; never mix those two clocks.
    if compare_timestamps(age,-10000001)<0 or compare_timestamps(age,1000000000)>0:return unknown('position packet is stale or from the future')
    result.update(binding_verified=True,fresh=True)
    samples=packet.get('samples')
    if not isinstance(samples,list) or len(samples)>12:raise ValueError('bounded position history required')
    qualified=[];prior_sequence=prior_time=0
    for sample in samples:
        if not isinstance(sample,dict):raise ValueError('invalid position sample')
        sequence=unsigned(sample.get('sequence'),'sample sequence',True)
        ns=unsigned(sample.get('native_monotonic_ns'),'sample clock',True)
        if sequence<=prior_sequence or compare_timestamps(ns,prior_time)<=0 or sequence>latest or compare_timestamps(ns,published)>0:
            raise ValueError('nonmonotonic position history')
        prior_sequence,prior_time=sequence,ns
        records=sample.get('decks')
        if not isinstance(records,list) or len(records)!=4 or any(not isinstance(row,dict) for row in records) or [row.get('deck') for row in records]!=list(range(4)):
            raise ValueError('all four ordered position records required')
        row=records[deck-1]
        if compare_timestamps(ns,floor)<0:continue
        if row.get('read_rc')!=0 or row.get('loaded')!=1:return unknown('loaded position unavailable')
        frame,seconds,title=row.get('playhead'),row.get('pos_s'),row.get('title')
        if frame is None or seconds is None:return unknown('source position not observed')
        if type(frame) is not int or not 0<=frame<2**63 or type(seconds) not in (int,float) or not math.isfinite(seconds) or compare_timestamps(seconds,0.,units_per_second=1,sample_rate=rate)<0 or not isinstance(title,str) or len(title)>512:
            raise ValueError('invalid source position sample')
        if compare_timestamps(seconds,frame/rate,units_per_second=1,sample_rate=rate,window_samples=max(1,math.ceil(abs(seconds)*2.220446049250313e-16*rate)))!=0:raise ValueError('position disagrees with bound native source clock')
        qualified.append((ns,frame,seconds,title))
    if samples and prior_sequence!=latest:raise ValueError('latest position sequence missing')
    result.update(sequence=latest,engine_rate=rate,provider_build_id=binding['provider_build_id'],as_of_ns=published)
    if len(qualified)<5 or compare_timestamps(qualified[-1][0]-qualified[0][0],400000000)<0:return unknown('400ms of post-receipt source history required')
    gaps=[b[0]-a[0] for a,b in zip(qualified,qualified[1:])]
    if compare_timestamps(max(gaps),250000000)>0 or compare_timestamps(published-qualified[-1][0],250000000)>0:return unknown('position sampling gap exceeds 250ms')
    if len({row[3] for row in qualified})!=1:return unknown('track title changed within position history')
    result.update(window_start_ns=qualified[0][0],window_end_ns=qualified[-1][0],samples=len(qualified))
    bounds=None
    if loop and loop.get('engaged') in (1,'1') and loop.get('bounds') in (1,'1'):
        try:start,end=float(loop['in_s']),float(loop['out_s'])
        except (KeyError,TypeError,ValueError):raise ValueError('invalid loop context for position witness')
        if not all(map(math.isfinite,(start,end))) or not (compare_timestamps(start,0.,units_per_second=1,sample_rate=rate)>=0 and compare_timestamps(start,end,units_per_second=1,sample_rate=rate)<0):raise ValueError('invalid loop position range')
        bounds=(start,end)
    steps=0;quantum=1/rate
    for a,b in zip(qualified,qualified[1:]):
        delta=(b[1]-a[1])/rate;elapsed=(b[0]-a[0])/1e9
        if compare_timestamps(delta,0.,units_per_second=1,sample_rate=rate)<0:
            if bounds is None or not all(compare_timestamps(row[2],bounds[0],units_per_second=1,sample_rate=rate)>=0 and compare_timestamps(row[2],bounds[1],units_per_second=1,sample_rate=rate)<=0 for row in (a,b)):
                return unknown('unexplained backward seek/track transition')
            delta=(bounds[1]-a[2])+(b[2]-bounds[0])
        if compare_timestamps(delta,0.,units_per_second=1,sample_rate=rate)!=0:
            if max_source_rate is not None and compare_timestamps(delta,max_source_rate*elapsed,units_per_second=1,sample_rate=rate,window_samples=2)>0:return unknown('source-position jump exceeds explicit rate bound')
            steps+=1
    if steps>=2:
        result.update(state='MOVING',reason='multiple source-position advances or validated loop wraps');return result
    if steps:return unknown('one position jump cannot establish sustained movement')
    if bounds is not None and (max_source_rate is None or compare_timestamps(bounds[1]-bounds[0],max_source_rate*max(gaps)/1e9,units_per_second=1,sample_rate=rate,window_samples=2)<=0):
        return unknown('equal loop phase can alias moving playback')
    result.update(state='STATIONARY',reason='source position unchanged across the qualified observation window')
    return result

def renew_position_capture(read_snapshot,send_nonce,deck,pid,binding,*,clock=time.monotonic,sleep=time.sleep):
    """Begin a new bounded capture in the same opt-in producer, before a case."""
    initial=read_snapshot().get('positions')
    if not isinstance(initial,dict) or 'capture_nonce' not in initial:
        raise MissingEvidence('position producer does not support capture renewal')
    nonce=uuid.uuid4().hex;send_nonce(nonce);end=clock()+10
    while compare_timestamps(clock(),end,units_per_second=1)<0:
        snapshot=read_snapshot();packet=snapshot.get('positions')
        if isinstance(packet,dict) and packet.get('capture_nonce')==nonce:
            proof=position_motion(packet,deck,pid,binding,now_boottime_ns=snapshot.get('boottime_ns'))
            rows=packet.get('samples',[])
            if proof.get('binding_verified') and proof.get('fresh') and len(rows)>=5 and compare_timestamps(rows[-1]['native_monotonic_ns']-rows[0]['native_monotonic_ns'],400000000)>=0:
                return dict(capture_nonce=nonce,sequence=packet['sequence'])
        sleep(.1)
    raise MissingEvidence('new position capture was not acknowledged with fresh bound history')

def decode_observation(pid,heartbeat,uptime,loop_log,semantic=None,positions=None):
    """Validate existing heartbeat/probe formats, without inventing raw semantics."""
    if not isinstance(pid,str) or not re.fullmatch('[1-9][0-9]*',pid):raise ValueError('one pinned process required')
    if not isinstance(heartbeat,str) or not isinstance(loop_log,str) or len(heartbeat)>4096:
        raise ValueError('bounded textual heartbeat and transcript required')
    lines=heartbeat.strip().splitlines()
    if len(lines)!=4 or lines[0]!='azdecks 1' or lines[1]!='pid '+pid or not re.fullmatch('up [0-9]+(?:\\.[0-9]+)?',lines[2]) or not re.fullmatch('decks [ELU]{4}',lines[3]):
        raise ValueError('heartbeat identity/format mismatch')
    try:age=float(uptime)-float(lines[2].split()[1])
    except (TypeError,ValueError):raise ValueError('invalid uptime observation')
    if not math.isfinite(age) or compare_timestamps(age,0.,units_per_second=1)<0:raise ValueError('stale/future heartbeat')
    if compare_timestamps(age,5.,units_per_second=1)>0:raise StaleObservation('stale heartbeat')
    transcript=validate_transcript(loop_log)
    if transcript['status']=='FAIL':raise ValueError('invalid native transcript: '+','.join(transcript['errors']))
    headers=[_fields(line) for line in loop_log.splitlines() if line.startswith('pid=')]
    if len(headers)!=1 or headers[0].get('pid')!=pid:raise ValueError('probe process identity mismatch')
    if any(line.startswith('complete ') for line in loop_log.splitlines()):raise ValueError('operator capture already completed')
    # Native U means an unvalidated sample or the first zero TrackID during a
    # transition. It certifies neither loaded nor empty; wait for known state.
    if 'U' in lines[3][6:]:raise UnavailableObservation('unknown deck heartbeat state')
    decks=[{'loaded':value=='L','title':None,'playing':None,'streams':{}} for value in lines[3][6:]]
    for line in loop_log.splitlines():
        if not line.startswith(('deck=','live_deck=')):continue
        row=_fields(line);index=int(row.get('deck',row.get('live_deck')))
        stream='loop' if 'deck' in row else row['stream']
        decks[index]['streams'][stream]=row
    semantic={} if semantic is None else semantic
    if not isinstance(semantic,dict):raise ValueError('semantic witness mapping required')
    for key,source in [('titles','title_source'),('playing','playing_source')]:
        if key not in semantic:continue
        values=semantic[key]
        if not isinstance(values,(tuple,list)) or len(values)!=4 or not isinstance(semantic.get(source),str) or not 1<=len(semantic[source])<=128:
            raise ValueError('independent '+key+' witness/provenance required')
        for deck,value in zip(decks,values):
            if key=='playing':
                if value is not None and type(value) is not bool:raise ValueError('playing witness must be boolean or unavailable')
                deck['playing']=value
            else:
                if value is not None and (not isinstance(value,str) or len(value)>512):raise ValueError('invalid title witness')
                deck['title']=value
    return {'pid':pid,'mask':lines[3][6:],'decks':decks,'positions':positions,
            'boottime_ns':int(float(uptime)*1e9),'semantic_sources':
            {key:semantic[key] for key in ('title_source','playing_source') if key in semantic}}

class Controller:
    def __init__(self,driver_path,driver_sha256,output,owner_token,verify_owner,send_control,
                 qmp_execute=None,read_observation=None,pid=None,work_dir=None,
                 clock=time.monotonic,sleep=time.sleep,timeout=30,interval=0.2,
                 position_binding=None,max_source_rate=None,rate_bound_source=None):
        if not isinstance(owner_token,str) or not re.fullmatch('[0-9a-f]{32}',owner_token):raise ValueError('owned VM nonce required')
        if type(timeout) not in (int,float) or not math.isfinite(timeout) or not 0<timeout<=180 or type(interval) not in (int,float) or not math.isfinite(interval) or not 0<interval<=timeout:
            raise ValueError('bounded control timeout/interval required')
        self.owner_token=owner_token;self.verify_owner=verify_owner;self.send_control=send_control
        self.qmp_execute=qmp_execute;self.read_observation=read_observation;self.pid=pid
        if max_source_rate is not None and (type(max_source_rate) not in (int,float) or not math.isfinite(max_source_rate) or not 0<max_source_rate<=8 or not isinstance(rate_bound_source,str) or not rate_bound_source):
            raise ValueError('explicit source-rate bound needs independent provenance')
        self.position_binding=position_binding;self.max_source_rate=max_source_rate;self.rate_bound_source=rate_bound_source
        self.motion_after={};self.motion_evidence={}
        self.last_ack_frames={};self.panel_write_failed=False
        self.clock=clock;self.sleep=sleep;self.timeout=timeout;self.configured_control_timeout=timeout;self.interval=interval;self.last_clock=None
        self.work_dir=Path(work_dir) if work_dir is not None else None
        self.events=[];self.cleanup_errors=[];self.semantic_sources={};self.require_loaded=False;self.setup_attempts=set();self._owner()
        self.source_identity={'path':str(driver_path),'sha256':driver_sha256}
        self.output=Path(output)
        if self.output.exists() or self.output.is_symlink():raise ValueError('control output must be new')
        self.output.mkdir()
        self.driver=load_driver(driver_path,driver_sha256,self.output,self._send,clock,sleep)
    def _owner(self):
        if self.verify_owner()!=self.owner_token:raise ValueError('emulator ownership changed')
    def _now(self):
        now=self.clock()
        if type(now) not in (int,float) or not math.isfinite(now) or compare_timestamps(now,0.,units_per_second=1)<0 or (self.last_clock is not None and compare_timestamps(now,self.last_clock,units_per_second=1)<0):
            raise ValueError('invalid/backward control clock')
        self.last_clock=now;return now
    def _send(self,bus,frame):
        self._owner()
        if type(bus) is not int or not 0<=bus<=3 or len(frame)!=152 or any(type(x) is not int or not 0<=x<=255 for x in frame):
            raise ValueError('one complete native panel frame required')
        try:
            result=self.send_control(bytes([bus]+list(frame)))
            if result is False:raise RuntimeError('control transport rejected frame')
        except BaseException:
            self.panel_write_failed=True;raise
        self.last_ack_frames[bus]=bytes(frame)
    def panel_inputs_neutral(self):
        """Both used control buses must have acknowledged their neutral frames.

        This concerns only panel holds sent by this controller. It certifies no
        playback/paused state and never permits renewal after an ambiguous write.
        """
        if self.panel_write_failed:return False
        expected={self.driver.BUS_ERP:bytes(self.driver.ERP_IDLE),
                  self.driver.BUS_MIXER:bytes(self.driver._mixer())}
        return self.last_ack_frames==expected
    def _release(self,bus):
        frame=self.driver.ERP_IDLE if bus==self.driver.BUS_ERP else self.driver._mixer()
        self._send(bus,frame)
    def _press(self,side,name):
        try:self.driver.press(side,name,hold=0.15)
        finally:self._release(self.driver.BUS_ERP)
    def select(self,deck):
        if type(deck) is not int or deck not in SIDES:raise ValueError('logical deck must be 1..4')
        side,name=SIDES[deck];self._press(side,name)
        return side
    def button(self,deck,name):
        if name not in BUTTONS:raise ValueError('unsupported observed panel button')
        side=self.select(deck)
        self.events.append({'action':name,'deck':deck})
        self._press(side,name)
    def quantize(self):
        self.events.append({'action':'quantize','scope':'unit input; affected decks require observations'})
        self._press(1,'quantize')
    def load(self,deck):
        if type(deck) is not int or deck not in SIDES:raise ValueError('logical deck must be 1..4')
        self.events.append({'action':'load','deck':deck})
        # A mixer write is only a one/two-frame pulse. Reuse the driver's
        # frame builder and its existing 20ms hold cadence so a missed first
        # native sample does not silently lose LOAD.
        try:
            frame=self.driver._mixer(33,self.driver.LOAD_MASK[deck],True)
            end=self._now()+0.15;sent=0
            # Ownership/birth checks and the receiver acknowledgement can take
            # longer than the hold window. Still deliver the repeated frame
            # promised above when the first native pulse was not sampled.
            while sent<2 or compare_timestamps(self._now(),end,units_per_second=1)<0:
                self._send(self.driver.BUS_MIXER,frame)
                sent+=1
                self.sleep(.02)
        finally:self._release(self.driver.BUS_MIXER)
    def _qmp(self,command):
        self._owner()
        if self.qmp_execute is None:raise MissingEvidence('owned QMP connection unavailable')
        result=self.qmp_execute(command)
        if not isinstance(result,dict) or 'error' in result or 'return' not in result:raise RuntimeError('QMP command failed')
        return result['return']
    def tap_pixel(self,x,y):
        if any(type(v) not in (int,float) or not math.isfinite(v) for v in (x,y)) or not 0<=x<1280 or not 0<=y<800:
            raise ValueError('touch must use 1280x800 screenshot pixels')
        events=[{'type':'abs','data':{'axis':'x','value':round(x/1280*0x7fff)}},
                {'type':'abs','data':{'axis':'y','value':round(y/800*0x7fff)}},
                {'type':'btn','data':{'button':'left','down':True}}]
        try:
            self._qmp({'execute':'input-send-event','arguments':{'events':events}})
            self.sleep(0.08)
        finally:self._qmp({'execute':'input-send-event','arguments':{'events':[{'type':'btn','data':{'button':'left','down':False}}]}})
    def screenshot(self,name):
        if not isinstance(name,str) or not re.fullmatch('[a-z0-9][a-z0-9-]{0,63}',name):raise ValueError('safe screenshot name required')
        if self.work_dir is None:raise MissingEvidence('owned QEMU work directory unavailable')
        basename='sdk-controls-'+name+'.ppm';destination=self.output/(name+'.ppm')
        if destination.exists():raise ValueError('screenshot evidence must be new')
        self._qmp({'execute':'screendump','arguments':{'filename':'/work/'+basename}})
        source=self.work_dir/basename
        if source.is_symlink() or not source.is_file() or source.stat().st_size>4*1024*1024:raise MissingEvidence('QMP screenshot artifact unavailable')
        shutil.copyfile(str(source),str(destination));return str(destination)
    def observe(self):
        self._owner()
        if self.read_observation is None:raise MissingEvidence('pinned observation reader unavailable')
        raw=self.read_observation()
        if not isinstance(raw,dict) or set(raw)-{'pid','heartbeat','uptime','loop_log','semantic','positions'} or not {'pid','heartbeat','uptime','loop_log'}<=set(raw):
            raise ValueError('complete pinned observation mapping required')
        if raw['pid']!=self.pid:raise ValueError('control process identity changed')
        value=decode_observation(**raw)
        if value['pid']!=self.pid:raise ValueError('control process identity changed')
        if self.require_loaded and value['mask']!='LLLL':raise ValueError('loaded decks changed during control capture')
        if value['positions'] is not None:
            for deck in range(1,5):
                motion=position_motion(value['positions'],deck,self.pid,self.position_binding,
                    now_boottime_ns=value['boottime_ns'])
                if motion.get('binding_verified') and motion.get('fresh'):
                    samples=value['positions']['samples']
                    if samples:
                        row=samples[-1]['decks'][deck-1]
                        if row.get('read_rc')==0 and row.get('loaded')==1 and row.get('title'):
                            if value['decks'][deck-1]['title'] is None:value['decks'][deck-1]['title']=row['title']
                            value['semantic_sources']['title_source']='azstem_deck_snapshot copied title'
        for key,source in value['semantic_sources'].items():
            self.semantic_sources.setdefault(key,set()).add(source)
        return value
    def wait(self,predicate,label,allow_partial=False,unavailable_on_timeout=False):
        start=self._now();stable=0;value=None;stale=None;late=False
        for _ in range(int(self.timeout/self.interval)+3):
            if compare_timestamps(self._now()-start,self.timeout,units_per_second=1)>=0:break
            try:
                candidate=self.observe()
            except UnavailableObservation as error:
                stable=0;stale=str(error)
                self.events.append({'action':'observation_rejected','reason':stale})
            else:
                stale=None
                # Read/transport time is part of the advertised wait budget.
                # An old positive result cannot close a test after its deadline.
                if compare_timestamps(self._now()-start,self.timeout,units_per_second=1)>=0:
                    late=True;break
                value=candidate
                stable=stable+1 if predicate(value) else 0
                if stable>=2:return value
            if compare_timestamps(self._now()-start,self.timeout,units_per_second=1)>=0:break
            self.sleep(self.interval)
        if late:raise MissingEvidence('observation exceeded wait budget: '+label)
        if stale is not None:raise MissingEvidence('fresh observation unavailable: '+label+': '+stale)
        if allow_partial:return value
        if unavailable_on_timeout:raise MissingEvidence('qualified observation unavailable within wait budget: '+label)
        raise RuntimeError('unobserved transition: '+label)
    @staticmethod
    def _stream(snapshot,deck,name):
        value=snapshot['decks'][deck-1]['streams'].get(name)
        if not value or (name!='loop' and value.get('valid')!='1'):raise MissingEvidence('deck%d %s stream unavailable'%(deck,name))
        return value
    @staticmethod
    def _newer(before,after):
        if before['unit']!=after['unit']:raise ValueError('native unit changed during panel transition')
        return int(after['revision'])>int(before['revision'])
    def establish_live_receipt(self,deck,stream):
        """Explicit observation setup, separate from any known-state roundtrip.

        Native registration does not replay a default. A missing stream may be
        stimulated once; its first genuine value is recorded without guessing
        the initial state. A failed setup is never retried by this controller.
        """
        if type(deck) is not int or deck not in SIDES or stream not in ('play','key_lock'):
            raise ValueError('observed setup needs one logical deck and admitted live stream')
        self._owner()
        key=(deck,stream)
        if key in self.setup_attempts:raise MissingEvidence('deck%d %s observation setup already attempted; no retry'%(deck,stream))
        before=self.observe()
        if not self.require_loaded or before['mask']!='LLLL':raise MissingEvidence('all four loaded decks required before observation setup')
        unit=self._stream(before,deck,'quantize')['unit']
        def native_now(snapshot):
            now=self._native_now(snapshot,deck)
            if now is None:raise MissingEvidence('fresh bound native clock required for observation setup')
            return now
        fence=native_now(before)
        try:existing=self._stream(before,deck,stream)
        except MissingEvidence:existing=None
        if existing is not None:
            if existing['unit']!=unit:raise ValueError('native unit mismatch before observation setup')
            if compare_timestamps(int(existing['received_ns']), fence+10000000) > 0:raise ValueError('future receipt before observation setup')
            return {'deck':deck,'stream':stream,'input_sent':False,'initial_value':None,'receipt':dict(existing)}
        self.setup_attempts.add(key)
        self.events.append({'action':'observation_setup_input','deck':deck,'stream':stream,'before_ns':fence,
                            'scope':'obtain first receipt; no initial value or transition inferred'})
        self.button(deck,'play' if stream=='play' else 'mastertempo')
        def received(snapshot):
            try:row=self._stream(snapshot,deck,stream)
            except MissingEvidence:return False
            if row['unit']!=unit:raise ValueError('native unit changed during observation setup')
            now=self._native_now(snapshot,deck)
            if now is None:return False
            stamp=int(row['received_ns'])
            if compare_timestamps(stamp,now+10000000)>0:raise ValueError('future receipt during observation setup')
            return int(row['revision'])>0 and compare_timestamps(fence,stamp)<0 and compare_timestamps(stamp,now+10000000)<=0
        result=self.wait(received,'deck%d first %s receipt after one setup input'%(deck,stream),unavailable_on_timeout=True)
        if result is None or not received(result):
            raise MissingEvidence('deck%d %s receipt unavailable after one observation setup input'%(deck,stream))
        row=self._stream(result,deck,stream)
        self.events.append({'action':'observation_setup_receipt','deck':deck,'stream':stream,
                            'unit':int(row['unit']),'revision':int(row['revision']),
                            'received_ns':int(row['received_ns']),'before_ns':fence})
        return {'deck':deck,'stream':stream,'input_sent':True,'initial_value':None,'receipt':dict(row)}
    def ensure_playing(self,deck,want):
        """Observe transport or source-position movement/stationarity, not raw flags."""
        before=self.observe()
        current=self._transport(before,deck)
        if current is None and before['positions'] is not None:
            before=self.wait(lambda s:self._transport(s,deck) is not None,'qualified source-position witness',allow_partial=True)
            current=self._transport(before,deck)
        if current is None:raise MissingEvidence('independent transport/position witness unavailable for deck%d'%deck)
        if current is want:return before
        raw=self._stream(before,deck,'play')
        fence=self._native_now(before,deck)
        if before['positions'] is not None and fence is None:
            raise MissingEvidence('fresh bound native clock required before play input')
        if fence is not None and compare_timestamps(int(raw['received_ns']), fence+10000000) > 0:
            raise ValueError('future receipt before play input')
        self.button(deck,'play')
        # Receipt and motion use the same observations and the same finite wait.
        # Starting a second wait discards qualified post-input windows already
        # captured while waiting for the receipt. No input is retried.
        result=self.wait(lambda s:self._play_motion_transition(s,deck,raw,want,fence),
                         'deck%d play receipt and transport-position moving=%s'%(deck,want),
                         unavailable_on_timeout=True)
        row=self._stream(result,deck,'play')
        self.events.append({'action':'transport_observed','deck':deck,'moving':want,
                            'unit':int(row['unit']),'revision':int(row['revision']),
                            'receipt_ns':int(row['received_ns']),'before_ns':fence,
                            'position_evidence':self.motion_evidence.get(deck)})
        return result
    def _native_now(self,snapshot,deck):
        packet=snapshot['positions']
        proof=position_motion(packet,deck,self.pid,self.position_binding,
                              now_boottime_ns=snapshot['boottime_ns'])
        if not proof.get('binding_verified') or not proof.get('fresh'):return None
        return packet['native_monotonic_ns']+snapshot['boottime_ns']-packet['native_boottime_ns']
    def _play_motion_transition(self,snapshot,deck,before,want,fence):
        row=self._stream(snapshot,deck,'play')
        newer=self._newer(before,row) # Unit changes fail even before a new receipt.
        stamp=int(row['received_ns'])
        if fence is not None:
            now=self._native_now(snapshot,deck)
            if now is None:return False
            if compare_timestamps(stamp,now+10000000)>0:raise ValueError('future receipt during play transition')
            if compare_timestamps(stamp,fence)<=0:return False
        if not newer or compare_timestamps(stamp, int(before['received_ns'])) <= 0:return False
        self.motion_after[deck]=max(self.motion_after.get(deck,0),stamp)
        return self._transport(snapshot,deck) is want
    def _transport(self,snapshot,deck):
        direct=snapshot['decks'][deck-1]['playing']
        if direct is not None:return direct
        row=snapshot['decks'][deck-1]['streams'].get('loop')
        motion=position_motion(snapshot['positions'],deck,self.pid,self.position_binding,
            after_ns=self.motion_after.get(deck,0),loop=row,max_source_rate=self.max_source_rate,
            now_boottime_ns=snapshot['boottime_ns'])
        self.motion_evidence[deck]=motion
        if motion['state']=='MOVING':return True
        if motion['state']=='STATIONARY':return False
        return None
    def raw_play_roundtrip(self,deck):
        before=self._stream(self.observe(),deck,'play')
        keys=('mode_is_2','play_pause','play_aux');initial=tuple(before[k] for k in keys)
        self.button(deck,'play')
        changed=self.wait(lambda s:self._newer(before,self._stream(s,deck,'play')) and
                          tuple(self._stream(s,deck,'play')[k] for k in keys)!=initial,'raw play notification change')
        middle=self._stream(changed,deck,'play');middle_values=tuple(middle[k] for k in keys)
        self.button(deck,'play')
        second=self.wait(lambda s:self._newer(middle,self._stream(s,deck,'play')) and
                  tuple(self._stream(s,deck,'play')[k] for k in keys)!=middle_values,'second raw play notification change')
        self.motion_after[deck]=int(self._stream(second,deck,'play')['received_ns'])
    def key_lock_roundtrip(self,deck):
        before=self._stream(self.observe(),deck,'key_lock');initial=before['key_lock']
        self.button(deck,'mastertempo')
        changed=self.wait(lambda s:self._newer(before,self._stream(s,deck,'key_lock')) and
                          self._stream(s,deck,'key_lock')['key_lock']!=initial,'key lock change')
        middle=self._stream(changed,deck,'key_lock');self.button(deck,'mastertempo')
        self.wait(lambda s:self._newer(middle,self._stream(s,deck,'key_lock')) and
                  self._stream(s,deck,'key_lock')['key_lock']==initial,'key lock restore')
    def manual_loop(self,deck):
        if type(deck) is not int or deck not in SIDES:raise ValueError('logical deck must be 1..4')
        # Paused-loop stationarity cannot exclude equal-phase playback without
        # this independent bound. Refuse before changing transport or loop state.
        if self.max_source_rate is None and self.observe()['decks'][deck-1]['playing'] is None:
            raise MissingEvidence('deck%d manual loop pause needs an independent source rate bound'%deck)
        self.ensure_playing(deck,True)
        before=self._stream(self.observe(),deck,'loop')
        self.button(deck,'loopin');self.sleep(0.8);self.button(deck,'loopout')
        looped=self.wait(lambda s:self._newer(before,self._stream(s,deck,'loop')) and
                         self._stream(s,deck,'loop')['engaged']=='1' and self._stream(s,deck,'loop')['bounds']=='1','manual loop bounds')
        self.ensure_playing(deck,False)
        before=self._stream(looped,deck,'loop');self.button(deck,'reloop')
        exited=self._wait_loop_transition(deck,before,'0',False,'paused EXIT')
        before=self._stream(exited,deck,'loop');self.button(deck,'reloop')
        relooped=self._wait_loop_transition(deck,before,'1',True,'paused RELOOP starts source-position movement')
        self.ensure_playing(deck,False)
        before=self._stream(relooped,deck,'loop');self.button(deck,'reloop')
        self._wait_loop_transition(deck,before,'0',False,'final paused EXIT')
    def _wait_loop_transition(self,deck,before,engaged,moving,label):
        value=self.wait(lambda s:self._loop_transition(s,deck,before,engaged,moving),label,allow_partial=True)
        row=self._stream(value,deck,'loop')
        if not self._newer(before,row) or row['engaged']!=engaged:raise RuntimeError('unobserved transition: '+label)
        observed=self._transport(value,deck)
        if observed is None:raise MissingEvidence('ambiguous source-position witness after '+label)
        if observed is not moving:raise RuntimeError('unobserved transition: '+label)
        self.events.append({'action':'loop_transition_observed','deck':deck,'engaged':engaged,'moving':moving,
                            'receipt_ns':int(row['received_ns']),'position_evidence':self.motion_evidence.get(deck)})
        return value
    def _loop_transition(self,snapshot,deck,before,engaged,moving):
        row=self._stream(snapshot,deck,'loop')
        if not self._newer(before,row) or row['engaged']!=engaged:return False
        self.motion_after[deck]=max(self.motion_after.get(deck,0),int(row['received_ns']))
        return self._transport(snapshot,deck) is moving
    def quantize_roundtrip(self):
        before=self.observe();rows=[self._stream(before,d,'quantize') for d in range(1,5)]
        self.quantize()
        changed=self.wait(lambda s:all(self._newer(rows[d-1],self._stream(s,d,'quantize')) and
                            self._stream(s,d,'quantize')['quantize']!=rows[d-1]['quantize'] for d in range(1,5)),'all-deck quantize change',allow_partial=True)
        observed=[d for d in range(1,5) if self._newer(rows[d-1],self._stream(changed,d,'quantize')) and self._stream(changed,d,'quantize')['quantize']!=rows[d-1]['quantize']]
        if not observed:raise RuntimeError('unobserved transition: global quantize change')
        middle=[self._stream(changed,d,'quantize') for d in range(1,5)]
        self.quantize()
        self.wait(lambda s:all(self._newer(middle[d-1],self._stream(s,d,'quantize')) and
                       self._stream(s,d,'quantize')['quantize']==rows[d-1]['quantize'] for d in observed),'global quantize restore')
        return observed
    def run_selected_track(self,prepare_track,expected_titles,prepare_deck_load=None,prepare_controls=None):
        """Load an independently selected test track, then exercise all four slots."""
        control_timeout=self.configured_control_timeout
        report={'status':'INCOMPLETE','scope':SCOPE,'decks':{str(d):{} for d in range(1,5)},
                'missing':[],'errors':[],'excluded':['beat-grid quantization','saved-memory persistence','native SDK commands','audio output','source-qualified load binding']}
        report['wait_budgets']={'control_seconds':control_timeout,'loads':{}}
        def check(deck,name,operation):
            try:operation();report['decks'][str(deck)][name]='PASS'
            except MissingEvidence as error:
                report['decks'][str(deck)][name]='INCOMPLETE';report['missing'].append(str(error))
        try:
            if not isinstance(expected_titles,(list,tuple)) or not 1<=len(expected_titles)<=8 or any(not isinstance(title,str) or not 1<=len(title)<=512 for title in expected_titles):
                raise ValueError('explicit exact expected title aliases required')
            if prepare_deck_load is not None and not callable(prepare_deck_load):
                raise ValueError('bounded per-deck selection callback required')
            if prepare_controls is not None and not callable(prepare_controls):
                raise ValueError('bounded explicit observation setup callback required')
            seen_evidence=set()
            def selection(value,deck):
                if (not isinstance(value,dict) or value.get('status')!='PASS' or
                    value.get('selected_title') not in expected_titles or
                    not isinstance(value.get('evidence'),str) or not 1<=len(value['evidence'])<=4096 or
                    value['evidence'] in seen_evidence):
                    raise MissingEvidence('deck%d current track selection UI evidence unavailable'%deck)
                seen_evidence.add(value['evidence'])
                return dict(value)
            selected=selection(prepare_track(self),1)
            report['selection']=selected
            report['load_selections']={}
            for deck in range(1,5):
                current=selected
                if deck>1:
                    if prepare_deck_load is None:
                        raise MissingEvidence('deck%d current track selection callback unavailable'%deck)
                    current=selection(prepare_deck_load(self,deck,selected),deck)
                report['load_selections'][str(deck)]=current
                self.events.append({'action':'selection_observed','deck':deck,'evidence':current['evidence'],'local_monotonic':self._now()})
                report['wait_budgets']['loads'][str(deck)]=self.timeout
                self.load(deck)
                state=self.wait(lambda s:s['decks'][deck-1]['loaded'],'deck%d loaded'%deck)
                report['decks'][str(deck)]['loaded_heartbeat']='PASS'
                state=self.wait(lambda s:s['decks'][deck-1]['loaded'] and s['decks'][deck-1]['title'] in expected_titles,
                                'deck%d loaded title'%deck,allow_partial=True)
                title=state['decks'][deck-1]['title']
                if title is None:
                    report['decks'][str(deck)]['loaded_title']='INCOMPLETE';report['missing'].append('deck%d loaded title witness unavailable'%deck)
                else:
                    if title not in expected_titles:raise RuntimeError('unexpected loaded title on deck%d'%deck)
                    report['decks'][str(deck)]['loaded_title']='PASS'
            # GUI preparation can request the existing longer LOAD wait. The
            # independently configured receipt/motion budget must not inherit it.
            self.timeout=control_timeout
            self.require_loaded=True
            if prepare_controls is not None:
                report['observation_setup']=prepare_controls(self)
                if not isinstance(report['observation_setup'],dict) or report['observation_setup'].get('status')!='PASS':
                    raise MissingEvidence('explicit live observation setup incomplete')
            for deck in range(1,5):
                check(deck,'raw_play_notifications',lambda d=deck:self.raw_play_roundtrip(d))
                check(deck,'key_lock',lambda d=deck:self.key_lock_roundtrip(d))
            quantized=self.quantize_roundtrip()
            for deck in range(1,5):
                report['decks'][str(deck)]['quantize_state']='PASS' if deck in quantized else 'INCOMPLETE'
                if deck not in quantized:report['missing'].append('deck%d quantize transition unavailable'%deck)
            # Finish the available setting/receipt checks on all four decks
            # before spending the bounded waits on independent motion evidence.
            # Transport and paused-loop requirements retain their own guards.
            for deck in range(1,5):
                def transport(d=deck):
                    self.ensure_playing(d,False);self.ensure_playing(d,True);self.ensure_playing(d,False)
                check(deck,'play_pause',transport)
                check(deck,'manual_loop_paused_exit_reloop',lambda d=deck:self.manual_loop(d))
        except MissingEvidence as error:report['missing'].append(str(error))
        except (RuntimeError,ValueError,OSError) as error:report['errors'].append(str(error))
        finally:
            self.timeout=control_timeout
            for bus in (self.driver.BUS_ERP,self.driver.BUS_MIXER):
                try:self._release(bus)
                except (RuntimeError,ValueError,OSError) as error:self.cleanup_errors.append(str(error))
            report['errors'].extend(self.cleanup_errors)
            report['events']=self.events
            report['owner_nonce']=self.owner_token;report['pid']=self.pid
            report['panel_source']=self.source_identity
            report['semantic_sources']={key:sorted(value) for key,value in self.semantic_sources.items()}
            report['position_binding']=self.position_binding
            report['source_rate_bound']={'maximum':self.max_source_rate,'provenance':self.rate_bound_source}
            report['position_witness_scope']='source-position movement/stationarity in qualified receipt windows; position ABI carries no native unit token; no native paused flag semantics'
            report['status']='FAIL' if report['errors'] else 'INCOMPLETE' if report['missing'] or any(len(v)<7 for v in report['decks'].values()) else 'PASS'
            (self.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        return report
