#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Bounded read-only Linux scheduling captures for the pinned emulator process.

Cumulative scheduler runtime and runqueue delay distinguish CPU work from time
waiting to run. A sleeping thread's wchan remains only a point-in-time witness.
No native loaded/transport semantics or feature acceptance are inferred here.
"""
from sdk_timestamps import compare_timestamps
import math
import re

BYTE_CAP=524288
THREAD_CAP=512

def _identity(pid,birth):
    if (not isinstance(pid,str) or not re.fullmatch('[1-9][0-9]{0,9}',pid) or int(pid)>2147483647 or
        not isinstance(birth,str) or not re.fullmatch('[1-9][0-9]{0,19}',birth) or int(birth)>=2**64):
        raise ValueError('one pinned process and birth required')

def scheduler_snapshot_script(pid,birth):
    _identity(pid,birth)
    path='/proc/'+pid
    guard='test "$(awk \'{sub(/^.*\\) /, ""); if ($1 != "Z" && $1 != "X" && $1 != "x") print $20}\' '+path+'/stat 2>/dev/null)" = '+birth+' || exit 93\n'
    # One AWK process reads each enumerated thread without aborting the remainder
    # on a retired procfs path. A live unreadable path fails the whole capture.
    # There is no pipeline whose final status could hide an AWK read failure.
    program = r"""
