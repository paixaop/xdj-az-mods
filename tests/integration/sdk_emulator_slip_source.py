#!/usr/bin/env python3
"""Read-only source-owned Slip diagnostics, with no enabled-state projection."""
from sdk_timestamps import compare_timestamps
import hashlib
import math
import re
import struct
from pathlib import Path
from sdk_emulator_controls import MissingEvidence
from sdk_probe_binding import Elf,_artifact,_string

FIELDS=('sequence','generation','revision','observed_ns','callbacks','registered','valid','raw','reserved')
GETTER_FIELDS=('sequence','generation','observed_ns','target_slot4','target_slot13','target_slot14','seen_mask','conflict_mask')
SCOPE='source-owned raw Slip observations; no enabled-state semantics'


def _object(full,small,name,expected_size):
    found=[]
    for section in full.sections:
        if section[1]!=2:continue
        if section[9]!=24 or section[5]%24 or not 0<section[6]<len(full.sections):
            raise ValueError('invalid Slip symbol table')
        symbols=full.section_data(section,2,24*65536)
        strings=full.section_data(full.sections[section[6]],3,8*1024*1024)
        for offset in range(0,len(symbols),24):
            key,info,other,index,rva,size=struct.unpack_from('<IBBHQQ',symbols,offset)
            if _string(strings,key)==name:
                if info&15!=1 or not 0<index<len(full.sections) or size!=expected_size or rva%min(expected_size,8) or \
                   full.sections[index][2]&3!=3 or full.sections[index][1] not in (1,8) or \
                   not all(any(p[0]==1 and p[1]&7==6 and p[3]<=rva and rva+size<=p[3]+p[6] for p in elf.programs) for elf in (full,small)):
                    raise ValueError('Slip observation object layout mismatch')
                found.append(dict(name=name,rva=rva,size=size))
    if len(found)!=1:raise ValueError('one Slip observation object required')
    return found[0]


def binding(core,getters=False):
    full=Elf(_artifact(Path(core)/'azmod.so.full'));small=Elf(_artifact(Path(core)/'azmod.so'))
    if full.identity!=small.identity:raise ValueError('Slip full/stripped build mismatch')
    symbol=_object(full,small,'azsdk_slip_observation_v1',224)
    collector=full.function('azsdk_slip_collect_v1',2)
    if full.code(collector['rva'],collector['size'])!=small.code(collector['rva'],collector['size']):
        raise ValueError('Slip collector code differs in stripped artifact')
    result=dict(build_id=small.identity,sha256=hashlib.sha256(small.data).hexdigest(),symbol=symbol,collector=collector)
    if getters:
        result.update(getter_symbol=_object(full,small,'azsdk_slip_getter_observation_v1',224),
                      getter_hooks=_object(full,small,'azsdk_slip_getter_hooks_v1',4))
    return result


def decode(first,last,now_ns):
    if type(now_ns) is not int or not 0<now_ns<2**64:raise ValueError('bound native monotonic clock required')
    if not isinstance(first,bytes) or not isinstance(last,bytes) or len(first)!=224 or len(last)!=224:
        raise MissingEvidence('complete four-row Slip array unavailable')
    if first!=last:raise MissingEvidence('Slip observation changed during capture')
    rows=[dict(zip(FIELDS,struct.unpack_from('<5Q4I',first,d*56))) for d in range(4)]
    generations=set()
    for row in rows:
        if not row['sequence'] or row['sequence']&1:raise MissingEvidence('stable collected Slip row unavailable')
        if row['reserved'] or row['registered'] not in (0,1) or row['valid'] not in (0,1):
            raise ValueError('Slip observation flags/reserved mismatch')
        if row['registered']:
            if not row['generation'] or row['generation'] in generations:raise ValueError('Slip source generation missing or aliased')
            generations.add(row['generation'])
        elif row['generation'] or row['callbacks'] or row['valid']:
            raise ValueError('retired Slip source retained observation')
        if row['valid']:
            if not row['registered'] or not row['revision'] or not row['observed_ns'] or \
               row['callbacks']<row['revision'] or compare_timestamps(row['observed_ns'], now_ns) > 0:
                raise ValueError('Slip callback generation/clock/count mismatch')
        elif row['revision'] or row['observed_ns'] or row['raw']:
            raise ValueError('invalid Slip receipt retained raw value')
    return rows


