# SPDX-License-Identifier: GPL-3.0-only
"""Actual public-SDK recurrence after terminal release and original deadline."""
from sdk_timestamps import compare_timestamps
import json,math,time
from sdk_emulator_loop import LoopClient
from sdk_emulator_controls import position_motion

def run_continuation(read_log,send_request,health,pid,read_guest,binding,*,clock=time.monotonic,sleep=time.sleep,expected_titles=("Bad Romance","Lady Gaga - Bad Romance","Lady Gaga - Bad Romance.m4a")):
 if type(pid) is not int or not 1<=pid<2**31:raise ValueError("positive native process ID required")
 client=LoopClient(read_log,send_request,health,pid,timeout=20,clock=clock,sleep=sleep,sample_rate=binding['engine_rate'])
 def operation(op,beats=0):
  client.submit(0,op,**({'beats':beats} if beats else {}))
  record=client.evidence[-1]
  observed=dict(record['observed'],terminal_receipt_ns=record['terminal']['receipt_ns'])
  return observed,record['before']['query_ns']+10_000_000_000
 def positions():
  raw=read_guest('cat /tmp/az-sdk-position.'+str(pid)+'.json; printf "\\n__UPTIME__\\n"; cat /proc/uptime')
  packet,uptime=raw.split('\n__UPTIME__\n');packet=json.loads(packet);now=int(float(uptime.split()[0])*1e9)
  proof=position_motion(packet,1,str(pid),binding,now_boottime_ns=now)
  if not proof.get('binding_verified') or not proof.get('fresh'):raise RuntimeError('continuation source clock/binding unavailable')
  rows=[]
  for sample in packet['samples']:
   deck=sample['decks'][0];value=deck.get('pos_s')
   if deck.get('read_rc')!=0 or deck.get('loaded')!=1 or deck.get('title') not in expected_titles or type(value) not in (float,int) or not math.isfinite(value):raise RuntimeError('continuation loaded source identity changed')
   rows.append((sample['native_monotonic_ns'],value))
  return rows,packet['native_monotonic_ns']
 operation(4);operation(1);sleep(1);operation(2);out,deadline=operation(9,beats=1)
 if out['engaged']!=1 or not out['bounds_valid']:raise RuntimeError('continuation native loop bounds unavailable')
 a,b=out['in'],out['out'];rate=binding['engine_rate'];seen=[];end=clock()+300;wraps=0
 while compare_timestamps(clock(), end, units_per_second=1) < 0:
  rows,now=positions()
  for stamp,value in rows:
   if seen and compare_timestamps(stamp,seen[-1][0])<=0:continue
   if seen:
    oldstamp,old=seen[-1]
    if compare_timestamps(oldstamp,deadline)>0 and compare_timestamps(stamp,deadline)>0 and compare_timestamps(old,value,units_per_second=1,sample_rate=rate)>0 and compare_timestamps(old,a,units_per_second=1,sample_rate=rate)>=0 and compare_timestamps(old,b,units_per_second=1,sample_rate=rate)<=0 and compare_timestamps(value,a,units_per_second=1,sample_rate=rate)>=0 and compare_timestamps(value,b,units_per_second=1,sample_rate=rate)<=0:wraps+=1
   seen.append((stamp,value))
  if compare_timestamps(now,deadline)>0 and wraps>=2:break
  sleep(.25)
 else:raise RuntimeError('SDK loop did not wrap twice after released request deadline')
 exited,_=operation(4);floor=exited['terminal_receipt_ns'];end=clock()+150;forward=[]
 while compare_timestamps(clock(), end, units_per_second=1) < 0:
  rows,now=positions()
  for stamp,value in rows:
   if compare_timestamps(stamp,floor)<=0 or forward and compare_timestamps(stamp,forward[-1][0])<=0:continue
   forward.append((stamp,value))
  if len(forward)>=2 and compare_timestamps(forward[-1][1],b,units_per_second=1,sample_rate=rate)>0 and compare_timestamps(forward[-1][1],forward[-2][1],units_per_second=1,sample_rate=rate)>0:break
  sleep(.25)
 else:raise RuntimeError('SDK EXIT did not resume source beyond loop OUT')
 cleared,_=operation(6)
 if (cleared['native_load'],cleared['load_unit'])!=(out['native_load'],out['load_unit']):raise RuntimeError('continuation native load changed')
 print('PASS public SDK loop continues after release/deadline, one-beat loop wraps twice after LENGTH deadline, EXIT advances, CLEAR applies',flush=True)
 return {'status':'PASS','post_deadline_wraps':wraps}
