#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Strict parsing of normal-loader public SDK setting observations.

These records describe enabled settings, with independent clocks. They supply
no transport, paused, source/load association or native-command evidence.
"""
from sdk_timestamps import compare_timestamps
import hashlib
import io
import json
import re
import zipfile
from sdk_emulator_controls import MissingEvidence,StaleObservation
from sdk_emulator_loaded import native_birth_check_script

FIELDS={'v','seq','deck','returned','query_ns','rc','valid','mt','mt_unit','mt_rev','mt_ns',
        'sync','sync_unit','sync_rev','sync_ns','raw'}
STREAMS={'mt':(256,'mt','mastertempo'),'sync':(512,'sync','sync')}
PROBE_IDS={'sdk-state-probe','sdk-slip-capture','sdk-loop-command-probe','sdk-cue-command-probe'}
SLIP_FIRMWARE=['ep147-79d43e5de779fc1b','ep147-9e9454e357389ece']

def read_script(pid,birth,body,cap):
    if type(cap) is not int or not 1<=cap<=1048576 or not isinstance(body,str) or len(body)>8192 or '\0' in body:
        raise ValueError('bounded owned read required')
    identity=native_birth_check_script(pid,birth)
    guard='test "$(check_public_native)" = AZSDK_NATIVE_BIRTH_MATCH || exit 93\n'
    return ('AZSDK_PUBLIC_READ=1\nset -e\ncheck_public_native() {\n'+identity+'}\n'+guard+
            '{\n'+body+'\n} | head -c '+str(cap+1)+'\n'+guard)

def stage_script(token,mod_id='sdk-state-probe'):
    """Normal RAM staging only; the UI must make its own approval decision."""
    if not isinstance(token,str) or not re.fullmatch('[a-f0-9]{32}',token):raise ValueError('owned staging nonce required')
    if mod_id not in PROBE_IDS:raise ValueError('reviewed normal probe id required')
    root='/home/root/settings/az-usbload/community'
    target=root+'/'+mod_id;stage=root+'/.sdk-probe-'+token
    return ('set -e\numask 077\ntest ! -e '+target+'\nmkdir -p '+root+'\nmkdir '+stage+'\n'
            'tar -xf - -C '+stage+'\nmv '+stage+' '+target+'\n'
            'test -f '+root+'/enabled || : >'+root+'/enabled\n'
            'grep -qx '+mod_id+' '+root+'/enabled || printf \'%s\\n\' '+mod_id+' >>'+root+'/enabled\n'
            'touch /home/root/settings/azmod-logs.on\nsha256sum '+
            ' '.join(target+'/'+name for name in ('azmod.json',mod_id+'.so','LICENSE','icon.png'))+'\n')

def approved(text,mod_id,sha):
    if not isinstance(text,str) or len(text.encode())>32768 or mod_id not in PROBE_IDS or not re.fullmatch('[0-9a-f]{64}',sha):
        raise ValueError('bounded exact public probe approval read required')
    return any(line.split()==[mod_id,sha] for line in text.splitlines())

def new_process(before,after):
    for value in (before,after):
        if not isinstance(value,dict) or any(not isinstance(value.get(k),str) or not re.fullmatch('[1-9][0-9]{0,19}',value[k]) for k in ('pid','birth')):
            raise ValueError('pinned reload process identities required')
    if before['pid']==after['pid'] or before['birth']==after['birth']:
        raise ValueError('reload requires a new pinned native PID and birth')
    return True

def probe_archive(data,binding,mod_id='sdk-state-probe'):
    """Validate the handed-off package; normal loader/Allow still owns trust."""
    if type(data) is not bytes or len(data)>1048576 or hashlib.sha256(data).hexdigest()!=binding['package']['sha256']:
        raise ValueError('public probe package hash/size mismatch')
    if mod_id not in PROBE_IDS:raise ValueError('reviewed normal probe id required')
    names={'azmod.json',mod_id+'.so','LICENSE','icon.png'}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries=archive.infolist()
        if len(entries)!=4 or {e.filename for e in entries}!=names or any(e.file_size>262144 or e.is_dir() or (e.external_attr>>16)&0o170000==0o120000 for e in entries):
            raise ValueError('exact normal-loader probe package files required')
        files={e.filename:archive.read(e) for e in entries}
    if hashlib.sha256(files[mod_id+'.so']).hexdigest()!=binding['binary']['sha256']:
        raise ValueError('public probe binary hash mismatch')
    metadata=json.loads(files['azmod.json'])
    if (metadata.get('id'),metadata.get('api'),metadata.get('hooks'),metadata.get('firmware'))!=(mod_id,1,mod_id=='sdk-slip-capture',SLIP_FIRMWARE if mod_id=='sdk-slip-capture' else ['any']) or metadata.get('version') not in (('0.1.0','0.1.1') if mod_id=='sdk-loop-command-probe' else ('0.1.0',)):
        raise ValueError('reviewed public probe identity required')
    return files

def setting(row,stream):
    if stream not in STREAMS:raise ValueError('public MT or Sync setting required')
    bit,key,_=STREAMS[stream]
    if row['rc']!=0 or not row['valid']&bit:raise MissingEvidence('public '+stream+' setting unavailable')
    value=dict(value=row[key],unit=row[key+'_unit'],revision=row[key+'_rev'],received_ns=row[key+'_ns'])
    if value['value'] not in (0,1) or not all(0<value[k]<2**64 for k in ('unit','revision','received_ns')):
        raise ValueError('invalid public '+stream+' value/receipt')
    if compare_timestamps(value['received_ns'], row['query_ns']) > 0:raise ValueError('public receipt exceeds its API query clock')
    if stream=='sync' and row['raw']!=(2 if value['value'] else 0):raise ValueError('public Sync/raw mode disagreement')
    return value

def public_rows(text,now_ns):
    if not isinstance(text,str) or len(text.encode())>262144:raise ValueError('bounded public probe log required')
    if type(now_ns) is not int or not 0<now_ns<2**64:raise ValueError('bound native monotonic clock required')
    latest={};sequence=0;prior_query=0
    for line in text.splitlines():
        if not line.startswith('SDK_PUBLIC_STATE v='):continue
        parts=line.split()[1:];pairs=[x.split('=',1) for x in parts]
        if any(len(x)!=2 for x in pairs) or len({x[0] for x in pairs})!=len(pairs) or {x[0] for x in pairs}!=FIELDS:
            raise ValueError('exact public record fields required')
        if any(not re.fullmatch('-?[0-9]+',x[1]) for x in pairs):raise ValueError('integer public fields required')
        row={k:int(v) for k,v in pairs}
        if row['v']!=1 or row['seq']!=sequence+1 or row['seq']>512:raise ValueError('public record sequence/version mismatch')
        sequence=row['seq']
        if row['deck'] not in range(1,5) or row['returned']!=row['deck']:raise ValueError('public queried/returned deck mismatch')
        query=row['query_ns']
        if not 0<query<2**64 or compare_timestamps(query,prior_query)<0 or compare_timestamps(query,now_ns+10_000_000)>0:raise ValueError('backward/future public query clock')
        prior_query=query
        if not 0<=row['valid']<2**32 or not -(2**31)<=row['rc']<2**31 or not -(2**31)<=row['raw']<2**31:
            raise ValueError('public integer range exceeded')
        for stream,(bit,key,_) in STREAMS.items():
            for field in ('unit','rev','ns'):
                if not 0<=row[key+'_'+field]<2**64:raise ValueError('public receipt range exceeded')
            if row['valid']&bit:setting(row,stream)
            elif row[key]!=-1:raise ValueError('unavailable public setting must remain unknown')
        latest[row['deck']]=row
    if set(latest)!=set(range(1,5)):raise MissingEvidence('all four public API rows required')
    if any(compare_timestamps(now_ns - r['query_ns'], 5000000000) > 0 for r in latest.values()):raise StaleObservation('public API query is stale')
    return latest

def tempo_rows(text,now_ns):
    """Read the separate public BPM/slider rows; never infer motion or load."""
    fields={'v','seq','deck','returned','query_ns','rc','valid','tempo_valid','tempo',
            'tempo_state','tempo_rev','tempo_ns','slider_valid','slider_locked',
            'slider_unit','slider_rev','slider_ns'}
    if not isinstance(text,str) or len(text.encode())>262144:
        raise ValueError('bounded public tempo log required')
    if type(now_ns) is not int or not 0<now_ns<2**64:
        raise ValueError('bound native monotonic clock required')
    latest={};sequence=0;prior_query=0
    for line in text.splitlines():
        if not line.startswith('SDK_PUBLIC_TEMPO v='):continue
        pairs=[part.split('=',1) for part in line.split()[1:]]
        if len(line.encode())>=512 or any(len(p)!=2 for p in pairs) or len({p[0] for p in pairs})!=len(pairs) or {p[0] for p in pairs}!=fields:
            raise ValueError('exact public tempo record fields required')
        raw=dict(pairs)
        if not re.fullmatch(r'(0|[1-9][0-9]*)\.[0-9]{6}',raw['tempo']):
            raise ValueError('finite fixed-decimal BPM required')
        if any(not re.fullmatch(r'0|[1-9][0-9]*|-[1-9][0-9]*',v) for k,v in pairs if k!='tempo'):
            raise ValueError('canonical public tempo integers required')
        row={k:float(v) if k=='tempo' else int(v) for k,v in pairs}
        sequence+=1;query=row['query_ns']
        if sequence>512 or row['v']!=1 or row['seq']!=sequence or row['deck'] not in range(1,5) or row['returned']!=row['deck']:
            raise ValueError('public tempo version/sequence/deck mismatch')
        if not 0<query<2**64 or compare_timestamps(query,prior_query)<0 or compare_timestamps(query,now_ns+10_000_000)>0:
            raise ValueError('backward/future public tempo query clock')
        prior_query=query
        if not 0<=row['valid']<2**32 or not -(2**31)<=row['rc']<2**31:
            raise ValueError('public tempo integer range exceeded')
        for prefix,bit,identity,scalar in [('tempo',64,'tempo_state','tempo'),('slider',131072,'slider_unit','slider_locked')]:
            flag=row[prefix+'_valid']
            if flag not in (0,1):raise ValueError('public tempo availability must be boolean')
            receipt=[row[identity],row[prefix+'_rev'],row[prefix+'_ns']]
            if any(not 0<=v<2**64 for v in receipt):raise ValueError('public tempo receipt range exceeded')
            if flag:
                valid_scalar=(20.0<row[scalar]<400.0 if prefix=='tempo' else row[scalar] in (0,1))
                if row['rc'] or not row['valid']&bit or not all(receipt) or compare_timestamps(receipt[2],query)>0 or not valid_scalar:
                    raise ValueError('unqualified public tempo/slider scalar')
            elif row[scalar]!=(0.0 if prefix=='tempo' else -1):
                raise ValueError('unavailable public tempo/slider must remain unknown')
        latest[row['deck']]=row
    if set(latest)!=set(range(1,5)):raise MissingEvidence('all four public tempo API rows required')
    if any(compare_timestamps(now_ns - r['query_ns'], 5000000000) > 0 for r in latest.values()):
        raise StaleObservation('public tempo API query is stale')
    return latest

def native_rows(text,now_ns):
    """Current target's public native/BPM rows; source-qualified load is absent."""
    tempo_fields=['v','seq','deck','returned','query_ns','rc','valid','tempo_valid','tempo',
                  'tempo_state','tempo_rev','tempo_ns','slider_valid','slider_locked','slider_unit','slider_rev','slider_ns']
    fields=set(tempo_fields)|{'master_valid','master','master_rev','master_ns','load_valid','native_load','load_unit','load_epoch','load_ns','source_load'}
    if not isinstance(text,str) or len(text.encode())>524288:raise ValueError('bounded public native log required')
    latest={};tempo=[];sequence=0
    for line in text.splitlines():
        if not line.startswith('SDK_PUBLIC_NATIVE v='):continue
        pairs=[part.split('=',1) for part in line.split()[1:]]
        if len(line.encode())>=768 or any(len(p)!=2 for p in pairs) or len({p[0] for p in pairs})!=len(pairs) or {p[0] for p in pairs}!=fields:
            raise ValueError('exact combined public native record required')
        raw=dict(pairs)
        if any(not re.fullmatch(r'0|[1-9][0-9]*|-[1-9][0-9]*',v) for k,v in pairs if k!='tempo'):
            raise ValueError('canonical public native integers required')
        row={k:v if k=='tempo' else int(v) for k,v in pairs}
        sequence+=1
        if sequence>512 or row['seq']!=sequence:raise ValueError('public native sequence/count mismatch')
        if row['source_load'] or row['valid']&2:raise ValueError('native load must not manufacture source-qualified load/track')
        for key in ('master_rev','master_ns','native_load','load_unit','load_epoch','load_ns','source_load'):
            if not 0<=row[key]<2**64:raise ValueError('public native receipt range exceeded')
        if row['master_valid'] not in (0,1) or row['load_valid'] not in (0,1):raise ValueError('public native validity must be boolean')
        if row['master_valid']:
            if row['rc'] or not row['valid']&1024 or row['master'] not in (0,1) or not row['master_rev'] or not (compare_timestamps(0, row['master_ns']) < 0 and compare_timestamps(row['master_ns'], row['query_ns']) <= 0):
                raise ValueError('unqualified public master receipt')
        elif row['master']!=-1:raise ValueError('unknown public master must stay unknown')
        if row['load_valid']:
            if row['rc'] or not row['valid']&262144 or not all(row[k] for k in ('native_load','load_unit','load_epoch','load_ns')) or compare_timestamps(row['load_ns'], row['query_ns']) > 0:
                raise ValueError('unqualified public native load receipt')
        elif row['native_load']:raise ValueError('unknown public native load must stay unknown')
        tempo.append('SDK_PUBLIC_TEMPO '+' '.join(k+'='+raw[k] for k in tempo_fields))
        latest[row['deck']]=row
    # Reuse the exact version, returned-deck, clock, BPM and slider guards.
    qualified=tempo_rows('\n'.join(tempo)+'\n',now_ns)
    for deck,row in qualified.items():latest[deck].update(row)
    return latest