def decode_getters(first,last,sources,now_ns):
    """Code targets observed before genuine BLR; one time dates the latest site."""
    if type(now_ns) is not int or not 0<now_ns<2**64 or len(sources)!=4:
        raise ValueError('four bound Slip sources and native clock required')
    if not isinstance(first,bytes) or not isinstance(last,bytes) or len(first)!=224 or len(last)!=224 or first!=last:
        raise MissingEvidence('stable four-row Slip getter observations unavailable')
    rows=[dict(zip(GETTER_FIELDS,struct.unpack_from('<6Q2I',first,d*56))) for d in range(4)]
    for row,raw in zip(rows,sources):
        if not row['sequence'] or row['sequence']&1:raise MissingEvidence('stable Slip getter collection unavailable')
        seen,conflict=row['seen_mask'],row['conflict_mask']
        if seen&~7 or conflict&~seen:raise ValueError('Slip getter site masks mismatch')
        if row['generation']!=raw['generation'] or (seen and not raw['registered']):
            raise MissingEvidence('Slip getter observation is not from current collected source')
        if (seen and not (compare_timestamps(0, row['observed_ns']) < 0 and compare_timestamps(row['observed_ns'], now_ns) <= 0)) or (not seen and row['observed_ns']):
            raise ValueError('Slip getter invocation clock mismatch')
        for bit,slot in enumerate((4,13,14)):
            target=row['target_slot'+str(slot)]
            if ((seen>>bit)&1 and (not 0<target<2**48 or target&3)) or (not (seen>>bit)&1 and target):
                raise ValueError('Slip getter seen/target mismatch')
    return rows


def getter_sites(observation,deck):
    if type(deck) is not int or deck not in range(1,5):raise ValueError('one logical Slip deck required')
    row=observation['getters'][deck-1]
    return {slot:row['target_slot'+str(slot)] for bit,slot in enumerate((4,13,14))
            if row['seen_mask']&(1<<bit) and not row['conflict_mask']&(1<<bit)}


def _maps(text):
    values=[]
    for line in text.splitlines():
        # procfs pathnames may contain spaces, including the standard deleted
        # suffix on SysV mappings; the first five columns remain fixed.
        fields=line.split(maxsplit=5)
        if len(fields) not in (5,6) or not re.fullmatch('[0-9a-f]+-[0-9a-f]+',fields[0]) or \
           not re.fullmatch('[r-][w-][x-][ps]',fields[1]) or not re.fullmatch('[0-9a-f]+',fields[2]) or \
           not re.fullmatch('[0-9a-f]+:[0-9a-f]+',fields[3]) or not re.fullmatch('[0-9]+',fields[4]):
            raise ValueError('invalid Slip process mapping')
        lo,hi=(int(v,16) for v in fields[0].split('-'))
        if not 0<lo<hi<2**64:raise ValueError('invalid Slip map bounds')
        values.append((lo,hi,fields[1],int(fields[2],16),fields[3],fields[4],fields[5] if len(fields)==6 else ''))
    if not values or len(values)>8192:raise ValueError('bounded Slip process maps required')
    return values


