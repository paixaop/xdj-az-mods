#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Normal-loader public native cue commands, with source and storage witnesses."""
from sdk_timestamps import compare_timestamps
from sdk_emulator_loop_runtime import *
from sdk_emulator_controls import position_motion,MissingEvidence
import math,time
ID='sdk-cue-command-probe'
LOG='/tmp/azmod-community/'+ID+'.log'
OPS=tuple(range(13,27));ALL_OPS=tuple(range(1,27));MASK=sum(1<<op for op in OPS)
SELECTED_CASES={'active-loop-switch':(16,),'memory-set':(21,),'memory-comment':(25,),'memory-color':(25,26),'memory-color-reload':(25,26),'main-cue':(23,24),'saved-loop':(13,14,15,16),'saved-loop-recall':(13,),'hot-clear':(19,),'hot-set':(18,),'hot-cues':(17,18,19,25,26)}
BASE=set('v seq event id deck op target_load target_revision flags classification entry_id rgb timeout_ns submitted_ns deadline_ns rc outcome reason revision receipt_ns query_ns load_valid native_load load_unit load_ns loop_valid engaged loop_in loop_out main_valid main_s cues_rc cues_load cues_rev cue_count'.split())
ENTRY=set('v snapshot_seq deck load revision classification entry_id slot valid type start end rgb color_id saved modified auto_active comment_hex'.split())
FLOATS={'loop_in','loop_out','main_s','start','end'}
SIGNED={'rc','outcome','reason','engaged','cues_rc','saved','modified','auto_active'}
def ready(text,pid):
 found=[]
 for line in text.splitlines():
  if not line.startswith('SDK_CUE_READY '):continue
  pairs=[p.split('=',1) for p in line.split()[1:]]
  if len(pairs)!=4 or {p[0] for p in pairs}!=set(('v','pid','commands','data')):raise ValueError('exact cue readiness required')
  row=dict(pairs)
  if row['v']!='1' or row['pid']!=str(pid) or not row['commands'].isdecimal():raise ValueError('cue readiness process changed')
  found.append(dict(data=row['data'],commands=int(row['commands'])))
 if not found:raise MissingEvidence('cue probe readiness missing')
 if len({x['data'] for x in found})!=1:raise ValueError('cue data_dir changed')
 return found[-1]
def records(text):
 if not isinstance(text,str) or len(text.encode())>2097152:raise ValueError('bounded cue transcript required')
 rows=[];entries=[];last_seq=0
 for line in text.splitlines():
  kind='base' if line.startswith('SDK_CUE ') else 'entry' if line.startswith('SDK_CUE_ENTRY ') else None
  if kind is None:continue
  pairs=[p.split('=',1) for p in line.split()[1:]];fields=BASE if kind=='base' else ENTRY
  if len(line)>2048 or any(len(p)!=2 for p in pairs) or len(pairs)!=len(fields) or {p[0] for p in pairs}!=fields:raise ValueError('exact cue receipt fields required')
  row={}
  for key,value in pairs:
   if key=='event':
    if value not in ('snapshot','submit','poll','release','cancel','refused'):raise ValueError('unknown cue event')
    row[key]=value
   elif key=='comment_hex':
    if value=='-':row['comment']=''
    else:
     if not re.fullmatch('[0-9a-f]{2,254}',value) or len(value)%2:raise ValueError('bounded cue UTF8 hex required')
     raw=bytes.fromhex(value)
     if b'\0' in raw:raise ValueError('embedded cue comment NUL')
     row['comment']=raw.decode('utf8')
   elif key in FLOATS:
    n=float(value)
    if not math.isfinite(n):raise ValueError('finite cue source seconds required')
    row[key]=n
   else:
    if not re.fullmatch('-?(0|[1-9][0-9]*)' if key in SIGNED else '(0|[1-9][0-9]*)',value):raise ValueError('canonical cue integer required')
    n=int(value)
    if not (-(2**31) if key in SIGNED else 0)<=n<2**64:raise ValueError('bounded cue integer required')
    row[key]=n
  if row['v']!=1 or row['deck'] not in range(4):raise ValueError('cue schema/deck mismatch')
  if kind=='base':
   if row['seq']<=last_seq or row['outcome'] not in (-1,0,1,2,3,4):raise ValueError('cue sequence/outcome changed')
   last_seq=row['seq'];rows.append(row)
  else:
   if row['classification'] not in (0,1,2) or row['classification']==1 and row['entry_id'] not in range(1,9):raise ValueError('native Hot IDs must be1..8')
   if row['classification']==2 and not row['entry_id']:raise ValueError('native memory entry ID required')
   entries.append(row)
 return rows,entries

def request(pid,action,identity=0,deck=0,op=0,load=0,revision=0,timeout=0,flags=0,classification=0,entry_id=0,position=0,end=0,beats=0,rgb=0,comment=''):
 if isinstance(pid,bool) or not re.fullmatch('[1-9][0-9]{0,9}',str(pid)) or int(pid)>=2**31:raise ValueError('positive native PID required')
 raw=comment.encode('utf8')
 if len(raw)>127 or b'\0' in raw:raise ValueError('cue comment UTF8 maximum127bytes')
 for value in (position,end,beats):
  if type(value) not in (int,float) or not math.isfinite(value):raise ValueError('finite cue arguments required')
 values=(int(pid),identity,deck,op,load,revision,timeout,flags,classification,entry_id,rgb)
 if any(type(v)is not int or not 0<=v<2**64 for v in values) or deck not in range(4) or action not in ('snapshot','state','submit','cancel'):raise ValueError('bounded cue request identity required')
 if not 0<=flags<=7 or not 0<=classification<=2 or not 0<=op<=26 or not 0<=entry_id<2**32 or not 0<=rgb<=0xFFFFFF:raise ValueError('bounded native cue arguments required')
 line=f'v=1 pid={pid} action={action} id={identity} deck={deck} op={op} load={load} revision={revision} timeout_ns={timeout} flags={flags} classification={classification} entry_id={entry_id} position={position:.17g} end={end:.17g} beats={beats:.17g} rgb={rgb} comment_hex={raw.hex() if raw else "-"}\n'
 if len(line.encode())>1023:raise ValueError('bounded cue request required')
 return line

class CueClient:
 def __init__(self,read_log,send,health,pid,*,sleep=time.sleep,clock=time.monotonic):self.read_log=read_log;self.send=send;self.health=health;self.pid=pid;self.sleep=sleep;self.clock=clock;self.rows=[];self.entries=[];self.evidence=[];self.high=0
 def read(self):
  if not self.health():raise MissingEvidence('owned cue process unhealthy')
  self.rows,self.entries=records(self.read_log());self.high=max(self.high,max((r['id'] for r in self.rows),default=0));return self.rows
 def wait(self,predicate,seconds=30):
  end=self.clock()+seconds
  while compare_timestamps(self.clock(), end, units_per_second=1) < 0:
   for row in reversed(self.read()):
    if predicate(row):return row
   self.sleep(.1)
  raise MissingEvidence('bounded public cue receipt wait expired')
 def snapshot(self,deck,*,entries=True):
  self.read();fence=self.rows[-1]['seq'] if self.rows else 0;self.send(request(self.pid,'snapshot' if entries else 'state',deck=deck))
  def complete(row):return row['event']=='snapshot' and row['deck']==deck and row['seq']>fence and (row['cues_rc']!=0 or len([e for e in self.entries if e['snapshot_seq']==row['seq']])==row['cue_count'])
  row=self.wait(complete);row=dict(row,entries=[e for e in self.entries if e['snapshot_seq']==row['seq']])
  if any((e['deck'],e['load'],e['revision'])!=(deck,row['cues_load'],row['cues_rev']) for e in row['entries']):raise ValueError('cue snapshot association changed')
  keys=[(e['classification'],e['entry_id']) for e in row['entries']]
  if len(set(keys))!=len(keys):raise ValueError('duplicate native cue entry IDs')
  return row
 def submit(self,deck,op,*,load=None,expect_unsupported=False,**arguments):
  before=self.snapshot(deck,entries=False)
  if load is None:
   if not before['load_valid'] or not before['native_load']:raise MissingEvidence('actual native load required')
   load=before['native_load']
  self.high+=1;identity=self.high
  self.send(request(self.pid,'submit',identity,deck,op,load=load,timeout=10000000000,**arguments))
  row=self.wait(lambda r:r['id']==identity and r['event'] in ('submit','poll','refused') and r['outcome']!=0)
  if (row['deck'],row['op'],row['target_load'])!=(deck,op,load):raise ValueError('cue result belongs to different request')
  if any(row[field]!=arguments.get(argument,0) for field,argument in (('target_revision','revision'),('flags','flags'),('classification','classification'),('entry_id','entry_id'),('rgb','rgb'))):raise ValueError('cue result arguments changed')
  if row['event']=='refused':raise MissingEvidence('cue request refused before SDK admission')
  if compare_timestamps(row['timeout_ns'], 10000000000) != 0 or compare_timestamps(row['submitted_ns'], before['query_ns']) <= 0 or compare_timestamps(row['deadline_ns'], row['submitted_ns']+10000000000) != 0:raise ValueError('actual native10-second submit interval required')
  submitted=next((r for r in self.rows if r['id']==identity and r['event']=='submit'),None)
  if submitted is None or any(compare_timestamps(row[k],submitted[k])!=0 for k in ('timeout_ns','submitted_ns','deadline_ns')):raise ValueError('pending cue deadline changed')
  if load==2**64-1:
   if (row['event'],row['rc'],row['outcome'],row['reason'])!=('submit',-4,2,-4) or not (compare_timestamps(row['submitted_ns'], row['receipt_ns']) <= 0 and compare_timestamps(row['receipt_ns'], row['deadline_ns']) < 0):raise ValueError('actual SDK stale admission required')
   print('SDK cue refusal deck=%d op=%d outcome=REJECTED reason=AZ_STALE' % (deck+1,op),flush=True)
   return row
  if expect_unsupported:
   if row['outcome']!=2 or row['reason']!=-2 or row['rc'] not in (0,-2):raise ValueError('actual SDK unsupported rejection required')
   if not (compare_timestamps(row['submitted_ns'], row['receipt_ns']) <= 0 and compare_timestamps(row['receipt_ns'], row['deadline_ns']) <= 0):raise ValueError('unsupported rejection outside native request deadline')
   # scmd_submit validation refusal returns before slot allocation.
   if row['rc']==-2:
    if row['event']!='submit':raise ValueError('immediate unsupported must be SDK submit refusal')
    return row
   if not any(r['id']==identity and r['event']=='release' and r['rc']==0 for r in self.rows):self.wait(lambda r:r['id']==identity and r['event']=='release' and r['rc']==0)
   return row
  if row['outcome']==1 and not (compare_timestamps(row['submitted_ns'], row['receipt_ns']) <= 0 and compare_timestamps(row['receipt_ns'], row['deadline_ns']) <= 0):raise ValueError('cue APPLIED receipt outside native request deadline')
  if (row['rc'],row['outcome'],row['reason'])!=(0,1,0) or not row['revision'] or not row['load_valid'] or (row['native_load'],row['load_unit'])!=(load,before['load_unit']):raise ValueError('cue command deck%d op%d did not reach load-qualified APPLIED: rc=%d outcome=%d reason=%d' % (deck+1,op,row['rc'],row['outcome'],row['reason']))
  if not any(r['id']==identity and r['event']=='release' and r['rc']==0 for r in self.rows):self.wait(lambda r:r['id']==identity and r['event']=='release' and r['rc']==0)
  print('SDK cue receipt deck=%d op=%d outcome=APPLIED native_ms=%.3f' % (deck+1,op,(row['receipt_ns']-row['submitted_ns'])/1000000),flush=True)
  self.evidence.append(dict(operation=op,before=before,terminal=row));return row
 def cues(self,deck,predicate=lambda s:True):
  end=self.clock()+45
  while compare_timestamps(self.clock(), end, units_per_second=1) < 0:
   s=self.snapshot(deck)
   if s['load_valid'] and s['cues_rc']==0 and s['cues_load']==s['native_load'] and s['cues_rev'] and predicate(s):return s
   self.sleep(.1)
  raise MissingEvidence('fresh native cue storage snapshot unavailable')