def native_load_after(row,fence_ns):
    if type(fence_ns) is not int or not 0<fence_ns<2**64:raise ValueError('native pre-LOAD clock fence required')
    return bool(row['load_valid'] and compare_timestamps(row['query_ns'], fence_ns) > 0 and compare_timestamps(row['load_ns'], fence_ns) > 0)

def confirmed_master(rows):
    if set(rows)!=set(range(1,5)) or not all(r['master_valid'] for r in rows.values()):raise MissingEvidence('four qualified public master rows unavailable')
    first=next(iter(rows.values()))
    if any(r['master_rev']!=first['master_rev'] or compare_timestamps(r['master_ns'],first['master_ns'])!=0 for r in rows.values()):raise MissingEvidence('one shared public master event unavailable')
    members=[d for d,r in rows.items() if r['master']==1]
    if len(members)!=1:raise ValueError('shared master event must select one local deck')
    return members[0]

def cue_rows(text,now_ns):
    fields={'v','seq','deck','returned','query_ns','rc','valid','cue_valid','cue_unit','cue_rev','cue_ns','event_ns','source_load'}
    if not isinstance(text,str) or len(text.encode())>524288:raise ValueError('bounded public cue log required')
    if type(now_ns) is not int or not 0<now_ns<2**64:raise ValueError('bound native cue query clock required')
    latest={};sequence=0;prior_query=0
    for line in text.splitlines():
        if not line.startswith('SDK_PUBLIC_CUE v='):continue
        pairs=[part.split('=',1) for part in line.split()[1:]]
        if len(line)>512 or any(len(p)!=2 for p in pairs) or len({p[0] for p in pairs})!=len(pairs) or {p[0] for p in pairs}!=fields:
            raise ValueError('exact public cue fields required')
        if any(not re.fullmatch('-?[0-9]+',p[1]) for p in pairs):raise ValueError('integer public cue fields required')
        row={k:int(v) for k,v in pairs}
        if row['v']!=1 or row['seq']!=sequence+1 or row['seq']>512:raise ValueError('public cue version/sequence mismatch')
        sequence=row['seq']
        if any(not 0<=row[k]<2**64 for k in fields-{'rc'}):raise ValueError('invalid public cue unsigned field')
        if not -2**31<=row['rc']<2**31 or not 0<=row['valid']<2**32 or row['cue_valid'] not in (0,1):raise ValueError('invalid public cue availability')
        if row['deck'] not in range(1,5) or row['returned'] not in (0,row['deck']):raise ValueError('public cue queried/returned deck mismatch')
        query=row['query_ns']
        if not query or compare_timestamps(query,prior_query)<0 or compare_timestamps(query,now_ns+10_000_000)>0:raise ValueError('invalid/future public cue query')
        prior_query=query
        if row['source_load'] or row['valid']&2:raise ValueError('unqualified cue source/load association')
        if compare_timestamps(row['cue_ns'], query) > 0 or compare_timestamps(row['event_ns'], query) > 0:raise ValueError('future public cue/callback receipt')
        if row['cue_valid'] and (row['rc']!=0 or row['returned']!=row['deck'] or not row['valid']&524288 or not all(row[k] for k in ('cue_unit','cue_rev','cue_ns'))):
            raise ValueError('invalid public cue receipt')
        latest[row['deck']]=row
    if set(latest)!=set(range(1,5)):raise MissingEvidence('four public cue query rows unavailable')
    if any(compare_timestamps(now_ns - r['query_ns'], 5000000000) > 0 for r in latest.values()):raise StaleObservation('public cue query is stale')
    return latest