def _double_snapshot(read,pid,objects):
    commands=[]
    for i in range(2):
        for name,(address,size) in objects.items():
            commands.append("printf 'AZSLIP_"+str(i)+'_'+name+"\\n'")
            commands.append('dd if=/proc/'+pid+'/mem bs=1 skip='+str(address)+' count='+str(size)+' 2>/dev/null | od -v -x | cut -c9-')
    raw=read('\n'.join(commands),16384);result=[{},{}]
    for i in range(2):
        for name,(_,size) in objects.items():
            marker='AZSLIP_'+str(i)+'_'+name+'\n'
            if raw.count(marker)!=1:raise MissingEvidence('complete Slip memory samples unavailable')
            words=raw.split(marker,1)[1].split('AZSLIP_',1)[0].split()
            if any(not re.fullmatch('[0-9a-f]{4}',word) for word in words):raise ValueError('invalid Slip memory words')
            value=b''.join(struct.pack('<H',int(word,16)) for word in words)
            if len(value)!=size:raise MissingEvidence('short Slip memory sample')
            result[i][name]=value
    return result


def _reader(controller,read_guest,pid,birth,verify_process,deadline):
    if any(not isinstance(v,str) or not re.fullmatch('[1-9][0-9]{0,19}',v) for v in (pid,birth)) or int(pid)>2147483647 or int(birth)>=2**64:
        raise ValueError('pinned Slip PID and birth required')
    if type(deadline) not in (int,float) or not math.isfinite(deadline):raise ValueError('finite existing Slip deadline required')
    def guard():
        controller._owner()
        now=controller._now()
        if type(now) not in (int,float) or not math.isfinite(now) or compare_timestamps(now, deadline, units_per_second=1) >= 0:
            raise MissingEvidence('Slip read overall budget expired')
        if verify_process(pid,birth) is not True:raise ValueError('pinned Slip process changed')
    def read(body,cap):
        guard();text=read_guest(body,cap);guard()
        if not isinstance(text,str) or len(text.encode())>cap:raise ValueError('Slip read byte cap exceeded')
        return text
    return guard,read


def capture(controller,read_guest,pid,birth,proof,read_clock,verify_process,deadline,previous=None):
    """Use the existing owned normal-SDK process; no GUI-tap mod or native call.

    `read_guest` is the normal bounded owner/PID/birth-guarded read adapter.
    `read_clock` must supply its fresh, bound native monotonic clock. Registered
    means current at the producer's GUI collection, not an external owner lease.
    """
    guard,read=_reader(controller,read_guest,pid,birth,verify_process,deadline)
    before=_maps(read('cat /proc/'+pid+'/maps',524288))
    module=[r for r in before if r[-1].endswith('/azmod.so')]
    paths={r[-1] for r in module};bases={r[0]-r[3] for r in module if r[2]=='r-xp'}
    if len(paths)!=1 or len(bases)!=1:raise ValueError('one pinned Slip azmod module required')
    path=paths.pop();base=bases.pop()
    if not re.fullmatch('/[A-Za-z0-9_./-]+',path) or not re.fullmatch('[0-9a-f]{64}',proof['sha256']) or \
       not re.fullmatch('[0-9a-f]{8,128}',proof['build_id']) or proof['symbol']['name']!='azsdk_slip_observation_v1' or proof['symbol']['size']!=224:
        raise ValueError('exact bound Slip module/object required')
    if read('sha256sum '+path,4096).split()!=[proof['sha256'],path]:raise ValueError('mapped Slip core hash mismatch')
    objects={'raw':(base+proof['symbol']['rva'],224)}
    if 'getter_symbol' in proof:
        for key,size in (('getter_symbol',224),('getter_hooks',4)):
            if proof[key]['size']!=size:raise ValueError('Slip getter object size mismatch')
            objects[key]=(base+proof[key]['rva'],size)
    object_maps=[]
    for address,size in objects.values():
        matches=[r for r in before if r[0]<=address and address+size<=r[1] and r[2]=='rw-p']
        if len(matches)!=1:raise ValueError('Slip object outside bound writable mapping')
        object_maps+=matches
    first,last=_double_snapshot(read,pid,objects);now=read_clock();guard()
    after=_maps(read('cat /proc/'+pid+'/maps',524288))
    if module!=[r for r in after if r[-1]==path] or any(r not in after for r in object_maps):
        raise ValueError('Slip module/object mapping changed during read')
    result=dict(pid=pid,birth=birth,build_id=proof['build_id'],sha256=proof['sha256'],
                rows=decode(first['raw'],last['raw'],now),native_now_ns=now,scope=SCOPE,module_path=path,module_maps=module)
    if 'getter_symbol' in objects:
        if first['getter_hooks']!=last['getter_hooks'] or first['getter_hooks']!=struct.pack('<I',7):
            raise MissingEvidence('all three checked Slip getter sites are not installed')
        result['getters']=decode_getters(first['getter_symbol'],last['getter_symbol'],result['rows'],now)
    if previous is not None:
        _same_process(previous,result)
        if compare_timestamps(now, previous['native_now_ns']) < 0:raise ValueError('Slip native clock regressed')
        for old,new in zip(previous['rows'],result['rows']):
            if new['sequence']<=old['sequence']:raise MissingEvidence('new Slip GUI collection unavailable')
            if new['generation']==old['generation'] and (new['callbacks']<old['callbacks'] or
               (new['valid'] and any(new[k]<old[k] for k in ('revision','observed_ns')))):
                raise ValueError('Slip source counters regressed')
    return result