def arguments(op):
 if op in range(1,13):
  return dict(flags=2,**({'beats':8} if op==9 else {'position':.05} if op in (10,11) else {'end':.05} if op==12 else {}))
 if op in (17,18,19):return dict(flags=2 if op==17 else 3,classification=1,entry_id=1)
 if op==26:return dict(flags=7,classification=2,entry_id=1,revision=1,rgb=0x00C3FF)
 if op in (13,15,16,20,22,25,26):return dict(flags=6 if op in (13,20) else 7,classification=2,entry_id=1,revision=1)
 return dict(flags=2 if op in (23,24) else 3,classification=0 if op in (23,24) else 2)
def run_refusals(read_log,send_request,health,pid,*,decks=(0,1,2,3)):
 c=CueClient(read_log,send_request,health,pid);report=dict(status='PASS',scope='all26 stale-load admission rejections only; no native application acceptance',errors=[],missing=[],decks={})
 try:
  for deck in decks:
   for op in ALL_OPS:c.submit(deck,op,load=2**64-1,**arguments(op))
   report['decks'][str(deck)]=dict(status='PASS',operations=list(ALL_OPS))
 except MissingEvidence as e:report.update(status='INCOMPLETE',missing=[str(e)])
 except (ValueError,RuntimeError) as e:report.update(status='FAIL',errors=[str(e)])
 return report

def validate_test_selection(phase,selection=None):
 if phase not in ('full','positive','refusals','memory-color','memory-color-reload','selected'):raise ValueError('explicit cue test phase required')
 if phase=='selected':
  if not isinstance(selection,str) or selection not in SELECTED_CASES:raise ValueError('unknown selected native cue case')
 elif selection is not None:raise ValueError('cue case selector requires selected phase')
 return selection