def cue_witness(row,fence_ns):
    if type(fence_ns) is not int or not 0<fence_ns<2**64:raise ValueError('native pre-LOAD cue clock fence required')
    return bool(row['rc']==0 and row['cue_valid'] and all(compare_timestamps(row[k],fence_ns)>0 for k in ('query_ns','cue_ns','event_ns')))

def transition(before,after,stream,fence_ns,want):
    if before['deck']!=after['deck'] or after['returned']!=before['deck']:raise ValueError('public transition deck changed')
    if type(fence_ns) is not int or not 0<fence_ns<2**64 or type(want) is not int or want not in (0,1):
        raise ValueError('public transition fence/value required')
    first=setting(before,stream);last=setting(after,stream)
    if first['unit']!=last['unit']:raise ValueError('public native unit changed during transition')
    return (compare_timestamps(after['query_ns'], fence_ns) > 0 and last['revision']>first['revision'] and compare_timestamps(last['received_ns'], fence_ns) > 0 and
            last['value']==want and first['value']!=last['value'])

class PublicSettingChecks:
    """One explicit setup, then causal roundtrips through fresh public rows."""
    def __init__(self,controller,read_public):
        if not callable(read_public):raise ValueError('owned public API reader required')
        self.controller=controller;self.read=read_public;self.setup_attempts=set()
    def wait(self,predicate,label):
        c=self.controller;end=c._now()+min(c.timeout,30);stable=0;result=None
        for _ in range(int(min(c.timeout,30)/c.interval)+3):
            if compare_timestamps(c._now(), end, units_per_second=1) >= 0:break
            try:result=self.read();passed=predicate(*result)
            except MissingEvidence:stable=0
            else:
                if compare_timestamps(c._now(), end, units_per_second=1) >= 0:break
                stable=stable+1 if passed else 0
                if stable>=2:return result
            c.sleep(c.interval)
        raise MissingEvidence('public setting observation deadline: '+label)
    def establish(self,deck,stream):
        if type(deck) is not int or deck not in range(1,5) or stream not in STREAMS:raise ValueError('one logical deck/public setting required')
        if (deck,stream) in self.setup_attempts:raise MissingEvidence('public observation setup already attempted; no retry')
        rows,fence=self.read();before=rows[deck]
        try:existing=setting(before,stream)
        except MissingEvidence:existing=None
        if existing is not None:return dict(deck=deck,stream=stream,input_sent=False,initial_value=None,receipt=before)
        self.setup_attempts.add((deck,stream))
        self.controller.button(deck,STREAMS[stream][2])
        def ready(rows,now):
            value=setting(rows[deck],stream)
            return compare_timestamps(rows[deck]['query_ns'], fence) > 0 and compare_timestamps(value['received_ns'], fence) > 0
        rows,_=self.wait(ready,'deck%d first %s receipt'%(deck,stream))
        return dict(deck=deck,stream=stream,input_sent=True,initial_value=None,before_ns=fence,receipt=rows[deck])
    def roundtrip(self,deck,stream):
        if type(deck) is not int or deck not in range(1,5) or stream not in STREAMS:raise ValueError('one logical deck/public setting required')
        rows,fence=self.read();before=rows[deck];first=setting(before,stream);records=[dict(before)]
        for want in (1-first['value'],first['value']):
            self.controller.button(deck,STREAMS[stream][2])
            rows,now=self.wait(lambda rows,now:transition(before,rows[deck],stream,fence,want),'deck%d %s=%d'%(deck,stream,want))
            before=rows[deck];fence=now;records.append(dict(before))
        return dict(status='PASS',deck=deck,stream=stream,records=records)

