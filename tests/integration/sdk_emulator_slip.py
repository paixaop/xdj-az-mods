#!/usr/bin/env python3
"""One raw Slip callback experiment; labels come from the documented controls.

No public Slip projection, receiver dereference, logical-deck or active-state
semantics are established by these callback correlations.
"""
from sdk_timestamps import compare_timestamps
import re
from sdk_emulator_controls import MissingEvidence


def parse_capture(text,binding,now_ns):
    if not isinstance(text,str) or len(text.encode())>262144:raise ValueError('bounded raw Slip log required')
    if set(binding)!={'profile','target','caller'} or binding['profile'] not in ('1.30','2.00') or any(type(binding[k]) is not int or not 0<binding[k]<2**64 for k in ('target','caller')):
        raise ValueError('exact raw Slip code binding required')
    if type(now_ns) is not int or not 0<now_ns<2**64:raise ValueError('bound native clock required')
    ready=None;records=[];stats=None;prior_ns=0;counts=(0,0,0,0,0)
    for line in text.splitlines():
        if not line.startswith('SDK_SLIP_CAPTURE'):continue
        parts=line.split();kind=parts[0];pairs=[p.split('=',1) for p in parts[1:]]
        if len(line)>512 or any(len(p)!=2 for p in pairs) or len({p[0] for p in pairs})!=len(pairs):raise ValueError('exact raw Slip fields required')
        row=dict(pairs)
        def number(name,maximum=2**64-1,positive=False,hexadecimal=False):
            value=row[name];pattern=r'0x[0-9a-f]+' if hexadecimal else r'0|[1-9][0-9]*'
            if not re.fullmatch(pattern,value):raise ValueError('canonical raw Slip integer required')
            n=int(value,16 if hexadecimal else 10)
            if not int(positive)<=n<=maximum:raise ValueError('raw Slip integer range exceeded')
            row[name]=n;return n
        if kind=='SDK_SLIP_CAPTURE_READY':
            if ready is not None or set(row)!={'v','profile','target','caller','max_records','ring','semantics'}:raise ValueError('unique exact Slip READY required')
            for k in ('v','max_records','ring'):number(k)
            for k in ('target','caller'):number(k,positive=True,hexadecimal=True)
            if row!=dict(v=1,**binding,max_records=512,ring=128,semantics='raw_no_deck'):raise ValueError('raw Slip READY binding mismatch')
            ready=row
        elif kind=='SDK_SLIP_CAPTURE':
            if set(row)!={'v','seq','observed_ns','raw','receiver','caller'}:raise ValueError('exact Slip callback record required')
            for k in ('v','seq','observed_ns'):number(k,positive=True)
            number('raw',2**32-1)
            for k in ('receiver','caller'):number(k,positive=True,hexadecimal=True)
            if row['v']!=1 or row['seq']!=len(records)+1 or row['seq']>512 or row['caller']!=binding['caller'] or compare_timestamps(row['observed_ns'], prior_ns) < 0 or compare_timestamps(row['observed_ns'], now_ns + 10000000) > 0:
                raise ValueError('raw Slip sequence/caller/clock mismatch')
            prior_ns=row['observed_ns'];records.append(row)
        elif kind=='SDK_SLIP_CAPTURE_STATS':
            if set(row)!={'v','saved','emitted','dropped','rejected','missed','reason'}:raise ValueError('exact Slip capture stats required')
            for k in ('v','saved','emitted','dropped','rejected','missed'):number(k)
            current=tuple(row[k] for k in ('saved','emitted','dropped','rejected','missed'))
            if row['v']!=1 or row['saved']>512 or row['emitted']>row['saved'] or row['emitted']!=len(records) or any(a<b for a,b in zip(current,counts)) or row['reason'] not in ('running','record_limit','time_limit','install_failed','sleep'):
                raise ValueError('raw Slip stats mismatch')
            counts=current;stats=row
        else:raise ValueError('unknown Slip capture record')
    if ready is None or stats is None:raise MissingEvidence('raw Slip READY/stats unavailable')
    if any(stats[k] for k in ('dropped','rejected','missed')) or stats['reason'] in ('install_failed','sleep'):
        raise MissingEvidence('raw Slip capture refused or lost events')
    return dict(ready=ready,records=records,stats=stats,native_now_ns=now_ns)


def run_slip_trial(controller,prepare_track,expected_titles,read_capture,verify_process,deadline,ready_for_slip=None):
    """Actual one-deck LOAD then two distinct presses; never retry a failed input."""
    configured=controller.configured_control_timeout
    def guard():
        controller._owner()
        if compare_timestamps(controller._now(), deadline, units_per_second=1) >= 0:raise MissingEvidence('raw Slip overall deadline expired')
        if verify_process() is not True:raise ValueError('pinned native identity refused')
    def read():
        guard();value=read_capture();guard();return value
    def phase_ui(label):
        result={};guard()
        try:result['screenshot']=controller.screenshot('slip-'+label.lower().replace('_','-'))
        except (MissingEvidence,OSError,RuntimeError) as error:
            # Supporting artifacts may be missing; process/owner/deadline
            # failures remain fatal before another input or candidate read.
            guard();result['screenshot_missing']=str(error)[:256]
        return result
    def stage(label,fence,required,ui=None):
        end=min(deadline,controller._now()+configured)
        while True:
            value=read();new=[r for r in value['records'] if compare_timestamps(r['observed_ns'], fence) > 0]
            # A worker stats receipt must cover these emitted records. The
            # regular10s stats cadence fits the unchanged30s control bound.
            covered=value['stats']['emitted']>=len(value['records'])
            if (new or not required) and covered:
                result=dict(label=label,records=new,native_read_ns=value['native_now_ns'])
                result.update(phase_ui(label) if ui is None else ui)
                return result
            if compare_timestamps(controller._now(), end, units_per_second=1) >= 0:raise MissingEvidence('raw Slip callback unavailable after '+label)
            controller.sleep(min(controller.interval,max(0,end-controller._now())))
    try:
        guard();selected=prepare_track(controller);guard()
        if not isinstance(selected,dict) or selected.get('status')!='PASS' or selected.get('selected_title') not in expected_titles or not selected.get('evidence'):
            raise MissingEvidence('current native track selection required')
        before=read();controller.load(1)
        controller.wait(lambda s:s['decks'][0]['loaded'] and s['decks'][0]['title'] in expected_titles,'deck1 loaded title')
        controller.timeout=configured
        stages=[stage('documented_OFF_after_LOAD',before['native_now_ns'],False)]
        for label in ('documented_ON_after_SLIP','documented_OFF_after_SLIP'):
            before=read();guard()
            if not callable(ready_for_slip) or ready_for_slip(controller) is not True:
                raise MissingEvidence('independent completed-load/Slip readiness unavailable')
            guard();controller.button(1,'slip');guard()
            ui=phase_ui(label)
            stages.append(stage(label,before['native_now_ns'],True,ui=ui))
        return dict(status='PASS',scope='one documented native control sequence and raw callbacks; no enabled/deck/active projection',stages=stages)
    finally:controller.timeout=configured