def run_test_phase(phase,read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),reload_deck=None,selection=None,sample_rate=None,memory_entry_id=None):
 validate_test_selection(phase,selection)
 persistence=phase=='memory-color-reload' or selection=='memory-color-reload'
 if persistence and not callable(reload_deck):raise ValueError('explicit Memory reload persistence requires a reload callback')
 if memory_entry_id is not None and (selection!='memory-comment' or type(memory_entry_id)is not int or not 0<memory_entry_id<2**32):raise ValueError('Memory comment selection requires a native uint32 entry ID')
 if phase=='refusals':return run_refusals(read_log,send_request,health,pid,decks=decks)
 if phase=='selected':
  if selection=='active-loop-switch':result=run_active_loop_switch(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='memory-comment':result=run_memory_comment(read_log,send_request,health,pid,decks=decks,sample_rate=sample_rate,entry_id=memory_entry_id)
  elif selection=='saved-loop-recall':result=run_saved_recall(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='saved-loop':result=run_saved_loop(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='main-cue':result=run_main_cue(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='memory-set':result=run_memory_set(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='hot-clear':result=run_hot_clear(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='hot-set':result=run_hot_set(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  elif selection=='hot-cues':result=run_hot_cues(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
  else:
   result=run_memory_color(read_log,send_request,health,pid,setup_deck=setup_deck,reload_deck=reload_deck if persistence else None,decks=decks,sample_rate=sample_rate)
   if result['status']=='PASS':
    for value in result['decks'].values():value['operations']=list(SELECTED_CASES[selection])
  allowed=list(SELECTED_CASES[selection])
  for value in result.get('decks',{}).values():
   if value.get('operations',[])!=allowed:raise ValueError('selected case cannot claim complete command coverage')
  if result['status']=='PASS' and set(result.get('decks',{}))!={str(deck) for deck in decks}:
   result.update(status='INCOMPLETE',missing=['selected native case missing a requested deck'])
  result.update(mode='selected',selection=selection,requested_operations=allowed,coverage='selected case only')
  return result
 if phase in ('memory-color','memory-color-reload'):return run_memory_color(read_log,send_request,health,pid,setup_deck=setup_deck,reload_deck=reload_deck if persistence else None,decks=decks,sample_rate=sample_rate)
 refusals=run_refusals(read_log,send_request,health,pid) if phase=='full' else dict(status='NOT_RUN',scope='separate positive-only phase')
 if phase=='full' and refusals['status']!='PASS':
  return dict(status=refusals['status'],errors=list(refusals.get('errors',[])),missing=list(refusals.get('missing',[])),decks={},evidence=[],admission_refusals=refusals)
 result=run_commands(read_log,send_request,health,pid,setup_deck=setup_deck,read_positions=read_positions,decks=decks,sample_rate=sample_rate)
 result['admission_refusals']=refusals
 return result

def run_logged_case(argv):
 """Stream an existing behavioral runner and read its final qualified report."""
 import subprocess
 report_path=None
 with subprocess.Popen(argv,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1) as process:
  for line in process.stdout:
   print(line,end='',flush=True)
   if line.startswith('CURRENT_NATIVE_RESULT '):report_path=Path(line[len('CURRENT_NATIVE_RESULT '):].strip())
  if process.wait()!=0:raise RuntimeError('native command case failed: '+str(argv[1:]))
 if report_path is None or not report_path.is_absolute() or report_path.is_symlink() or not report_path.is_file() or report_path.stat().st_size>2097152:raise MissingEvidence('bounded native case report unavailable')
 result=json.loads(report_path.read_text())
 if result.get('status')!='PASS':raise ValueError('native case did not pass: '+str(result.get('errors',result.get('missing',[]))))
 return result

def run_loaded_command_groups(run_loops,run_cues,observe,*,decks=(0,)):
 """Compose existing loop/cue checks on one pinned load, with no GUI callback.

 The caller supplies normal public-SDK runners and a fresh native/source guard.
 Reload/reboot persistence is a separate explicit test, never a group boundary.
 """
 decks=tuple(decks)
 if not decks or len(set(decks))!=len(decks) or any(type(d)is not int or d not in range(4) for d in decks):raise ValueError('explicit unique loaded decks required')
 if not all(callable(f) for f in (run_loops,run_cues,observe)):raise ValueError('existing behavioral runners and a loaded-source guard required')
 report=dict(status='PASS',errors=[],missing=[],decks={},scope='all26 command behavior on the existing track/load; reload and reboot persistence not run')
 def identity(deck):
  value=observe(deck)
  if not isinstance(value,dict) or set(value)!=set(('pid','birth','native_load','load_unit','title','sample_rate')):raise MissingEvidence('complete loaded process/source identity required')
  if any(not isinstance(value[k],str) or not value[k].isascii() or not value[k].isdecimal() or int(value[k])<=0 for k in ('pid','birth')) or any(type(value[k])is not int or value[k]<=0 for k in ('native_load','load_unit','sample_rate')) or not isinstance(value['title'],str) or not value['title']:raise MissingEvidence('qualified loaded track required before command groups')
  return dict(value)
 try:
  pinned={deck:identity(deck) for deck in decks}
  for deck in decks:
   completed=[];groups={}
   report['decks'][str(deck)]=dict(status='INCOMPLETE',operations=completed,identity=pinned[deck],groups=groups)
   for name,run,expected in (('loops',run_loops,list(range(1,13))),('cues',run_cues,list(OPS))):
    if identity(deck)!=pinned[deck]:raise ValueError('loaded track/process changed before '+name)
    print('SAME_TRACK deck=%d group=%s load=%d'%(deck+1,name,pinned[deck]['native_load']),flush=True)
    result=run(deck);groups[name]=result
    if identity(deck)!=pinned[deck]:raise ValueError('loaded track/process changed during '+name)
    if result.get('status')!='PASS':
     report.update(status='FAIL' if result.get('status')=='FAIL' else 'INCOMPLETE',errors=list(result.get('errors',[])),missing=list(result.get('missing',[])))
     report['decks'][str(deck)]['status']=report['status']
     return report
    actual=result.get('decks',{}).get(str(deck),{})
    if set(result.get('decks',{}))!={str(deck)} or actual.get('status')!='PASS' or actual.get('operations')!=expected:raise MissingEvidence('complete per-deck '+name+' behavioral coverage required')
    completed.extend(expected)
   report['decks'][str(deck)]['status']='PASS'
 except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)])
 except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)])
 return report

def source_between(value,first,last,sample_rate):
 return compare_timestamps(value,first,units_per_second=1,sample_rate=sample_rate)>=0 and compare_timestamps(value,last,units_per_second=1,sample_rate=sample_rate)<=0

def source_capture_bracket(rows,submitted_ns,receipt_ns,sample_rate):
 """Bracket a moving native capture with independently published playheads.

 The snapshot cursor is published after the native render returns. A read just
 after command settlement can therefore still hold the previous render's cursor.
 Require the first post-receipt publication that advances the observed source;
 do not add a sample tolerance or infer a render size from the reader's clock.
 """
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required for capture')
 if compare_timestamps(receipt_ns,submitted_ns)<0:raise ValueError('capture receipt precedes submission')
 if any(compare_timestamps(b[0],a[0])<=0 for a,b in zip(rows,rows[1:])):raise ValueError('source capture clock did not advance')
 previous=[index for index,row in enumerate(rows) if compare_timestamps(row[0],submitted_ns)<=0]
 if not previous:raise MissingEvidence('source observation before native capture required')
 first=previous[-1]
 following=[index for index,row in enumerate(rows) if index>first and compare_timestamps(row[0],receipt_ns)>=0 and compare_timestamps(row[1],rows[first][1],units_per_second=1,sample_rate=sample_rate)>0]
 if not following:raise MissingEvidence('post-receipt source publication must advance')
 bracket=rows[first:following[0]+1]
 if any(compare_timestamps(b[1],a[1],units_per_second=1,sample_rate=sample_rate)<0 for a,b in zip(bracket,bracket[1:])):raise ValueError('captured source moved backwards')
 return bracket

def cue_target_seen(rows,target,floor=0,*,sample_rate):
 return any(compare_timestamps(stamp,floor)>=0 and compare_timestamps(position,target,units_per_second=1,sample_rate=sample_rate)>=0 and compare_timestamps(position,target+.5,units_per_second=1,sample_rate=sample_rate)<=0 for stamp,position in rows)

def wait_cue_source(reader,deck,predicate,floor=0,*,clock=time.monotonic,sleep=time.sleep):
 end=clock()+90
 while compare_timestamps(clock(),end,units_per_second=1)<0:
  rows=[row for row in reader(deck) if compare_timestamps(row[0],floor)>=0]
  if rows and predicate(rows):return rows
  sleep(.1)
 raise MissingEvidence('actual cue source effect unavailable')

def main_cue_roundtrip(client,deck,reader,source,*,sample_rate):
 baseline=reader(deck)[-1][1];result=client.submit(deck,23,flags=2)
 after=source(deck,lambda rows:True,result['receipt_ns']);band=(baseline,after[0][1])
 if compare_timestamps(band[1],band[0],units_per_second=1,sample_rate=sample_rate)<0:raise ValueError('MAIN_SET source moved backwards')
 source(deck,lambda rows:compare_timestamps(rows[-1][1],band[1]+.5,units_per_second=1,sample_rate=sample_rate)>=0)
 result=client.submit(deck,24,flags=2)
 source(deck,lambda rows:len(rows)>=2 and all(source_between(position,band[0],band[1],sample_rate) for _,position in rows[-2:]) and compare_timestamps(rows[-1][1],rows[-2][1],units_per_second=1,sample_rate=sample_rate)==0,result['receipt_ns'])
 return band

def run_main_cue(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 source=lambda deck,predicate,floor=0:wait_cue_source(reader,deck,predicate,floor,clock=clock,sleep=sleep)
 for deck in decks:
  try:
   setup_deck(deck);main_cue_roundtrip(client,deck,reader,source,sample_rate=sample_rate)
   report['decks'][str(deck)]=dict(status='PASS',operations=[23,24],assertions=['capture requested source point','recall seeks to captured point and pauses'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected MAIN_SET and MAIN_RECALL source seek/pause',flush=True)
 return report

def empty_hot_fixture(client,deck):
 s=client.cues(deck)
 def occupied(snapshot,slot):return any(e['classification']==1 and e['entry_id']==slot and e['valid']&1 for e in snapshot['entries'])
 empty=[slot for slot in (7,8) if not occupied(s,slot)]
 if empty:return empty[0]
 # Caller operates solely on the disposable guest USB overlay. Clearing H is
 # deliberate fixture preparation; discarding that overlay restores the base.
 client.submit(deck,19,flags=3,classification=1,entry_id=8)
 fresh=client.cues(deck,lambda newer:newer['native_load']==s['native_load'] and newer['cues_rev']>s['cues_rev'] and not occupied(newer,8))
 if fresh['native_load']!=s['native_load'] or fresh['cues_rev']<=s['cues_rev'] or occupied(fresh,8):raise MissingEvidence('fresh empty Hot H fixture required')
 return 8

def run_hot_clear(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 for deck in decks:
  try:
   setup_deck(deck);before=client.cues(deck)
   occupied=[row for row in before['entries'] if row['classification']==1 and row['entry_id'] in (7,8) and row['valid']&1]
   if not occupied:raise MissingEvidence('occupied stored Hot G/H fixture required for selected clear')
   slot=max(row['entry_id'] for row in occupied)
   target=lambda row:row['classification']==1 and row['entry_id']==slot
   expected=stored_cue_contents([row for row in before['entries'] if not target(row)])
   position=reader(deck)[-1][1]
   result=client.submit(deck,19,flags=3,classification=1,entry_id=slot)
   after=client.cues(deck,lambda state:state['native_load']==before['native_load'] and state['cues_rev']>before['cues_rev'] and not any(target(row) and row['valid']&1 for row in state['entries']))
   if after['native_load']!=before['native_load'] or after['cues_rev']<=before['cues_rev'] or any(target(row) and row['valid']&1 for row in after['entries']):raise ValueError('HOT_CLEAR did not remove the selected stored slot')
   if not cue_values_equal(stored_cue_contents([row for row in after['entries'] if not target(row)]),expected,sample_rate):raise ValueError('HOT_CLEAR changed another stored cue')
   wait_cue_source(reader,deck,lambda rows:len(rows)>=2 and compare_timestamps(rows[-1][1],position+.05,units_per_second=1,sample_rate=sample_rate)>0 and compare_timestamps(rows[-1][1],rows[-2][1],units_per_second=1,sample_rate=sample_rate)>0,result['receipt_ns'],clock=clock,sleep=sleep)
   report['decks'][str(deck)]=dict(status='PASS',operations=[19],cleared_slot=slot,assertions=['stored Hot slot removed after native reply','other stored cues preserved','playing source continues'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected HOT_CLEAR with stored deletion and transport preservation',flush=True)
 return report

def hot_metadata_roundtrip(client,deck,snapshot,entry,*,sample_rate):
 if entry['classification']!=1 or entry['entry_id'] not in range(1,9):raise ValueError('owned Hot fixture required')
 load=snapshot['native_load'];slot=entry['entry_id'];shape=lambda row:dict(classification=row['classification'],entry_id=row['entry_id'],type=row['type'],valid=row['valid']&3,start=row['start'],end=row['end'])
 original=shape(entry);original_color=entry['rgb'];text='emulator hot ✓';color=0xD9842B
 for op,arguments,field,expected in ((25,dict(comment=text),'comment',text),(26,dict(rgb=color),'rgb',color)):
  before_revision=snapshot['cues_rev']
  client.submit(deck,op,flags=7,classification=1,entry_id=slot,revision=before_revision,**arguments)
  snapshot=client.cues(deck,lambda current:current['native_load']==load and current['cues_rev']>before_revision and any(row['classification']==1 and row['entry_id']==slot and row[field]==expected for row in current['entries']))
  found=[row for row in snapshot['entries'] if row['classification']==1 and row['entry_id']==slot]
  if len(found)!=1:raise MissingEvidence('unique owned Hot metadata fixture required')
  entry=found[0]
  if snapshot['native_load']!=load or snapshot['cues_rev']<=before_revision or not cue_values_equal(shape(entry),original,sample_rate):raise ValueError('Hot metadata changed source point/load unexpectedly')
  if entry[field]!=expected or entry['comment']!=text or not entry['valid']&4 or entry['rgb']!=(original_color if op==25 else color):raise ValueError('Hot metadata readback/preservation mismatch')
 return snapshot,entry

def set_hot_point(client,deck,reader,source,slot,*,sample_rate):
 before=client.cues(deck);color=0x2BA6D9
 hot_before=reader(deck)[-1][1];result=client.submit(deck,18,flags=3,classification=1,entry_id=slot,rgb=color)
 hot_after=source(deck,lambda rows:True,result['receipt_ns'])[0][1]
 snapshot=client.cues(deck,lambda state:any(row['classification']==1 and row['entry_id']==slot and row['valid']&1 for row in state['entries']))
 found=[row for row in snapshot['entries'] if row['classification']==1 and row['entry_id']==slot and row['valid']&1]
 if len(found)!=1:raise MissingEvidence('unique native Hot SET readback required')
 created=found[0]
 if not source_between(created['start'],hot_before,hot_after,sample_rate):raise ValueError('Hot SET did not capture actual playhead')
 if created['type']!=1 or created['valid']&2:raise ValueError('Hot SET did not store a point from the inactive-loop fixture')
 if not created['valid']&4 or created['rgb']!=color:raise ValueError('Hot SET stored color mismatch')
 others=lambda state:stored_cue_contents([row for row in state['entries'] if not (row['classification']==1 and row['entry_id']==slot)])
 if snapshot['native_load']!=before['native_load'] or not cue_values_equal(others(snapshot),others(before),sample_rate):raise ValueError('Hot SET changed another stored cue or native load')
 return snapshot,created

def run_hot_set(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 source=lambda deck,predicate,floor=0:wait_cue_source(reader,deck,predicate,floor,clock=clock,sleep=sleep)
 for deck in decks:
  try:
   setup_deck(deck);before=client.cues(deck)
   if before['engaged'] not in (0,1):raise MissingEvidence('native loop engagement required for Hot point fixture')
   if before['engaged']:client.submit(deck,4,flags=2)
   set_hot_point(client,deck,reader,source,8,sample_rate=sample_rate)
   position=reader(deck)[-1][1]
   source(deck,lambda rows:len(rows)>=2 and compare_timestamps(rows[-1][1],position+.05,units_per_second=1,sample_rate=sample_rate)>0 and compare_timestamps(rows[-1][1],rows[-2][1],units_per_second=1,sample_rate=sample_rate)>0)
   report['decks'][str(deck)]=dict(status='PASS',operations=[18],assertions=['stored Hot point captures actual source position','requested stored color','other cues preserved','playing source continues'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected HOT_SET with native position, color and transport witnesses',flush=True)
 return report

def hot_cue_roundtrip(client,deck,reader,source,hot,*,sample_rate):
 slot=empty_hot_fixture(client,deck);before=client.cues(deck)
 client.submit(deck,17,flags=2,classification=1,entry_id=hot['entry_id'])
 snapshot,created=set_hot_point(client,deck,reader,source,slot,sample_rate=sample_rate)
 snapshot,created=hot_metadata_roundtrip(client,deck,snapshot,created,sample_rate=sample_rate)
 client.submit(deck,24,flags=2)
 source(deck,lambda rows:len(rows)>=2 and compare_timestamps(rows[-1][1],rows[-2][1],units_per_second=1,sample_rate=sample_rate)==0)
 result=client.submit(deck,17,flags=2,classification=1,entry_id=slot)
 source(deck,lambda rows:cue_target_seen(rows,created['start'],result['receipt_ns'],sample_rate=sample_rate),result['receipt_ns'])
 playing=reader(deck)[-1][1]
 source(deck,lambda rows:compare_timestamps(rows[-1][1],playing+.05,units_per_second=1,sample_rate=sample_rate)>0)
 client.submit(deck,19,flags=3,classification=1,entry_id=slot)
 final=client.cues(deck,lambda state:not any(row['classification']==1 and row['entry_id']==slot and row['valid']&1 for row in state['entries']))
 others=lambda state:stored_cue_contents([row for row in state['entries'] if not (row['classification']==1 and row['entry_id']==slot)])
 if final['native_load']!=before['native_load'] or not cue_values_equal(others(final),others(before),sample_rate):raise ValueError('Hot cue roundtrip changed another stored cue or native load')
 return final

def run_hot_cues(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 source=lambda deck,predicate,floor=0:wait_cue_source(reader,deck,predicate,floor,clock=clock,sleep=sleep)
 for deck in decks:
  try:
   setup_deck(deck);initial=client.cues(deck)
   hot=[row for row in initial['entries'] if row['classification']==1 and row['entry_id']==1 and row['valid']&1]
   if len(hot)!=1:raise MissingEvidence('existing Hot A fixture required for the Hot cue group')
   client.submit(deck,4,flags=2);client.submit(deck,23,flags=2)
   hot_cue_roundtrip(client,deck,reader,source,hot[0],sample_rate=sample_rate)
   report['decks'][str(deck)]=dict(status='PASS',operations=[17,18,19,25,26],assertions=['Hot point captures source position and color','metadata preserves source point','trigger seeks and starts paused transport','clear removes stored slot','other cues preserved'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected HOT_TRIGGER, HOT_SET, HOT_CLEAR and Hot metadata',flush=True)
 return report

class StaleCueSource(MissingEvidence):
 """A structurally valid, bound packet missed the unchanged freshness cap."""

def fresh_cue_positions(reader,deck,*,sleep=time.sleep,clock=time.monotonic):
 end=clock()+5
 while True:
  try:return reader(deck)
  except StaleCueSource:
   if compare_timestamps(clock(), end, units_per_second=1) >= 0:raise
   sleep(.1)

def source_rows(snapshot,deck,pid,binding,titles):
 packet=snapshot.get('positions');proof=position_motion(packet,deck+1,str(pid),binding,now_boottime_ns=snapshot.get('boottime_ns'))
 if proof.get('reason')=='position packet is stale or from the future':raise StaleCueSource('cue source packet exceeded1s freshness cap')
 if not proof.get('binding_verified') or not proof.get('fresh'):raise MissingEvidence('fresh bound cue source position unavailable')
 rows=[]
 for sample in packet['samples']:
  row=sample['decks'][deck];p=row.get('pos_s')
  if row.get('read_rc')!=0 or row.get('loaded')!=1 or row.get('title') not in titles or type(p) not in (float,int) or not math.isfinite(p):raise MissingEvidence('actual cue track/source unavailable')
  rows.append((sample['native_monotonic_ns'],p))
 return rows

def cue_contents(entries):
 return [{key:value for key,value in entry.items() if key not in ('v','snapshot_seq','deck','load','revision')} for entry in entries]

def cue_values_equal(first,second,sample_rate):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required for cue timestamps')
 if isinstance(first,dict) and isinstance(second,dict):
  return first.keys()==second.keys() and all(compare_timestamps(first[key],second[key],units_per_second=1,sample_rate=sample_rate)==0 if key in ('start','end','loop_in','loop_out','main_s') else cue_values_equal(first[key],second[key],sample_rate) for key in first)
 if isinstance(first,list) and isinstance(second,list) and all(isinstance(row,dict) for row in first+second):
  if len(first)!=len(second):return False
  # Window equivalence is not transitive, so do not sort timestamps or greedily
  # pair near points. Require a one-to-one match of the complete cue records.
  matched=[-1]*len(second)
  def assign(index,seen):
   for other,row in enumerate(second):
    if other in seen or not cue_values_equal(first[index],row,sample_rate):continue
    seen.add(other)
    if matched[other]<0 or assign(matched[other],seen):matched[other]=index;return True
   return False
  return all(assign(index,set()) for index in range(len(first)))
 if isinstance(first,(list,tuple)) and isinstance(second,(list,tuple)):
  return len(first)==len(second) and all(cue_values_equal(a,b,sample_rate) for a,b in zip(first,second))
 return first==second

def require_memory_contents(snapshot,expected,*,sample_rate):
 matches=[entry for entry in snapshot['entries'] if entry['comment']==expected['comment']]
 if len(matches)!=1 or not cue_values_equal({key:matches[0][key] for key in expected if key!='entry_id'},{key:value for key,value in expected.items() if key!='entry_id'},sample_rate):raise ValueError('persisted Memory cue contents differ from the saved UTF8/color fixture')
 return matches[0]

def memory_color_roundtrip(client,deck,snapshot,point,*,sample_rate):
 load=snapshot['native_load'];before_entries=cue_contents(snapshot['entries']);revision=snapshot['cues_rev'];before=dict(point)
 arguments=dict(flags=7,classification=2,entry_id=point['entry_id'],revision=revision)
 client.submit(deck,26,**arguments,rgb=0x2BA6D9,expect_unsupported=True)
 unchanged=client.cues(deck)
 if unchanged['native_load']!=load or unchanged['cues_rev']!=revision or not cue_values_equal(cue_contents(unchanged['entries']),before_entries,sample_rate):raise ValueError('unsupported Memory RGB mutated native cues')
 print('PASS unsupported Memory RGB 0x2ba6d9 rejected; cue collection and revision unchanged',flush=True)
 client.submit(deck,26,**arguments,rgb=0x00C3FF)
 snapshot=client.cues(deck,lambda state:state['cues_rev']>revision and any(e['classification']==2 and e['entry_id']==point['entry_id'] and e['valid']&4 and e['rgb']==0x00C3FF for e in state['entries']))
 matches=[e for e in snapshot['entries'] if e['classification']==2 and e['entry_id']==point['entry_id']]
 if len(matches)!=1 or snapshot['native_load']!=load:raise ValueError('Memory COLOR native association changed')
 point=matches[0]
 if not cue_values_equal({key:point[key] for key in ('classification','entry_id','slot','type','start','end','comment','auto_active') if key in before},{key:before[key] for key in ('classification','entry_id','slot','type','start','end','comment','auto_active') if key in before},sample_rate):raise ValueError('Memory COLOR did not preserve prior UTF8 comment/source point/type')
 original_others=[entry for entry in before_entries if (entry['classification'],entry['entry_id'])!=(2,point['entry_id'])]
 after_others=cue_contents([entry for entry in snapshot['entries'] if (entry['classification'],entry['entry_id'])!=(2,point['entry_id'])])
 if not cue_values_equal(original_others,after_others,sample_rate):raise ValueError('Memory COLOR changed another cue')
 print('PASS Memory RGB 0x00c3ff read from native saved cue; UTF8 comment and other cue fields preserved',flush=True)
 return snapshot,point

def run_memory_color(read_log,send_request,health,pid,*,setup_deck,reload_deck=None,decks=(0,),sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 if reload_deck is not None and not callable(reload_deck):raise ValueError('explicit native track reload callback required')
 client=CueClient(read_log,send_request,health,pid);report=dict(status='PASS',errors=[],missing=[],decks={},reload_persistence='NOT_RUN')
 try:
  for deck in decks:
   setup_deck(deck);initial=client.cues(deck)
   client.submit(deck,23,flags=2);client.submit(deck,24,flags=2)
   client.submit(deck,21,flags=3,classification=2)
   snapshot=client.cues(deck,lambda state:state['cues_rev']>initial['cues_rev'] and len(state['entries'])==len(initial['entries'])+1)
   shape=lambda entry:{key:entry[key] for key in ('classification','type','start','end')}
   original=[shape(entry) for entry in initial['entries']]
   added=[entry for entry in snapshot['entries'] if not any(cue_values_equal(shape(entry),prior,sample_rate) for prior in original)]
   if len(added)!=1 or added[0]['classification']!=2 or added[0]['type']!=1:raise MissingEvidence('one disposable Memory cue fixture required')
   point=added[0];text='emulator cue café ✓ 🎵'
   client.submit(deck,25,flags=7,classification=2,entry_id=point['entry_id'],revision=snapshot['cues_rev'],comment=text)
   snapshot=client.cues(deck,lambda state:any(entry['comment']==text for entry in state['entries']))
   point=next(entry for entry in snapshot['entries'] if entry['comment']==text)
   snapshot,point=memory_color_roundtrip(client,deck,snapshot,point,sample_rate=sample_rate)
   expected=cue_contents([point])[0];load=initial['native_load']
   if snapshot['native_load']!=load:raise ValueError('Memory behavior test changed the loaded track')
   if reload_deck is not None:
    reload_deck(deck)
    snapshot=client.cues(deck,lambda state:state['native_load']!=load)
   point=require_memory_contents(snapshot,expected,sample_rate=sample_rate)
   client.submit(deck,22,flags=7,classification=2,entry_id=point['entry_id'],revision=snapshot['cues_rev'])
   restored=client.cues(deck,lambda state:state['cues_rev']>snapshot['cues_rev'] and len(state['entries'])==len(initial['entries']))
   if restored['native_load']!=snapshot['native_load'] or not cue_values_equal(stored_cue_contents(restored['entries']),stored_cue_contents(initial['entries']),sample_rate):raise ValueError('Memory fixture cleanup changed original cues or the loaded track')
   report['decks'][str(deck)]=dict(status='PASS',cue=expected,fixture_restored=True)
   print('PASS Memory palette color and complete UTF8 cue '+('survive native track reload' if reload_deck is not None else 'verified on the existing track')+' on deck%d; original cues restored'%(deck+1),flush=True)
  if reload_deck is not None:report['reload_persistence']='PASS'
 except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)])
 except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)])
 return report

def same_loop_position(first,second,sample_rate):
 return all(compare_timestamps(a,b,units_per_second=1,sample_rate=sample_rate)==0 for a,b in zip(first,second))

def auto_baselines_equal(first,second,sample_rate):
 return len(first)==len(second) and all(any(same_loop_position(key,other,sample_rate) and value==other_value for other,other_value in second.items()) for key,value in first.items())

def saved_auto_baseline(snapshot,*,sample_rate):
 rows=[e for e in snapshot['entries'] if e['classification']==2 and e['type']==2]
 baseline={(e['start'],e['end']):e['auto_active'] for e in rows}
 if len(baseline)!=len(rows) or any(same_loop_position(a,b,sample_rate) for i,a in enumerate(baseline) for b in list(baseline)[i+1:]) or any(type(value)is not int or value not in (0,1) for value in baseline.values()) or sum(baseline.values())>1:raise MissingEvidence('unique native saved-loop auto baseline required')
 return baseline

def saved_auto_matches(snapshot,active_key,*,sample_rate):
 rows=[e for e in snapshot['entries'] if e['classification']==2 and e['type']==2]
 return all(e['auto_active']==int(active_key is not None and same_loop_position((e['start'],e['end']),active_key,sample_rate)) for e in rows) and (active_key is None or sum(same_loop_position((e['start'],e['end']),active_key,sample_rate) for e in rows)==1)

def restore_saved_auto(client,deck,snapshot,baseline,*,sample_rate):
 current=saved_auto_baseline(snapshot,sample_rate=sample_rate)
 if len(current)!=len(baseline) or any(not any(same_loop_position(key,other,sample_rate) for other in baseline) for key in current):raise ValueError('original saved-loop fixture changed')
 active=next((key for key,value in baseline.items() if value),None)
 if not auto_baselines_equal(current,baseline,sample_rate):
  if active is None:raise ValueError('unexpected active original saved-loop fixture')
  row=next(e for e in snapshot['entries'] if e['classification']==2 and e['type']==2 and same_loop_position((e['start'],e['end']),active,sample_rate))
  client.submit(deck,16,flags=7,classification=2,entry_id=row['entry_id'],revision=snapshot['cues_rev'],position=1)
  snapshot=client.cues(deck,lambda state:state['cues_rev']>snapshot['cues_rev'] and saved_auto_matches(state,active,sample_rate=sample_rate))
 if not auto_baselines_equal(saved_auto_baseline(snapshot,sample_rate=sample_rate),baseline,sample_rate):raise ValueError('original saved-loop auto state not restored')
 return snapshot

def stored_cue_contents(entries,ignore_auto=False):
 rows=cue_contents(entries)
 for row in rows:
  if row['classification']==2:
   row.pop('entry_id',None)
   if ignore_auto and row['type']==2:row.pop('auto_active',None)
 return rows

def run_memory_comment(read_log,send_request,health,pid,*,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None,entry_id=None):
 """Write and restore one existing Memory comment without playback fixtures."""
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 if entry_id is not None and (type(entry_id)is not int or not 0<entry_id<2**32):raise ValueError('native uint32 Memory entry ID required')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock)
 report=dict(status='PASS',errors=[],missing=[],decks={},fixtures={});stage='select'
 for deck in decks:
  try:
   initial=client.cues(deck);load=initial['native_load']
   candidates=[(index,row) for index,row in enumerate(initial['entries']) if row['classification']==2 and row['type']==1 and (entry_id is None or row['entry_id']==entry_id)]
   if not candidates:raise MissingEvidence('existing native Memory point required for comment-only case')
   index,target=candidates[0];before=stored_cue_contents(initial['entries'])
   report['fixtures'][str(deck)]=dict(before=initial,target=target)
   comment='SDK comment: café ✓ 🎵' if target['comment']!='SDK comment: café ✓ 🎵' else 'SDK comment again: café ✓ 🎵'
   expected=[dict(row,comment=comment) if position==index else dict(row) for position,row in enumerate(initial['entries'])]
   def selected(value):
    if value['native_load']!=load:raise ValueError('comment fixture native load changed')
    matches=[row for row in value['entries'] if row['classification']==2 and row['type']==1 and compare_timestamps(row['start'],target['start'],units_per_second=1,sample_rate=sample_rate)==0]
    if len(matches)!=1:raise MissingEvidence('unique native Memory point required after comment update')
    return matches[0]
   stage='write';client.submit(deck,25,load=load,flags=7,classification=2,entry_id=target['entry_id'],revision=initial['cues_rev'],comment=comment)
   changed=client.cues(deck,lambda value:selected(value)['comment']==comment)
   if not cue_values_equal(stored_cue_contents(changed['entries']),stored_cue_contents(expected),sample_rate):raise ValueError('Memory comment changed another stored cue field')
   stage='restore';target_now=selected(changed)
   client.submit(deck,25,load=load,flags=7,classification=2,entry_id=target_now['entry_id'],revision=changed['cues_rev'],comment=target['comment'])
   client.cues(deck,lambda value:cue_values_equal(stored_cue_contents(value['entries']),before,sample_rate))
   report['decks'][str(deck)]=dict(status='PASS',operations=[25],assertions=['requested UTF8 comment observed in native storage','other cue fields and entries preserved','original comment restored with a fresh native ID/revision'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)],failure_stage=stage);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)],failure_stage=stage);break
 if report['status']=='PASS':print('PASS selected Memory comment write and restore; other cue fields preserved',flush=True)
 return report

def run_memory_set(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 """Exercise MEMORY_SET with one main point and remove its disposable entry."""
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 source=lambda deck,predicate,floor=0:wait_cue_source(reader,deck,predicate,floor,clock=clock,sleep=sleep)
 for deck in decks:
  try:
   setup_deck(deck);initial=client.cues(deck)
   main_cue_roundtrip(client,deck,reader,source,sample_rate=sample_rate)
   before=client.cues(deck)
   # MAIN_RECALL above independently observed this exact paused source point.
   # The legacy scalar loop snapshot need not expose a main-cue coordinate.
   target=native_saved_position(reader(deck)[-1][1],sample_rate)
   client.submit(deck,21,flags=3,classification=2)
   after=client.cues(deck,lambda state:state['cues_rev']>before['cues_rev'] and len(state['entries'])==len(before['entries'])+1)
   original=stored_cue_contents(initial['entries'])
   candidates=[entry for index,entry in enumerate(after['entries']) if entry['classification']==2 and entry['type']==1 and entry['saved']==1 and compare_timestamps(entry['start'],target,units_per_second=1,sample_rate=sample_rate)==0 and cue_values_equal(stored_cue_contents(after['entries'][:index]+after['entries'][index+1:]),original,sample_rate)]
   if after['native_load']!=before['native_load'] or len(candidates)!=1:raise ValueError('MEMORY_SET native point or original cue collection differs')
   point=candidates[0]
   client.submit(deck,22,flags=7,classification=2,entry_id=point['entry_id'],revision=after['cues_rev'])
   final=client.cues(deck,lambda state:state['cues_rev']>after['cues_rev'] and len(state['entries'])==len(initial['entries']))
   if not cue_values_equal(stored_cue_contents(final['entries']),original,sample_rate):raise ValueError('MEMORY_SET fixture cleanup changed original cues')
   report['decks'][str(deck)]=dict(status='PASS',operations=[21],assertions=['native saved main point','other cue contents preserved','disposable point deleted'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected MEMORY_SET native saved point and original cue preservation',flush=True)
 return report

def run_active_loop_switch(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 """Test SAVED_AUTO only; reuse saved loops or make two minimal prerequisites."""
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock)
 report=dict(status='PASS',errors=[],missing=[],decks={})
 def key(row):return (row['start'],row['end'])
 def loops(snapshot):return [row for row in snapshot['entries'] if row['classification']==2 and row['type']==2]
 for deck in decks:
  initial=None;baseline=None;created=[];stage='setup';load=None
  def fresh():
   value=client.cues(deck)
   if load is not None and value['native_load']!=load:raise MissingEvidence('Active Loop fixture native load changed')
   return value
  def selected(snapshot,target):
   matches=[row for row in loops(snapshot) if same_loop_position(key(row),target,sample_rate)]
   if len(matches)!=1:raise MissingEvidence('unique saved-loop fixture required')
   return matches[0]
  def auto(target,enabled,verify=True):
   before=fresh();row=selected(before,target)
   client.submit(deck,16,load=load,flags=7,classification=2,entry_id=row['entry_id'],revision=before['cues_rev'],position=enabled)
   after=client.cues(deck,lambda value:value['native_load']==load and value['cues_rev']>before['cues_rev'])
   if not cue_values_equal(stored_cue_contents(before['entries'],True),stored_cue_contents(after['entries'],True),sample_rate):raise ValueError('SAVED_AUTO changed other stored cue fields')
   if verify and not saved_auto_matches(after,target if enabled else None,sample_rate=sample_rate):
    actual=[(key(row),row['auto_active']) for row in loops(after)]
    raise ValueError('SAVED_AUTO '+stage+' expected sole active '+str(target if enabled else None)+'; got '+str(actual))
   return after
  try:
   setup_deck(deck);initial=fresh();load=initial['native_load'];baseline=saved_auto_baseline(initial,sample_rate=sample_rate)
   existing=loops(initial);needed=max(0,2-len(existing))
   if sum(row['classification']==2 for row in initial['entries'])+needed>10:raise MissingEvidence('two saved-loop fixtures need free Memory slots; original cues are preserved')
   hot=[]
   for row in initial['entries']:
    if row['classification']==1 and row['type']==1 and row['valid']&1 and all(compare_timestamps(row['start'],prior['start'],units_per_second=1,sample_rate=sample_rate)!=0 for prior in hot):hot.append(row)
   if len(hot)<needed:raise MissingEvidence('distinct native Hot points required only to create missing loop fixtures')
   for index in range(needed):
    client.submit(deck,17,load=load,flags=2,classification=1,entry_id=hot[index]['entry_id'])
    if fresh()['engaged']==1:client.submit(deck,4,load=load,flags=2)
    client.submit(deck,1,load=load,flags=2)
    positions=fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock);start=positions[-1][1];end=clock()+90
    while compare_timestamps(fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)[-1][1],start+.05,units_per_second=1,sample_rate=sample_rate)<0:
     if compare_timestamps(clock(), end, units_per_second=1) >= 0:raise MissingEvidence('native playback advance needed for saved-loop fixture')
     sleep(.1)
    client.submit(deck,2,load=load,flags=2);working=fresh()
    if not working['loop_valid'] or working['engaged']!=1 or compare_timestamps(working['loop_in'],working['loop_out'],units_per_second=1,sample_rate=sample_rate)>=0:raise MissingEvidence('native working loop required for saved-loop fixture')
    target=tuple(native_saved_position(working[field],sample_rate) for field in ('loop_in','loop_out'))
    if any(same_loop_position(key(row),target,sample_rate) for row in loops(working)):raise MissingEvidence('new saved-loop fixture would overlap an original loop')
    created.append(target)
    client.submit(deck,14,load=load,flags=3,classification=2)
    saved=client.cues(deck,lambda value:value['native_load']==load and value['cues_rev']>working['cues_rev'])
    selected(saved,target)
   fixtures=sorted(loops(fresh()),key=lambda row:row['auto_active'])[:2]
   a,b=map(key,fixtures)
   stage='enable';auto(a,1)
   stage='switch';auto(b,1)
   stage='disable';auto(b,0)
   report['decks'][str(deck)]=dict(status='PASS',operations=[16],assertions=['enable sole loop','switch active loop A to B','disable automatic activation'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)],failure_stage=stage)
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)],failure_stage=stage)
  finally:
   if baseline is not None:
    try:
     submitted=[row for row in client.rows if row['event']=='submit' and row['rc']==0 and row['id']]
     if submitted:
      identity=submitted[-1]['id']
      if not any(row['event']=='release' and row['id']==identity and row['rc']==0 for row in client.rows):client.wait(lambda row:row['event']=='release' and row['id']==identity and row['rc']==0)
     state=fresh()
     if created and state['engaged']==1:client.submit(deck,4,load=load,flags=2)
     for target in reversed(created):
      state=fresh();matches=[row for row in loops(state) if same_loop_position(key(row),target,sample_rate)]
      if not matches:continue
      row=selected(state,target)
      client.submit(deck,15,load=load,flags=7,classification=2,entry_id=row['entry_id'],revision=state['cues_rev'])
      client.cues(deck,lambda value:not any(same_loop_position(key(row),target,sample_rate) for row in loops(value)))
     state=fresh()
     for row in list(loops(state)):
      if row['auto_active'] and not any(same_loop_position(key(row),original,sample_rate) and active for original,active in baseline.items()):state=auto(key(row),0,verify=False)
     state=restore_saved_auto(client,deck,state,baseline,sample_rate=sample_rate)
     if not cue_values_equal(stored_cue_contents(state['entries']),stored_cue_contents(initial['entries']),sample_rate):raise ValueError('Active Loop fixture cleanup did not restore original cues')
     report.setdefault('cleanup',{})[str(deck)]=dict(status='PASS')
    except Exception as error:
     report.setdefault('errors',[]).append('Active Loop fixture cleanup: '+str(error));report['status']='FAIL'
  if report['status']!='PASS':break
 if report['status']=='PASS':print('PASS selected SAVED_AUTO enable, active-loop switch, and disable; original cues restored',flush=True)
 return report

def native_saved_position(seconds,sample_rate):
 """Project a working source sample through the native persisted-usec format.

 Both native CueProperty setters round the cached usec with FCVTAS. Persistence
 reconstructs its source sample by truncating double(Fs*usec)*1e-6. Compare the
 projected coordinate with the ordinary one-sample window, without widening it.
 """
 if type(sample_rate)is not int or sample_rate<=0 or type(seconds) not in (int,float) or not math.isfinite(seconds):raise MissingEvidence('bound finite saved-cue source clock required')
 samples=round(seconds*sample_rate)
 if not 0<=samples<=2**53:raise MissingEvidence('saved-cue source sample exceeds native precision')
 usec=math.floor((1000000./sample_rate)*samples+.5)
 if not 0<=sample_rate*usec<2**64:raise MissingEvidence('saved-cue microsecond projection exceeds native integer range')
 return int(float(sample_rate*usec)*.000001)/sample_rate

def saved_recall_roundtrip(client,deck,snapshot,loop,source,*,sample_rate):
 a,b=loop['start'],loop['end']
 recalled=client.submit(deck,13,flags=6,classification=2,entry_id=loop['entry_id'],revision=snapshot['cues_rev'])
 client.cues(deck,lambda state:state['loop_valid'] and state['engaged']==1 and same_loop_position((state['loop_in'],state['loop_out']),(a,b),sample_rate))
 source(deck,lambda rows:len(rows)>=2 and all(compare_timestamps(position,a,units_per_second=1,sample_rate=sample_rate)==0 for _,position in rows[-2:]),recalled['receipt_ns'])

def run_saved_recall(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 source=lambda deck,predicate,floor=0:wait_cue_source(reader,deck,predicate,floor,clock=clock,sleep=sleep)
 for deck in decks:
  try:
   setup_deck(deck);before=client.cues(deck)
   loops=[row for row in before['entries'] if row['classification']==2 and row['type']==2 and row['valid']&3==3]
   if not loops:raise MissingEvidence('existing saved-loop fixture required for selected recall')
   saved_recall_roundtrip(client,deck,before,loops[0],source,sample_rate=sample_rate)
   after=client.cues(deck)
   if not cue_values_equal(stored_cue_contents(before['entries']),stored_cue_contents(after['entries']),sample_rate):raise ValueError('Saved Recall changed the saved cue collection')
   report['decks'][str(deck)]=dict(status='PASS',operations=[13],assertions=['stored bounds engaged','source seeks to stored IN and pauses','saved cue collection unchanged'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected SAVED_RECALL with native bounds, source seek and pause',flush=True)
 return report

def saved_loop_roundtrip(client,deck,before,source,hot,*,sample_rate):
 if not before['loop_valid'] or before['engaged']!=1 or compare_timestamps(before['loop_in'],before['loop_out'],units_per_second=1,sample_rate=sample_rate)>=0:raise MissingEvidence('complete actual working loop required')
 a,b=(native_saved_position(before[field],sample_rate) for field in ('loop_in','loop_out'))
 baseline=saved_auto_baseline(before,sample_rate=sample_rate)
 match=lambda e:e['classification']==2 and e['type']==2 and same_loop_position((e['start'],e['end']),(a,b),sample_rate)
 if any(match(row) for row in before['entries']):raise MissingEvidence('new saved-loop fixture would overlap an original loop')
 client.submit(deck,14,flags=3,classification=2)
 state=client.cues(deck,lambda state:state['cues_rev']>before['cues_rev'] and len(state['entries'])==len(before['entries'])+1)
 def selected(snapshot):
  rows=[row for row in snapshot['entries'] if match(row)]
  if len(rows)!=1:raise ValueError('Saved Loop bounds differ from native persisted projection: expected=%r actual=%r'%((a,b),[(row['start'],row['end']) for row in snapshot['entries'] if row['classification']==2 and row['type']==2]))
  return rows[0]
 loop=selected(state)
 def metadata(snapshot,row,persist=True):return dict(flags=7 if persist else 6,classification=2,entry_id=row['entry_id'],revision=snapshot['cues_rev'])
 for active in (1,0):
  client.submit(deck,16,**metadata(state,loop),position=active)
  state=client.cues(deck,lambda state:any(match(row) and row['auto_active']==active for row in state['entries']) and saved_auto_matches(state,(a,b) if active else None,sample_rate=sample_rate));loop=selected(state)
 client.submit(deck,4,flags=2);client.submit(deck,17,flags=2,classification=1,entry_id=hot['entry_id']);state=client.cues(deck);loop=selected(state)
 saved_recall_roundtrip(client,deck,state,loop,source,sample_rate=sample_rate)
 client.submit(deck,4,flags=2);state=client.cues(deck);loop=selected(state);client.submit(deck,15,**metadata(state,loop))
 state=client.cues(deck,lambda state:not any(match(row) for row in state['entries']))
 restore_saved_auto(client,deck,state,baseline,sample_rate=sample_rate)
 return (a,b)

def run_saved_loop(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 client=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=lambda deck:fresh_cue_positions(read_positions,deck,sleep=sleep,clock=clock)
 source=lambda deck,predicate,floor=0:wait_cue_source(reader,deck,predicate,floor,clock=clock,sleep=sleep)
 for deck in decks:
  try:
   setup_deck(deck);initial=client.cues(deck)
   hot=[row for row in initial['entries'] if row['classification']==1 and row['entry_id']==1 and row['valid']&1]
   if len(hot)!=1:raise MissingEvidence('original Hot A fixture required')
   client.submit(deck,4,flags=2);client.submit(deck,1,flags=2)
   start=reader(deck)[-1][1];source(deck,lambda rows:compare_timestamps(rows[-1][1],start+.05,units_per_second=1,sample_rate=sample_rate)>0)
   client.submit(deck,2,flags=2)
   saved_loop_roundtrip(client,deck,client.cues(deck),source,hot[0],sample_rate=sample_rate)
   final=client.cues(deck)
   if not cue_values_equal(stored_cue_contents(initial['entries']),stored_cue_contents(final['entries']),sample_rate):raise ValueError('Saved Loop fixture cleanup did not restore original cues')
   report['decks'][str(deck)]=dict(status='PASS',operations=[13,14,15,16],assertions=['native persisted source bounds','sole active/disabled loop','recall engages stored bounds, seeks to IN and pauses','delete removes saved loop','original cues restored'])
  except MissingEvidence as error:report.update(status='INCOMPLETE',missing=[str(error)]);break
  except (ValueError,RuntimeError) as error:report.update(status='FAIL',errors=[str(error)]);break
 if report['status']=='PASS':print('PASS selected SAVED_SAVE, SAVED_AUTO, SAVED_RECALL and SAVED_DELETE',flush=True)
 return report

def run_commands(read_log,send_request,health,pid,*,setup_deck,read_positions,decks=(0,),sleep=time.sleep,clock=time.monotonic,sample_rate=None):
 if type(sample_rate)is not int or sample_rate<=0:raise MissingEvidence('bound source sample rate required before cue commands')
 c=CueClient(read_log,send_request,health,pid,sleep=sleep,clock=clock);report=dict(status='PASS',errors=[],missing=[],decks={})
 reader=read_positions
 read_positions=lambda deck:fresh_cue_positions(reader,deck,sleep=sleep,clock=clock)
 def source(deck,predicate,floor=0):
  return wait_cue_source(read_positions,deck,predicate,floor,clock=clock,sleep=sleep)
 def selected(s,matcher):
  found=[e for e in s['entries'] if matcher(e)]
  if len(found)!=1:raise MissingEvidence('unique native cue fixture required')
  return found[0]
 def metadata(s,row,persist=True):return dict(flags=7 if persist else 6,classification=row['classification'],entry_id=row['entry_id'],revision=s['cues_rev'])
 try:
  for deck in decks:
   setup_deck(deck);initial=c.cues(deck);load=initial['native_load']
   hot=selected(initial,lambda e:e['classification']==1 and e['entry_id']==1 and e['valid']&1)
   band=main_cue_roundtrip(c,deck,read_positions,source,sample_rate=sample_rate)
   result=c.submit(deck,17,flags=2,classification=1,entry_id=hot['entry_id']);source(deck,lambda rows:cue_target_seen(rows,hot['start'],result['receipt_ns'],sample_rate=sample_rate),result['receipt_ns']);start=read_positions(deck)[-1][1];source(deck,lambda rows:compare_timestamps(rows[-1][1],start+.05,units_per_second=1,sample_rate=sample_rate)>0)
   c.submit(deck,24,flags=2);before=c.cues(deck);c.submit(deck,21,flags=3,classification=2)
   s=c.cues(deck,lambda s:s['cues_rev']>before['cues_rev'] and len(s['entries'])==len(before['entries'])+1)
   point=selected(s,lambda e:e['classification']==2 and e['type']==1 and source_between(e['start'],band[0],band[1],sample_rate))
   text='emulator cue ✓';c.submit(deck,25,**metadata(s,point),comment=text)
   s=c.cues(deck,lambda s:any(e['comment']==text for e in s['entries']));point=selected(s,lambda e:e['comment']==text)
   s,point=memory_color_roundtrip(c,deck,s,point,sample_rate=sample_rate);color=0x2BA6D9
   c.submit(deck,17,flags=2,classification=1,entry_id=hot['entry_id']);s=c.cues(deck);point=selected(s,lambda e:e['comment']==text)
   result=c.submit(deck,20,**metadata(s,point,False));source(deck,lambda rows:len(rows)>=2 and all(compare_timestamps(position,point['start'],units_per_second=1,sample_rate=sample_rate)==0 for _,position in rows[-2:]),result['receipt_ns'])
   s=c.cues(deck);point=selected(s,lambda e:e['comment']==text);c.submit(deck,22,**metadata(s,point));c.cues(deck,lambda s:not any(e['comment']==text for e in s['entries']))
   c.submit(deck,17,flags=2,classification=1,entry_id=hot['entry_id']);c.submit(deck,23,flags=2);c.submit(deck,4,flags=2);c.submit(deck,1,flags=2);start=read_positions(deck)[-1][1];source(deck,lambda rows:compare_timestamps(rows[-1][1],start+.5,units_per_second=1,sample_rate=sample_rate)>0);c.submit(deck,2,flags=2);c.submit(deck,9,flags=2,beats=1)
   saved_loop_roundtrip(c,deck,c.cues(deck),source,hot,sample_rate=sample_rate)
   final=hot_cue_roundtrip(c,deck,read_positions,source,hot,sample_rate=sample_rate)
   if final['native_load']!=load:raise ValueError('cue fixture load changed')
   completed=completed_cue_operations(c.evidence,deck)
   if completed!=list(OPS):raise MissingEvidence('all14 native cue operations required')
   report['decks'][str(deck)]=dict(status='PASS',operations=completed)
   print('PASS all14 public native cue commands with actual source/storage witnesses',flush=True)
 except MissingEvidence as e:report.update(status='INCOMPLETE',missing=[str(e)])
 except (ValueError,RuntimeError) as e:report.update(status='FAIL',errors=[str(e)])
 return report

def completed_cue_operations(evidence,deck):
 """Command coverage belongs to the deck in each qualified SDK receipt."""
 return sorted({row['operation'] for row in evidence if row['operation'] in OPS and row['terminal']['deck']==deck})

def prepare_cue_decks_before_sdk(decks,setup,close,verify):
 decks=tuple(decks)
 if not decks or len(set(decks))!=len(decks) or any(type(deck)is not int or deck not in range(4) for deck in decks):raise ValueError('explicit unique cue decks required')
 prepared=False
 def ready(deck):
  nonlocal prepared
  if deck not in decks:raise ValueError('cue deck outside prepared fixture')
  if not prepared:
   for candidate in decks:setup(candidate)
   close()
   prepared=True
  return verify(deck)
 return ready

def reload_selected_cue_track(controller,sessions,prepare_selection,deck,titles):
 logical=deck+1
 sessions.renew('browse-reload-deck%d'%logical,neutral=controller.panel_inputs_neutral())
 try:controller.driver.panel('browse',hold=.15)
 finally:controller._release(controller.driver.BUS_MIXER)
 selection=prepare_selection(controller,logical)
 if not isinstance(selection,dict) or selection.get('status')!='PASS' or selection.get('selected_title') not in titles or not isinstance(selection.get('evidence'),str):raise MissingEvidence('current independently observed reload selection required')
 # LOAD on the waveform page does not establish a fresh native load. Require
 # the browser selection, then renew after any touchscreen navigation wait.
 sessions.renew('reload-deck%d'%logical,neutral=controller.panel_inputs_neutral())
 controller.load(logical)

def finalize_cue_session(controller,sessions,health):
 if not controller.panel_inputs_neutral():raise MissingEvidence('acknowledged neutral panel boundary required')
 sessions.close()
 if not health():raise MissingEvidence('owned cue process unhealthy after panel close')
 return dict(status='PASS',panel_closed=True,process_healthy=True)

def require_empty_cue_restart(controller,client,decks):
 controller.wait(lambda value:value['mask']=='EEEE' and all(not row['loaded'] for row in value['decks']),'all native decks unloaded before restart')
 for deck in decks:
  if client.snapshot(deck,entries=False)['load_valid']:raise MissingEvidence('native SDK load still valid after unload')
 return dict(status='PASS',decks_unloaded=True)


def validate_cold_cue_approval(cold,owned,manifest,probe_sha,*,boot_id,native_pid,trust,normal_probe_sha):
 """Require retained normal-UI trust and a distinct, empty native boot."""
 from sdk_emulator_public import approved
 if not isinstance(cold,dict) or not isinstance(cold.get('approval'),dict):raise ValueError('retained cold approval required')
 prior=cold['approval'];stopped=cold.get('stopped_identity')
 # The unchanged normal module retains its actual UI approval. A requested
 # core rebuild is independently source/hash validated before this guard; it
 # can replace the provider build ID while preserving the position ABI/rate.
 old_artifacts=prior.get('artifact_sha256',{})
 same_core=isinstance(old_artifacts,dict) and old_artifacts.keys()==manifest['sha256'].keys() and all(old_artifacts[name]==sha for name,sha in manifest['sha256'].items() if name not in ('loop-probe.so','source-probe.so'))
 updated_core=cold.get('updated_core',False)
 if type(updated_core)is not bool:raise ValueError('explicit updated-core cold transition required')
 old_binding=prior.get('position_binding',{});new_binding=manifest['position_binding']
 same_abi=isinstance(old_binding,dict) and old_binding.keys()==new_binding.keys() and all(old_binding[key]==value for key,value in new_binding.items() if key!='provider_build_id')
 candidate_bound=(same_core and old_binding==new_binding) or (updated_core and same_abi and isinstance(old_artifacts,dict) and old_artifacts.keys()==manifest['sha256'].keys())
 previous=cold.get('previous_activation')
 retained_activation=(isinstance(previous,dict) and set(previous)=={'status','boot_id','pid','birth','mapped_probe_sha256'} and
                      previous['status']=='PASS' and previous['boot_id']==cold.get('previous_boot_id') and
                      isinstance(stopped,dict) and all(previous[key]==stopped.get(key) for key in ('pid','birth')) and
                      previous['mapped_probe_sha256']==probe_sha)
 approval_boot=prior.get('approved_in_boot_id')
 cold_activation=(prior.get('ui_reload') is False and prior.get('activation')=='cold_boot' and
                  re.fullmatch('[a-f0-9-]{36}',str(approval_boot)) and approval_boot!=boot_id and
                  (approval_boot==cold.get('previous_boot_id') or retained_activation))
 activation=(prior.get('ui_reload') is True and prior.get('reload_request')=='apply') or cold_activation
 if (cold.get('container')!=owned.get('container') or cold.get('token')!=owned.get('token') or
     not re.fullmatch('[a-f0-9-]{36}',str(cold.get('previous_boot_id',''))) or
     not re.fullmatch('[a-f0-9-]{36}',str(boot_id)) or boot_id==cold['previous_boot_id'] or
     cold.get('boot_id')!=boot_id or native_pid or
     not isinstance(stopped,dict) or set(stopped)!={'pid','birth'} or
     any(not re.fullmatch('[1-9][0-9]{0,19}',str(stopped[key])) for key in ('pid','birth')) or
     prior.get('status')!='PASS' or prior.get('errors') or prior.get('missing') or
     prior.get('ui_allow') is not True or not activation or prior.get('approved_sha256')!=probe_sha or
     not candidate_bound or
     normal_probe_sha!=probe_sha or not approved(trust,ID,probe_sha)):
  raise ValueError('cold cue boot changed ownership, boot, native process, UI approval or artifacts')
 return dict(stopped)

def validate_cue_observation(loop, source, health_rows, identity, manifest, firmware_sha, controller_report, *unused):
    report = dict(controller_report or dict(status='INCOMPLETE', missing=['controller did not produce command report']))
    report['health'] = validate_health(health_rows)
    errors = list(report.get('errors', []))
    missing = list(report.get('missing', []))
    if report['health']['status'] == 'FAIL':
        errors += report['health'].get('errors', [])
    elif report['health']['status'] != 'PASS':
        missing.append('stable SDK runtime health')
    if len(health_rows) < 2 or compare_timestamps(health_rows[-1]['local_monotonic'] - health_rows[0]['local_monotonic'], 120, units_per_second=1) < 0:
        missing.append('120-second stable SDK runtime observation')
    if not identity.get('pid'):
        missing.append('mapped native process identity not captured')
    elif health_rows and identity['pid'] != health_rows[0].get('pid'):
        errors.append('mapped native process identity mismatch')
    mapped=identity.get('mapped', {})
    for name, expected in manifest['sha256'].items():
        if name not in mapped:
            missing.append('mapped core/diagnostic hash not captured ' + name)
        elif mapped[name] != expected:
            errors.append('mapped core/diagnostic hash mismatch ' + name)
    if not identity.get('firmware_sha256'):
        missing.append('actual firmware hash not captured')
    elif identity['firmware_sha256'] != firmware_sha:
        errors.append('actual firmware hash mismatch')
    report.update(status='FAIL' if errors else 'INCOMPLETE' if missing else report.get('status', 'INCOMPLETE'), errors=errors, missing=missing)
    return report

def _run_cue_runtime(ssh, package, source_root, output, firmware, seconds, command, context):
    """Run a normal-loader command client in an explicitly owned disposable VM.

 prepare_allow(controller,read_guest,sessions) performs actual UI Allow/reload.
 prepare_deck(controller,logical_deck) selects a test track and returns a PASS
 selection with exact selected_title and retained screenshot evidence. This
 adapter, rather than the callback, issues the single LOAD and setup PLAY.
 """
    validate_test_selection(context.get('test_phase','full'),context.get('selection'))
    PanelTransport._strict_ssh(ssh)
    owned = dict(context)
    debug = context.get('debug', False)
    if type(debug) is not bool:
        raise ValueError('debug must be an explicit boolean')
    if context.get('on_failure') is not None and (not callable(context['on_failure'])):
        raise ValueError('on_failure callback must be callable')
    if context.get('prepare_restart') is not None and not callable(context['prepare_restart']):
        raise ValueError('normal UI restart preparation callback required')
    if not callable(context.get('prepare_allow')) or not callable(context.get('prepare_deck')):
        raise ValueError('normal Allow and per-deck selection callbacks required')
    if not re.fullmatch('[a-f0-9]{32}', str(owned.get('token', ''))) or not re.fullmatch('az-sdk-automation-[a-z0-9-]+', str(owned.get('container', ''))):
        raise ValueError('one owned disposable emulator required')
    if type(seconds) is not int or not 240 <= seconds <= 1500:
        raise ValueError('bounded normal-loader runtime required')
    manifest = runtime.validate_package(package, source_root)
    DRIVER = Path(context['driver_path'])
    RECEIVER = Path(context['receiver_path'])
    for path, key in ((DRIVER, 'driver_sha256'), (RECEIVER, 'receiver_sha256')):
        if path.is_symlink() or not path.is_file() or sha(path) != context[key]:
            raise ValueError('bound driver/receiver required')
    titles = context.get('expected_titles')
    if not isinstance(titles, (list, tuple)) or not 1 <= len(titles) <= 8 or any((not isinstance(x, str) or not 1 <= len(x) <= 512 for x in titles)):
        raise ValueError('explicit exact test titles required')
    usb = context.get('usb_mount', '/media/usb/sda1')
    if not re.fullmatch('/media/usb/[A-Za-z0-9_-]+', usb):
        raise ValueError('one guest USB mount required')
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise ValueError('new cue runtime temporary directory required')
    binding = json.loads((Path(package) / 'runtime.json').read_text())['position_binding']
    probe_manifest = Path(context['probe_manifest'])
    pb = json.loads(probe_manifest.read_text())
    probe_package = Path(context['probe_package'])
    if context.get('position_binding') != binding or sha(Path(source_root) / 'src/azmod/azmod-mod.h') != pb['sha256']['include/azmod-mod.h']:
        raise ValueError('public header/native provider binding mismatch')
    files = probe_archive(probe_package.read_bytes(), pb, mod_id=ID)

    def owner():
        value = command(['docker', 'inspect', '--format', '{{index .Config.Labels "az-sdk-token"}}', owned['container']], timeout=10).decode().strip()
        if value != owned['token']:
            raise ValueError('owned VM identity changed')
        return value
    owner()
    cold=owned.get('cold_boot_approval')
    cold_stopped=None
    if cold is not None:
        from sdk_emulator_public_runtime import APPROVED
        normal='/home/root/settings/az-usbload/community/'+ID+'/'+ID+'.so'
        # This read-only check precedes any startup settings or package writes.
        raw=command(ssh+['set -e\ncat /proc/sys/kernel/random/boot_id\nprintf "__NATIVE__\\n"\npidof EP147 || true\nprintf "__TRUST__\\n"\ncat '+APPROVED+'\nprintf "__BINARY__\\n"\nsha256sum '+normal+'\n'],timeout=15).decode()
        boot,rest=raw.split('__NATIVE__\n',1);pid,rest=rest.split('__TRUST__\n',1);trust,measured=rest.split('__BINARY__\n',1)
        fields=measured.split()
        if len(fields)!=2 or fields[1]!=normal:raise ValueError('retained normal probe unavailable')
        cold_stopped=validate_cold_cue_approval(cold,owned,manifest,pb['binary']['sha256'],boot_id=boot.strip(),native_pid=pid.strip(),trust=trust,normal_probe_sha=fields[0])
    target = usb + '/MODS/community-mods/' + ID
    stage = usb + '/MODS/community-mods/.sdk-loop-' + owned['token']
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w') as archive:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 420
            archive.addfile(info, io.BytesIO(data))
    plan = 'set -e\nawk \'$2=="' + usb + '" && $4 ~ /(^|,)rw(,|$)/ {ok=1} END {exit !ok}\' /proc/mounts\ntest -d ' + usb + '\ntest -w ' + usb + '\ntest ! -e ' + target + '\nmkdir -p ' + usb + '/MODS/community-mods\nmkdir ' + stage + '\ntar -xf - -C ' + stage + '\nmv ' + stage + ' ' + target + '\nsync\nsha256sum ' + ' '.join((target + '/' + name for name in files)) + '\n'
    resuming=owned.get('approval_resume') is not None or cold is not None
    if resuming:
        # The original attempt already installed these exact disposable files.
        # Verify them in place before the public layer rechecks the UI proof.
        plan='set -e\ntest -d '+target+'\ntest ! -L '+target+'\nsha256sum '+' '.join(target+'/'+name for name in files)+'\n'
        staged=command(ssh+[plan],timeout=20).decode()
    else:
        staged = command(ssh + [plan], input=payload.getvalue(), timeout=20).decode()
    actual = {line.split()[1].rsplit('/', 1)[-1]: line.split()[0] for line in staged.splitlines() if len(line.split()) == 2}
    if actual != {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}:
        raise ValueError('real USB staged package hashes mismatch')
    flags = ('azmod.native-exit.on', 'azmod.native-reloop.on', 'azmod.native-loop-commands.on', 'azmod.native-cue-commands.on')
    extra = command(ssh + ['set -e\n'+('' if resuming else 'touch ' + ' '.join(('/home/root/settings/' + flag for flag in flags)) + '\n') + ''.join(('test -f /home/root/settings/' + flag + ' && echo COMMAND_FLAG ' + flag + '\n' for flag in flags))], timeout=10).decode()
    if set(extra.splitlines()) != {'COMMAND_FLAG ' + flag for flag in flags}:
        raise ValueError('command startup gates unavailable')
    adapter_binding = None
    prepare_allow = context['prepare_allow']
    meta = {}
    sessions = [None]
    read_index = [0]
    phase = output / 'loaded'

    def independent(pid, birth):
        owner()
        data = command(ssh + [native_birth_check_script(pid, birth)], timeout=5)
        if data not in (b'AZSDK_NATIVE_BIRTH_MATCH\n', b'AZSDK_NATIVE_BIRTH_REFUSED\n'):
            raise ValueError('native birth check unavailable')
        return data == b'AZSDK_NATIVE_BIRTH_MATCH\n'

    def bounded_read(body, cap=1048576):
        owner()
        data = command(ssh + [read_script(meta['pid'], meta['birth'], body, cap)], timeout=15)
        if len(data) > cap:
            raise ValueError('owned read limit')
        return data.decode()

    def loaded_executor(stopped, remaining, verify_new):

        def factory(pid, birth, guard):
            guard()
            verify_new(pid, birth)
            stage = '/tmp/az-sdk-panel.' + uuid.uuid4().hex
            command(ssh + ['umask 077; mkdir ' + stage + '; cat >' + stage + '/receiver; chmod 700 ' + stage + '/receiver'], input=RECEIVER.read_bytes(), timeout=15)
            meta.update(pid=pid, birth=birth, owner=owned['token'], stage=stage)
            (phase / 'panel-transport.json').write_text(json.dumps(meta, indent=2) + '\n')
            s = PanelTransportSessions(pid, birth, owned['token'], guard, independent, lambda label: PanelTransport(ssh, stage + '/receiver', sha(RECEIVER), pid, birth, owned['token'], guard), max_sessions=6)
            sessions[0] = s
            s.renew('navigation', neutral=True)
            command(ssh + ['test -x /home/root/scripts/usb-renotify.sh; AZ_RENOTIFY_LOG=' + stage + '/renotify.log /home/root/scripts/usb-renotify.sh --until-ack </dev/null >' + stage + '/renotify.stdout 2>&1 &'], timeout=15)
            return s
        captured = {'raw': None, 'fresh': False}

        def capture():
            results = target + '/data/results.' + meta['pid'] + '.log'
            body = 'cat ' + LOG + "; printf '\n__AZSDK_LOOP_RESULTS__\n'; test ! -f " + results + ' || cat ' + results + "; printf '\n__AZSDK_LOOP_HEALTH__\n'; systemctl show az-sdk-runtime.service -p NRestarts -p ActiveState -p SubState"
            data = bounded_read(body)
            core_log, rest = data.split('\n__AZSDK_LOOP_RESULTS__\n', 1)
            result_log, service = rest.split('\n__AZSDK_LOOP_HEALTH__\n', 1)
            if result_log and (not result_log.endswith('\n')):
                result_log = result_log.rsplit('\n', 1)[0] + '\n' if '\n' in result_log else ''
            if ready(core_log, meta['pid'])['data'] != target + '/data':
                raise ValueError('probe data_dir differs from staged USB owner')
            if not all((item in service.splitlines() for item in ('NRestarts=0', 'ActiveState=active', 'SubState=running'))):
                raise ValueError('owned native service identity/health changed')
            raw = core_log + '\n' + result_log
            read_index[0] += 1
            captured.update(raw=raw, fresh=True)
            return raw

        def read_log():
            if not captured['fresh']:
                capture()
            captured['fresh'] = False
            return captured['raw']

        def health():
            capture()
            return True

        def send_request(data):
            directory = target + '/data'
            file = directory + '/.request-' + uuid.uuid4().hex
            script = 'set -e\ntest ! -L ' + directory + '\nfor i in 1 2 3 4 5 6 7 8 9 10; do test ! -e ' + directory + '/request -a ! -e ' + directory + '/request.processing && break; sleep .1; done\ntest ! -e ' + directory + '/request\ntest ! -e ' + directory + '/request.processing\numask 077\ncat >' + file + '\nmv ' + file + ' ' + directory + '/request\n'
            guarded = read_script(meta['pid'], meta['birth'], script, 1024)
            owner()
            command(ssh + [guarded], input=data.encode() if isinstance(data, str) else data, timeout=10)

        def action(controller):
            controller._release(controller.driver.BUS_ERP)
            controller._release(controller.driver.BUS_MIXER)
            path = '/home/root/settings/az-usbload/community/' + ID + '/' + ID + '.so'
            maps = bounded_read('cat /proc/' + meta['pid'] + '/maps')
            if not any((line.split()[-1:] == [path] for line in maps.splitlines())):
                raise MissingEvidence('normal loader mapping absent')
            if bounded_read('sha256sum ' + path, 4096).split() != [pb['binary']['sha256'], path]:
                raise ValueError('loaded probe hash mismatch')

            memory_phase=owned.get('test_phase')=='memory-color-reload' or (owned.get('test_phase')=='selected' and owned.get('selection')=='memory-color-reload')
            def setup_deck(deck):
                logical = deck + 1
                if not memory_phase:
                    ensure_quantize_off(controller)
                selection = context['prepare_deck'](controller, logical)
                if not isinstance(selection, dict) or selection.get('status') != 'PASS' or selection.get('selected_title') not in titles or (not isinstance(selection.get('evidence'), str)):
                    raise MissingEvidence('current independently observed test selection required')
                evidence = controller.screenshot('deck%d-selected-track' % logical)
                controller.events.append(dict(action='selection_observed', deck=logical, evidence=evidence))
                # Touchscreen selection can outlast the receiver's idle lease.
                # Renew at this acknowledged neutral boundary before LOAD,
                # while no stateful frame has been attempted on the old lease.
                sessions[0].renew('load-deck%d' % logical, neutral=controller.panel_inputs_neutral())
                saved_timeout = controller.timeout
                try:
                    controller.timeout = 180
                    controller.load(logical)
                    snapshot = controller.wait(lambda s: s['decks'][deck]['loaded'], 'deck%d loaded' % logical)
                    if snapshot['decks'][deck].get('title') not in titles:
                        raise MissingEvidence('exact loaded test title unavailable')
                finally:
                    controller.timeout = saved_timeout
                controller.button(logical, 'play')
                wait_playback_motion(controller, logical, snapshot)
            prepared_setup=setup_deck if memory_phase else prepare_cue_decks_before_sdk(owned.get('decks',(0,)),setup_deck,lambda:finalize_cue_session(controller,sessions[0],health),lambda deck:fresh_cue_positions(lambda index:source_rows(controller.observe(),index,meta['pid'],binding,titles),deck))
            def reload_deck(deck):
                reload_selected_cue_track(controller,sessions[0],context['prepare_deck'],deck,titles)
            result = run_test_phase(owned.get('test_phase','full'),read_log,send_request,health,meta['pid'],setup_deck=prepared_setup,read_positions=lambda deck: source_rows(controller.observe(),deck,meta['pid'],binding,titles),decks=owned.get('decks',(0,)),reload_deck=reload_deck,selection=owned.get('selection'),sample_rate=binding['engine_rate'])
            if result.get('status') == 'PASS':
                try:
                    if memory_phase and callable(context.get('prepare_restart')):
                        context['prepare_restart'](controller)
                        result['restart_preparation']=require_empty_cue_restart(controller,CueClient(read_log,send_request,health,meta['pid']),owned.get('decks',(0,)))
                    result['finalization'] = finalize_cue_session(controller,sessions[0],health)
                except Exception as error:
                    result['status'] = 'FAIL'
                    result.setdefault('errors', []).append(str(error))
            if result.get('status') != 'PASS' and callable(context.get('on_failure')):
                context['on_failure'](controller, result, bounded_read)
            result.update(pid=meta['pid'], birth=meta['birth'], mapped_probe_sha256=pb['binary']['sha256'], events=list(controller.events))
            return result

        context = dict(owned, driver_path=DRIVER, driver_sha256=owned['driver_sha256'], position_binding=binding, stopped_identity=stopped, expected_titles=titles, panel_transport_factory=factory, verify_process=independent)
        result = run_observed_runtime(ssh, package, source_root, phase, firmware, remaining, command, context, execute_controls=action, validate_observed=validate_cue_observation)
        result.update(pid=meta.get('pid'), birth=meta.get('birth'), mapped_probe_sha256=pb['binary']['sha256'] if meta else None)
        return result
    config = dict(owned, driver_path=DRIVER, driver_sha256=owned['driver_sha256'], receiver_path=RECEIVER, receiver_sha256=owned['receiver_sha256'], position_binding=binding, probe_manifest=probe_manifest, probe_package=probe_package, probe_id=ID, prepare_allow=prepare_allow, run_settings=False, loaded_executor=loaded_executor)
    if cold is not None:
        output.mkdir()
        pinned=[None]
        def verify_cold(pid,birth):
            # The checked boot ID establishes a new process domain; numeric
            # PIDs and birth ticks can legitimately be reused after a reboot.
            if not independent(pid,birth):raise ValueError('cold native identity refused before controls')
            if pinned[0] is not None and pinned[0]!=(pid,birth):raise ValueError('cold native identity changed')
            pinned[0]=(pid,birth)
            return True
        loaded=loaded_executor(cold_stopped,seconds,verify_cold)
        if loaded.get('status')=='PASS' and pinned[0]!=(loaded.get('pid'),loaded.get('birth')):raise ValueError('cold callback did not pin its actual native process')
        return dict(status=loaded.get('status','INCOMPLETE'),scope='retained-disk cold boot and selected public SDK native commands',approval=cold['approval'],loaded=loaded,cold_boot=dict(previous=cold['previous_boot_id'],current=cold['boot_id']),errors=loaded.get('errors',[]),missing=loaded.get('missing',[]))
    report = run_public_runtime(ssh, package, source_root, output, firmware, seconds, command, config)
    return report


def run_cue_runtime(ssh,package,source_root,output,firmware,seconds,command,owned):
    """Run with disposable internal framework files; return only test results."""
    import tempfile
    parent=Path(output).parent
    with tempfile.TemporaryDirectory(prefix='sdk-cue-runtime-',dir=parent) as temporary:
        return _run_cue_runtime(ssh,package,source_root,Path(temporary)/'run',firmware,seconds,command,owned)