function readone(file, value, result) {
    result=(getline value < file); close(file); read_status=result; return value
}
function emit(value) {
    bytes+=length(value)+1
    if (bytes > CAP) exit 95
    print value
}
BEGIN {
    for (i=1; i<ARGC; i++) {
        path=ARGV[i]; directory=path; sub(/\/stat$/, "", directory)
        tid=directory; sub(/^.*\//, "", tid)
        if (tid !~ /^[1-9][0-9]*$/) exit 95
        emit("enumerated " tid)
        first=readone(path); first_status=read_status
        counters=readone(directory "/schedstat"); counters_status=read_status
        channel=readone(directory "/wchan"); channel_status=read_status
        last=readone(path); last_status=read_status
        if (first_status != 1 || counters_status != 1 || channel_status != 1 || last_status != 1) {
            if (tid == PID || system("test ! -d " directory) != 0) exit 95
            emit("retired " tid)
            continue
        }
        emit(path " " first)
        emit(directory "/schedstat " counters)
        emit(directory "/wchan " channel)
        emit(directory "/stat_after " last)
    }
    exit
}
"""
    # Reserve room for the fixed header and bounded final census as well.
    capture_cap=BYTE_CAP-(THREAD_CAP*32+1024)
    return ('set -e\n'+guard+'set -- '+path+'/task/[0-9]*/stat\ntest "$#" -le '+str(THREAD_CAP)+' || exit 94\n'+
            'printf "AZSDK_SCHED 2\\npid '+pid+'\\nbirth '+birth+'\\nuptime "\ncut -d" " -f1 /proc/uptime\n'+
            "capture=$(LC_ALL=C awk -v PID="+pid+" -v CAP="+str(capture_cap)+" '"+program+"' \"$@\") || exit 95\n"+
            'printf "%s\\n" "$capture"\nset -- '+path+'/task/[0-9]*/stat\ntest "$#" -le '+str(THREAD_CAP)+' || exit 94\n'+
            'for file in "$@"; do tid=${file%/stat}; tid=${tid##*/}; printf "census %s\\n" "$tid"; done\n'+
            guard+'printf "END '+pid+' '+birth+'\\n"\n')

def _uint(value):
    if not re.fullmatch('[0-9]+',value) or int(value)>=2**64:raise ValueError('invalid scheduler integer')
    return int(value)

def parse_scheduler_snapshot(text,pid,birth):
    _identity(pid,birth)
    if not isinstance(text,str) or len(text.encode())>BYTE_CAP:raise ValueError('scheduler capture byte cap')
    rows=text.splitlines()
    schema=2 if rows and rows[0]=='AZSDK_SCHED 2' else 1
    if len(rows)<8 or rows[:3]!=['AZSDK_SCHED '+str(schema),'pid '+pid,'birth '+birth] or rows[-1]!='END '+pid+' '+birth:
        raise ValueError('incomplete scheduler capture or identity mismatch')
    if not rows[3].startswith('uptime '):raise ValueError('scheduler clock missing')
    try:uptime=float(rows[3][7:])
    except ValueError:raise ValueError('invalid scheduler clock')
    if not math.isfinite(uptime) or compare_timestamps(uptime,0,units_per_second=1,sample_rate=100)<=0:raise ValueError('invalid scheduler clock')
    threads={};after_threads={};seen=set();enumerated=set();retired=set();census=set()
    for row in rows[4:-1]:
        marker=re.fullmatch('(enumerated|retired|census) ([1-9][0-9]*)',row)
        if marker:
            if schema!=2:raise ValueError('unexpected scheduler lifecycle record')
            kind,tid=marker.groups();group={'enumerated':enumerated,'retired':retired,'census':census}[kind]
            if tid in group:raise ValueError('duplicate scheduler lifecycle record')
            group.add(tid)
            if len(group)>THREAD_CAP:raise ValueError('scheduler thread count cap')
            continue
        match=re.fullmatch('/proc/'+re.escape(pid)+'/task/([1-9][0-9]*)/(stat|schedstat|wchan|stat_after) (.*)',row)
        if not match:raise ValueError('unexpected scheduler record path')
        tid,kind,value=match.groups()
        if (tid,kind) in seen:raise ValueError('duplicate scheduler record')
        seen.add((tid,kind));thread=(after_threads if kind=='stat_after' else threads).setdefault(tid,{})
        if kind=='stat_after' and schema!=2:raise ValueError('unexpected scheduler after stat')
        if len(threads)>THREAD_CAP:raise ValueError('scheduler thread count cap')
        if kind in ('stat','stat_after'):
            record=re.fullmatch(re.escape(tid)+r' \((.*)\) (\S+) (.*)',value)
            if not record:raise ValueError('invalid scheduler stat')
            name,state,tail=record.groups();fields=[state]+tail.split()
            if len(fields)<39:raise ValueError('short scheduler stat')
            thread.update(name=name,state=state,user_ticks=_uint(fields[11]),system_ticks=_uint(fields[12]),
                          birth=str(_uint(fields[19])),processor=_uint(fields[36]),
                          realtime_priority=_uint(fields[37]),policy=_uint(fields[38]))
        elif kind=='schedstat':
            fields=value.split()
            if len(fields)!=3:raise ValueError('invalid scheduler counters')
            thread.update(zip(('runtime_ns','runqueue_ns','timeslices'),map(_uint,fields)))
        else:
            if not re.fullmatch('[a-zA-Z0-9_.-]{1,128}',value):raise ValueError('invalid scheduler wait channel')
            thread['wchan']=value
    if pid not in threads or threads[pid].get('birth')!=birth:raise ValueError('scheduler principal birth mismatch')
    if any(not {'birth','runtime_ns','runqueue_ns','wchan'}<=set(thread) for thread in threads.values()):
        raise ValueError('partial scheduler thread capture')
    if schema==2:
        if (enumerated!=set(threads)|retired or set(threads)&retired or
            not census or not census<=set(threads) or pid not in census or
            set(after_threads)!=set(threads)):
            raise ValueError('incomplete scheduler lifecycle census')
        for tid,thread in threads.items():
            after=after_threads[tid]
            if any(thread[k]!=after[k] for k in ('birth','policy','realtime_priority')):
                raise ValueError('scheduler thread identity or policy changed during capture')
    return dict(pid=pid,birth=birth,uptime=uptime,threads=threads,schema=schema,retired=sorted(retired))

def scheduler_delta(before,after):
    if (before['pid'],before['birth'])!=(after['pid'],after['birth']):raise ValueError('scheduler process identity changed')
    elapsed=after['uptime']-before['uptime']
    if not math.isfinite(elapsed) or compare_timestamps(elapsed,0,units_per_second=1,sample_rate=100)<=0:raise ValueError('scheduler clock did not advance')
    threads={}
    for tid in before['threads'].keys() & after['threads'].keys():
        a,b=before['threads'][tid],after['threads'][tid]
        if a['birth']!=b['birth']:raise ValueError('scheduler thread identity changed')
        values={name:b[name]-a[name] for name in ('runtime_ns','runqueue_ns','timeslices')}
        if any(value<0 for value in values.values()):raise ValueError('scheduler counters decreased')
        values.update(name=b['name'],state=b['state'],wchan=b['wchan'],
                      runtime_fraction=values['runtime_ns']/(elapsed*1e9),
                      runqueue_fraction=values['runqueue_ns']/(elapsed*1e9))
        threads[tid]=values
    return dict(elapsed_s=elapsed,threads=threads,started=sorted(after['threads'].keys()-before['threads'].keys()),
                ended=sorted(before['threads'].keys()-after['threads'].keys()))

def require_fair_scheduler(snapshot):
    """Qualify the experimental service policy from actual kernel records.

    Call only with a parsed birth/nonce-owned capture, and repeat after native
    thread creation or controls. This says nothing about audio or load success.
    """
    _identity(snapshot.get('pid'),snapshot.get('birth'))
    threads=snapshot.get('threads')
    if (not isinstance(threads,dict) or not 1<=len(threads)<=THREAD_CAP or
        snapshot['pid'] not in threads or threads[snapshot['pid']].get('birth')!=snapshot['birth']):
        raise ValueError('complete parsed scheduler capture required')
    policies={}
    for tid,thread in threads.items():
        policy=thread.get('policy')
        if type(policy) is not int or policy not in (0,3,5):
            raise ValueError('fair scheduling profile not observed on native thread '+str(tid))
        key=str(policy);policies[key]=policies.get(key,0)+1
    return dict(threads=len(threads),policies=policies)