def _same_process(before,after):
    if any(before.get(k)!=after.get(k) for k in ('pid','birth','build_id','sha256','module_path','module_maps')):
        raise ValueError('Slip callback process/build identity changed')


def registered_sources(before,after):
    """Diagnostic readiness from successive GUI collections, never Slip state."""
    _same_process(before,after)
    if len(before['rows'])!=4 or len(after['rows'])!=4 or compare_timestamps(after['native_now_ns'], before['native_now_ns']) < 0:
        raise ValueError('four forward-clock Slip rows required')
    for old,new in zip(before['rows'],after['rows']):
        if not old['registered'] or not new['registered'] or not old['generation'] or \
           old['generation']!=new['generation'] or new['sequence']<=old['sequence']:
            raise MissingEvidence('same registered Slip source not observed in two collections')
    return (1,2,3,4)


class GetterRead:
    """One bounded diagnostic transaction on the existing guarded SSH adapter.

    Admission maps/hash are shared by its scalar reads and confirmed freshly
    before acceptance. Every uncached read still uses read_guest's actual
    owner/PID/birth checks before and after the remote operation. No provisional
    snapshot/identity may qualify a result until finish() succeeds.
    """
    def __init__(self,controller,read_guest,pid,birth,proof,read_clock,verify_process,overall_deadline):
        _reader(controller,read_guest,pid,birth,verify_process,overall_deadline)
        self.controller,self.read_guest,self.pid,self.birth=controller,read_guest,pid,birth
        self.proof,self.read_clock,self.verify_process=proof,read_clock,verify_process
        self.deadline=overall_deadline;self.started=False;self.closed=False;self.cached={};self.target_maps=[];self.last_memory_read=None
        self._guard()
        if verify_process(pid,birth) is not True:raise ValueError('getter admission process identity refused')
        text=self._uncached('cat /proc/'+pid+'/maps',524288);self.maps=_maps(text)
        modules=[r for r in self.maps if r[-1].endswith('/azmod.so')];paths={r[-1] for r in modules}
        if len(paths)!=1:raise ValueError('one getter admission core required')
        self.path=paths.pop();bases={r[0]-r[3] for r in modules if r[2]=='r-xp'}
        if len(bases)!=1 or not re.fullmatch('/[A-Za-z0-9_./-]+',self.path):raise ValueError('getter admission mapping refused')
        base=bases.pop();self.module_maps=modules;self.object_maps=[]
        for key in ('symbol','getter_symbol','getter_hooks'):
            obj=proof[key];matches=[r for r in self.maps if r[0]<=base+obj['rva'] and base+obj['rva']+obj['size']<=r[1] and r[2]=='rw-p']
            if len(matches)!=1:raise ValueError('getter admission object mapping refused')
            self.object_maps+=matches
        hash_body='sha256sum '+self.path;hashed=self._uncached(hash_body,4096)
        if hashed.split()!=[proof['sha256'],self.path]:raise ValueError('getter admission core hash mismatch')
        self.cached={'cat /proc/'+pid+'/maps':text,hash_body:hashed}

    def _guard(self):
        if self.closed:raise ValueError('getter transaction is closed')
        self.controller._owner();now=self.controller._now()
        if type(now) not in (int,float) or not math.isfinite(now) or compare_timestamps(now, 0, units_per_second=1) < 0 or compare_timestamps(now, self.deadline, units_per_second=1) >= 0:
            raise MissingEvidence('getter transaction budget expired')

    def _uncached(self,body,cap):
        self._guard();text=self.read_guest(body,cap)
        if not isinstance(text,str) or len(text.encode())>cap:raise ValueError('getter transaction read byte cap exceeded')
        if 'AZSLIP_' in body:self.last_memory_read=text
        self._guard()
        return text

    def read(self,body,cap):
        self._guard()
        if body not in self.cached:return self._uncached(body,cap)
        text=self.cached[body]
        if len(text.encode())>cap:raise ValueError('getter cached metadata byte cap exceeded')
        return text

    def verify(self,pid,birth):
        self._guard()
        if (pid,birth)!=(self.pid,self.birth):raise ValueError('getter transaction identity mismatch')
        return True

    def begin(self,deadline):
        self._guard()
        if self.started or type(deadline) not in (int,float) or not math.isfinite(deadline) or compare_timestamps(deadline, self.deadline, units_per_second=1) > 0 or compare_timestamps(deadline, self.controller._now(), units_per_second=1) <= 0:
            raise ValueError('one nonextended getter observation deadline required')
        self.deadline=deadline;self.started=True

    def snapshot(self):
        if not self.started:raise ValueError('getter observation has not begun')
        self._guard()
        return capture(self.controller,self.read,self.pid,self.birth,self.proof,self.read_clock,self.verify,self.deadline)

    def identify(self,observation,images,decks=(1,)):
        text=self._uncached('cat /proc/'+self.pid+'/maps',524288);fresh=_maps(text)
        if self.module_maps!=[r for r in fresh if r[-1]==self.path] or any(r not in fresh for r in self.object_maps):
            raise ValueError('getter admission mappings changed')
        self.cached['cat /proc/'+self.pid+'/maps']=text
        result=identify_targets(self.controller,self.read,observation,images,self.verify,self.deadline,decks)
        self.target_maps=[tuple(row['mapping']) for row in result['targets']]
        return result

    def finish(self):
        try:
            fresh=_maps(self._uncached('cat /proc/'+self.pid+'/maps',524288))
            if self.module_maps!=[r for r in fresh if r[-1]==self.path] or any(r not in fresh for r in self.object_maps+self.target_maps):
                raise ValueError('getter final module/object/target mapping changed')
            if self._uncached('sha256sum '+self.path,4096).split()!=[self.proof['sha256'],self.path]:raise ValueError('getter final core hash mismatch')
            if self.verify_process(self.pid,self.birth) is not True:raise ValueError('getter final process identity refused')
            self._guard()
        finally:self.closed=True