def run_public_phases(run_phase,restart,probe_sha256,require_settings=True,loaded=False):
    """Sequence normal UI trust and a distinct owned native process.

    Callbacks supply observed evidence; this function neither grants approval
    nor substitutes private receipts for the public settings checks.
    """
    if not all(callable(f) for f in (run_phase,restart)) or not re.fullmatch('[a-f0-9]{64}',probe_sha256):
        raise ValueError('bounded public phase callbacks and binary hash required')
    if type(require_settings) is not bool or type(loaded) is not bool or (loaded and require_settings):raise ValueError('explicit public feature selection required')
    report=dict(status='INCOMPLETE',scope='normal loader public Master Tempo/Sync enabled settings on units1–4; no loaded source/transport/paused evidence',errors=[],missing=[])
    report['settings_execution']='REQUESTED' if require_settings else 'NOT_RUN'
    if not require_settings:report['scope']='normal loader public waveform only; settings controls not run'
    if loaded:report['scope']='normal UI Allow/reload and separately scoped loaded-deck public observations/controls'
    try:
        before=run_phase('approval');report['approval']=before
        if before.get('status')!='PASS' or before.get('ui_allow') is not True:
            report['missing'].append('genuine normal UI Allow proof');return report
        if before.get('approved_sha256')!=probe_sha256:raise ValueError('normal approval binary hash mismatch')
        if before.get('ui_reload') is not True or before.get('reload_request')!='apply':
            report['missing'].append('genuine UI reload request');return report
        restart(before)
        phase='loaded' if loaded else 'settings'
        after=run_phase(phase);report[phase]=after
        new_process(before,after)
        if after.get('mapped_probe_sha256')!=probe_sha256:raise ValueError('new process public probe mapping hash mismatch')
        if after.get('status')=='FAIL':raise ValueError('public settings runtime failed')
        if loaded:
            if after.get('status')!='PASS':report['missing'].append('loaded public runtime completion')
            report['status']='INCOMPLETE' if report['missing'] else 'PASS'
            return report
        if not require_settings:
            if after.get('settings_execution')!='NOT_RUN' or after.get('events'):raise ValueError('wave-only phase must not execute settings stimuli')
            if after.get('status')!='PASS':report['missing'].append('public waveform runtime completion')
            report['status']='INCOMPLETE' if report['missing'] else 'PASS'
            return report
        for deck in range(1,5):
            for stream in STREAMS:
                if after.get('checks',{}).get(str(deck),{}).get(stream)!='PASS':
                    report['missing'].append('deck%d public %s roundtrip'%(deck,stream))
        if after.get('status')!='PASS':report['missing'].append('public settings runtime completion')
        report['status']='INCOMPLETE' if report['missing'] else 'PASS'
    except MissingEvidence as error:report['missing'].append(str(error))
    except (RuntimeError,ValueError,OSError) as error:report['status']='FAIL';report['errors'].append(str(error))
    return report