def identify_targets(controller,read_guest,observation,images,verify_process,deadline,decks=(1,)):
    """Identify captured code without calling it. Unknown images stay unresolved.

    `images` maps exact guest paths to already hash-validated Image instances
    from the existing ep147-addrgen reader. A 16-byte match identifies code at
    the observed entry only; it establishes no return domain or Slip semantics.
    """
    if not isinstance(decks,tuple) or not decks or len(set(decks))!=len(decks) or any(type(d) is not int or d not in range(1,5) for d in decks):
        raise ValueError('explicit unique getter diagnostic decks required')
    guard,read=_reader(controller,read_guest,observation['pid'],observation['birth'],verify_process,deadline)
    maps=_maps(read('cat /proc/'+observation['pid']+'/maps',524288))
    core_path=observation['module_path'];core_maps=[r for r in maps if r[-1]==core_path]
    if not re.fullmatch('/[A-Za-z0-9_./-]+',core_path) or core_maps!=observation['module_maps'] or read('sha256sum '+core_path,4096).split()!=[observation['sha256'],core_path]:
        raise ValueError('getter source core identity changed')
    targets={}
    for deck in decks:
        for slot,address in getter_sites(observation,deck).items():
            matched=[r for r in maps if r[0]<=address and address+16<=r[1] and r[2]=='r-xp']
            if len(matched)!=1:raise ValueError('captured getter target is not in one executable mapping')
            targets.setdefault(address,dict(mapping=matched[0],sites=[]))['sites'].append((deck,slot))
    if not targets:raise MissingEvidence('no unconflicted genuine getter invocation observed')
    objects={'code'+str(i):(address,16) for i,address in enumerate(targets)}
    first,last=_double_snapshot(read,observation['pid'],objects)
    results=[];hashes={}
    for i,(address,entry) in enumerate(targets.items()):
        key='code'+str(i);code=first[key]
        if code!=last[key]:raise MissingEvidence('getter target code changed during read')
        mapping=entry['mapping'];path=mapping[-1]
        result=dict(target=address,mapping=mapping,sites=entry['sites'],words=code.hex(),status='UNRESOLVED',reason='exact mapped image not supplied')
        if path.startswith('/') and re.fullmatch('/[A-Za-z0-9_./-]+',path):
            if path not in hashes:
                fields=read('sha256sum '+path,4096).split()
                if len(fields)!=2 or fields[1]!=path or not re.fullmatch('[0-9a-f]{64}',fields[0]):raise ValueError('getter mapped file hash unavailable')
                hashes[path]=fields[0]
            result['mapped_sha256']=hashes[path]
        if path in images:
            image=images[path]
            if hashes.get(path)!=hashlib.sha256(image.data).hexdigest():raise ValueError('getter mapped image differs from exact supplied file')
            biases={mapping[0]-(base+mapping[3]-offset) for base,offset,filesz,_,flags in image.loads
                    if flags&1 and offset<=mapping[3]<offset+filesz}
            if len(biases)!=1:raise ValueError('getter image load bias is ambiguous')
            bias=biases.pop();native=address-bias
            if image.code(native,native+16)!=code:raise ValueError('getter target code differs from exact original')
            result.update(status='BOUND',reason='exact image and entry words match',image_address=native,load_bias=bias)
        results.append(result)
    after=_maps(read('cat /proc/'+observation['pid']+'/maps',524288));guard()
    if core_maps!=[r for r in after if r[-1]==core_path] or any(entry['mapping'] not in after for entry in targets.values()):
        raise ValueError('getter executable/source mappings changed during capture')
    return dict(scope='observed getter code identities; no invocation or enabled-domain claim',targets=results)


def fresh_callback(before,after,deck,fence_ns):
    """Qualify a new raw callback only; never translate it to enabled state."""
    _same_process(before,after)
    if type(deck) is not int or deck not in range(1,5) or type(fence_ns) is not int or not (compare_timestamps(before['native_now_ns'], fence_ns) <= 0 and compare_timestamps(fence_ns, after['native_now_ns']) <= 0):
        raise ValueError('Slip stimulus deck/native clock boundary mismatch')
    old,new=before['rows'][deck-1],after['rows'][deck-1]
    if not old['registered'] or not new['registered'] or not new['valid'] or not old['generation'] or new['generation']!=old['generation'] or \
       new['sequence']<=old['sequence'] or new['revision']<=old['revision'] or new['callbacks']<=old['callbacks'] or \
       not (compare_timestamps(max(fence_ns, old['observed_ns']), new['observed_ns']) < 0 and compare_timestamps(new['observed_ns'], after['native_now_ns']) <= 0):
        raise MissingEvidence('new same-source post-stimulus Slip callback unavailable')
    return dict(new)
