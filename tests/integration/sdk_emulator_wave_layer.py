#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Host-only admission checks for the sealed normal-loader wave probe.

No target I/O is performed. Caller identity observations must come from the
owned normal Allow/apply/new-process path, never from the untrusted mod log.
"""
from sdk_timestamps import compare_timestamps
import re

def validate_context(context, expected):
    if not isinstance(context,dict) or not isinstance(expected,dict):raise ValueError('owned context required')
    for key,value in expected.items():
        if context.get(key)!=value:raise ValueError('process/build/source context mismatch: '+key)
    for key in ('new_process','pin_before','pin_after','old_process'):
        row=context.get(key,{})
        if any(not re.fullmatch('[1-9][0-9]{0,19}',str(row.get(k,''))) for k in ('pid','birth')):raise ValueError('pinned process identity required')
    if context['pin_before']!=context['new_process'] or context['pin_after']!=context['new_process']:raise ValueError('process identity changed')
    if any(context['old_process'][k]==context['new_process'][k] for k in ('pid','birth')):raise ValueError('new PID and birth required')
    if context.get('old_absent') is not True or context.get('approval_origin')!='actual_ui' or (type(context.get('apply_count')) is not int or context.get('apply_count')!=1) or context.get('loader')!='normal_sdk_entry_library_service':raise ValueError('actual normal-loader lifecycle required')
    if context.get('approved_probe_sha256')!=expected['mapped_probe_sha256']:raise ValueError('actual approval hash mismatch')

# Every field below is emitted by the sealed v1 probe, not an invented receipt.
COMMON={'v','seq','port','event'}
REF={'source','generation','library','track'}
ANALYSIS_RATE=150
DETAIL={
 'start':{'window','max_polls','max_calls','deadline_ns'},
 'resolve':REF,
 'query':REF|{'query_ns','ticket','encoding','first','requested','rc'},
 'view':REF|{'ticket','encoding','first','count','total','stride','bytes','availability','provenance','revision','observed_ns','start_s','step_s'},
 'payload':{'first','count','raw_ticket','layered_ticket','raw_fnv64','layered_fnv64','raw_hex','layered_hex'},
 'release':{'ticket','rc'},
 'final':{'status','reason','calls','accepted','released','outstanding'},
}
class MissingEvidence(ValueError):pass

def _uint(value,bits=64,positive=False):
    if type(value) not in (str,int) or not re.fullmatch('0|[1-9][0-9]*',str(value)):raise ValueError('canonical unsigned integer required')
    number=int(value)
    if number>=1<<bits or (positive and number==0):raise ValueError('integer range exceeded')
    return number

def _records(text):
    if not isinstance(text,str) or len(text.encode())>48*768:raise ValueError('bounded exact wave transcript required')
    lines=text.splitlines()
    if len(lines)>48:raise ValueError('wave line count cap exceeded')
    result=[]
    for index,line in enumerate(lines,1):
        if len(line.encode())>=768 or not line.startswith('SDK_PUBLIC_WAVE '):raise ValueError('exact bare wave record required')
        parts=[p.split('=',1) for p in line.split()[1:]]
        if any(len(p)!=2 for p in parts) or len({p[0] for p in parts})!=len(parts):raise ValueError('duplicate/malformed wave fields')
        row=dict(parts);event=row.get('event')
        if event not in DETAIL or set(row)!=COMMON|DETAIL[event]:raise ValueError('exact event fields required')
        if row['v']!='1' or row['port']!='0' or _uint(row['seq'])!=index:raise ValueError('wave version/sequence/source port mismatch')
        for key,value in list(row.items()):
            if key in COMMON or key in ('raw_fnv64','layered_fnv64','raw_hex','layered_hex','status','reason','start_s','step_s'):continue
            if key=='rc':
                if not re.fullmatch('0|-[1-9][0-9]*|[1-9][0-9]*',value) or not -(1<<31)<=int(value)<1<<31:raise ValueError('invalid API status')
                row[key]=int(value)
            else:row[key]=_uint(value,64 if key in ('ticket','raw_ticket','layered_ticket','query_ns','source','generation','library','revision','observed_ns','deadline_ns') else 32)
        if event=='final' and (row['status'] not in ('PASS','INCOMPLETE','FAIL') or not re.fullmatch('[a-z0-9_]{1,80}',row['reason'])):raise ValueError('invalid final status/reason')
        result.append(row)
    if not result or result[-1]['event']!='final':raise MissingEvidence('bounded final wave record unavailable')
    if any(r['event']=='final' for r in result[:-1]):raise ValueError('record after final or duplicate final')
    return result

def _ref(row):
    return tuple(_uint(row[k],32 if k=='track' else 64,True) for k in ('source','generation','library','track'))

def _analysis_axis(row):
    # start_s is a timestamp in the ANLZ frame domain. step_s encodes the
    # fixed frame-grid metadata in the sealed protocol's canonical format.
    return (re.fullmatch(r'(0|[1-9][0-9]*)\.[0-9]{9}',row['start_s']) is not None and
            compare_timestamps(float(row['start_s']),row['first']/ANALYSIS_RATE,
                               units_per_second=1,sample_rate=ANALYSIS_RATE)==0 and
            row['step_s']=='%.12f'%(1.0/ANALYSIS_RATE))

def _fnv(data):
    value=14695981039346656037
    for byte in data:value=((value^byte)*1099511628211)&((1<<64)-1)
    return '%016x'%value

def _inverse(raw,layer):
    if len(raw)!=3 or len(layer)!=6 or layer[5]:raise ValueError('invalid layer stride/reserved byte')
    header=int.from_bytes(layer[:2],'little');count=header>>12
    if not 1<=count<=3:raise ValueError('invalid symbolic layer count')
    if not any(raw):
        if layer!=bytes((8,16,0,0,0,0)):raise ValueError('noncanonical all-zero record')
        return
    masks={1:4,2:2,3:1,4:6,5:5,6:3,8:7};prior_mask=0;prior_level=256;recovered=[0,0,0]
    for index in range(3):
        symbol=(header>>(index*4))&15;level=layer[2+index]
        if index>=count:
            if symbol or level:raise ValueError('nonzero unused layer field')
            continue
        mask=masks.get(symbol)
        if mask is None or not level or level>=prior_level or mask==prior_mask or mask&prior_mask!=prior_mask:raise ValueError('invalid symbolic membership/magnitude progression')
        for channel in range(3):
            if mask&(1<<channel) and not recovered[channel]:recovered[channel]=level
        prior_mask,prior_level=mask,level
    if bytes(recovered)!=raw:raise ValueError('independent sample inverse mismatch')

def _payload(row,raw,layer):
    if row['first']!=raw['first'] or row['count']!=raw['count'] or any(raw[k]!=layer[k] for k in ('first','count','total')):raise ValueError('payload/view geometry mismatch')
    decoded=[]
    for prefix,view in [('raw',raw),('layered',layer)]:
        value=row[prefix+'_hex'];hash_value=row[prefix+'_fnv64']
        if not re.fullmatch('[0-9a-f]{16}',hash_value) or not re.fullmatch('[0-9a-f]+',value) or len(value)!=2*min(18,view['bytes']):raise ValueError('bounded full hash/sample shape required')
        data=bytes.fromhex(value);decoded.append(data)
        # A full-payload hash can be independently checked only when all bytes
        # fit the emitted sample. Never hash a prefix as though it were the view.
        if view['bytes']<=18 and _fnv(data)!=hash_value:raise ValueError('full small-window FNV mismatch')
    raw_bytes,layer_bytes=decoded
    for index in range(min(len(raw_bytes)//3,len(layer_bytes)//6)):
        _inverse(raw_bytes[index*3:index*3+3],layer_bytes[index*6:index*6+6])
    return {'first':row['first'],'count':row['count'],'raw_fnv64':row['raw_fnv64'],'layered_fnv64':row['layered_fnv64'],'independent_sample_columns':min(len(raw_bytes)//3,len(layer_bytes)//6),'full_payload_independently_available':raw['bytes']<=18 and layer['bytes']<=18}

def _validate_context(context,expected):
    required={'profile','owner_nonce','core_build_id','core_sha256','mapped_probe_sha256','probe_package_sha256','public_header_sha256','core_source_manifest_sha256','source_port'}
    if not isinstance(expected,dict) or set(expected)!=required:raise ValueError('complete externally bound expected identity required')
    if expected['profile'] not in ('130','200') or type(expected['source_port']) is not int or expected['source_port']!=0:raise ValueError('qualified profile/public port required')
    for key in required-{'profile','source_port'}:
        length=32 if key=='owner_nonce' else 40 if key=='core_build_id' else 64
        if not isinstance(expected[key],str) or not re.fullmatch('[0-9a-f]{%d}'%length,expected[key]):raise ValueError('malformed expected identity')
    validate_context(context,expected)
    for key in ('new_process','pin_before','pin_after','old_process'):
        _uint(context[key]['pid'],31,True);_uint(context[key]['birth'],64,True)
    return _uint(context.get('capture_monotonic_ns'),64,True)

def _partial_integrity(records,now):
    """Check observable contradictions even if a worker stopped incomplete.

    A clock failure after accepting a ticket can log its cleanup release without
    a query row. That blind spot must remain INCOMPLETE, never invented lineage.
    """
    reference=None;clock=0;queries={};views={};total=None
    for index,row in enumerate(records[:-1]):
        event=row['event']
        if event=='start':
            if index!=0 or any(row[k]!=v for k,v in dict(window=64,max_polls=256,max_calls=256,deadline_ns=30_000_000_000).items()):raise ValueError('changed producer budget/start')
        elif event=='resolve':
            if index!=1 or reference is not None:raise ValueError('duplicate/misordered resolved reference')
            reference=_ref(row)
        elif event=='query':
            if reference is None or _ref(row)!=reference:raise ValueError('queried public reference changed')
            if not (compare_timestamps(clock,row['query_ns'])<=0 and compare_timestamps(row['query_ns'],now+10_000_000)<=0) or not row['query_ns']:raise ValueError('backward/future public query clock')
            clock=row['query_ns']
            if row['ticket']:queries[row['ticket']]=row
        elif event=='view':
            query=queries.get(row['ticket'])
            if query is None or query['rc']!=0 or reference is None or _ref(row)!=reference:raise ValueError('view lacks matching public query/reference')
            if any(row[k]!=query[k] for k in ('first','encoding')):raise ValueError('view changed query')
            extent=_uint(row['total'],32,True);stride={3:3,8:6}.get(row['encoding']);count=min(extent-row['first'],query['requested'])
            if total is not None and total!=extent:raise ValueError('stored extent changed')
            total=extent
            if not stride or not 0<=row['first']<extent or not 0<count<=64 or row['count']!=count or row['stride']!=stride or row['bytes']!=count*stride:raise ValueError('invalid partial view geometry')
            if row['availability']!=2 or row['provenance']!=(2 if stride==3 else 3) or not row['revision'] or not row['observed_ns'] or compare_timestamps(row['observed_ns'],now+10_000_000)>0:raise ValueError('invalid partial provenance/clock')
            if not _analysis_axis(row):raise ValueError('analysis axis mismatch')
            views[row['ticket']]=row
        elif event=='payload':
            raw=views.get(row['raw_ticket']);layer=views.get(row['layered_ticket'])
            if raw is None or layer is None or raw['encoding']!=3 or layer['encoding']!=8:raise ValueError('payload lacks raw/derived views')
            _payload(row,raw,layer)
        elif event=='release':
            queries.pop(row['ticket'],None);views.pop(row['ticket'],None)

def _check(text,context,expected):
    now=_validate_context(context,expected);records=_records(text);final=records[-1]
    _partial_integrity(records,now)
    if final['calls']>256+24*min(2,final['accepted']) or final['accepted']>12 or final['released']>final['accepted'] or final['outstanding']>2 or final['outstanding']!=final['accepted']-final['released']:raise ValueError('impossible sealed worker counters')
    # Producer absence/timeout/failure remains its explicit result; malformed
    # grammar and external identity are rejected even on incomplete captures.
    if final['status']!='PASS':
        return {'status':final['status'],'reason':final['reason'],'outstanding':final['outstanding'],'scope':'bounded probe result, no waveform acceptance'}
    if records[0]!=dict(v='1',seq='1',port='0',event='start',window=64,max_polls=256,max_calls=256,deadline_ns=30_000_000_000):raise ValueError('exact sealed probe budgets required')
    if len(records)<3 or records[1]['event']!='resolve':raise ValueError('one resolved public reference required')
    reference=_ref(records[1]);active={};views={};queries=[];payloads=[];accepted=0;released=0;clock=0;initial=None
    for row in records[2:-1]:
        event=row['event']
        if event=='query':
            if _ref(row)!=reference:raise ValueError('queried public reference changed')
            if not (compare_timestamps(clock,row['query_ns'])<=0 and compare_timestamps(row['query_ns'],now+10_000_000)<=0) or not row['query_ns']:raise ValueError('backward/future public query clock')
            clock=row['query_ns'];_uint(row['encoding'],32);_uint(row['first'],32);_uint(row['requested'],32)
            queries.append(row)
            if row['ticket']:
                if row['ticket'] in active or len(active)>=2:raise ValueError('duplicate/too many outstanding tickets')
                active[row['ticket']]=row;accepted+=1
            elif row['rc']==0:raise ValueError('successful query needs owned ticket')
        elif event=='view':
            query=active.get(row['ticket'])
            if query is None or query['rc']!=0 or row['ticket'] in views:raise ValueError('view lacks current successful ticket')
            if _ref(row)!=reference or any(row[k]!=query[k] for k in ('encoding','first')):raise ValueError('view/reference/query mismatch')
            total=_uint(row['total'],32,True);count=min(total-row['first'],query['requested']);stride=3 if row['encoding']==3 else 6 if row['encoding']==8 else 0
            if not stride or not 0<=row['first']<total or not 0<count<=64 or row['count']!=count or row['stride']!=stride or row['bytes']!=count*stride:raise ValueError('invalid returned geometry/bounds')
            if row['availability']!=2 or row['provenance']!=(2 if stride==3 else 3) or not row['revision'] or not row['observed_ns'] or compare_timestamps(row['observed_ns'],now+10_000_000)>0:raise ValueError('availability/provenance/receipt mismatch')
            if not _analysis_axis(row):raise ValueError('analysis axis mismatch')
            if initial is None:
                if query['encoding']!=3 or query['first']!=0 or query['requested']!=1:raise ValueError('initial raw extent request required')
                initial=row
            elif total!=initial['total']:raise ValueError('stored extent changed')
            views[row['ticket']]=row
        elif event=='payload':
            raw=views.get(row['raw_ticket']);layer=views.get(row['layered_ticket'])
            if raw is None or layer is None or row['raw_ticket'] not in active or row['layered_ticket'] not in active or raw['encoding']!=3 or layer['encoding']!=8:raise ValueError('payload needs both current encoding3/8 views')
            payloads.append(_payload(row,raw,layer))
        elif event=='release':
            if row['ticket'] not in active or row['rc']!=0:raise ValueError('unowned/failed/duplicate release')
            del active[row['ticket']];views.pop(row['ticket'],None);released+=1
        else:raise ValueError('unexpected event inside request sequence')
    if initial is None:raise ValueError('missing initial waveform extent')
    total=initial['total'];expected_queries=[(3,0,1,0),(3,total//2,64,0),(8,total//2,64,0),(3,total-1,4096,0),(8,total-1,4096,0)]
    expected_queries += [(encoding,first,count,-8 if count==4097 else -3) for encoding in (3,8) for first,count in ((0,0),(0,4097),(total,1))]
    expected_queries += [(99,0,1,-2)]
    if [(r['encoding'],r['first'],r['requested'],r['rc']) for r in queries]!=expected_queries:raise ValueError('missing/wrong exact encoding8, window or refusal request')
    if [(r['first'],r['count']) for r in payloads]!=[(total//2,min(64,total-total//2)),(total-1,1)]:raise ValueError('both middle and clipped-tail payloads required')
    if active or final['outstanding'] or final['accepted']!=accepted or final['released']!=released or accepted!=released:raise ValueError('outstanding ticket or ownership counter mismatch')
    if not 2+len(queries)+accepted+released<=final['calls']<=304:raise ValueError('impossible bounded service-call count')
    if compare_timestamps(queries[-1]['query_ns']-queries[0]['query_ns'],30_000_000_000)>=0:raise ValueError('ordinary query span exceeded producer deadline')
    if final['reason']!='verified':raise ValueError('PASS must be sealed verification completion')
    return {'status':'PASS','public_reference':dict(zip(('source','generation','library','track'),reference)),'windows':payloads,'accepted':accepted,'released':released,'outstanding':0,'independent_checks':'exact transcript/identity/tickets/geometry/provenance/refusals and emitted sample inverse; full FNV only when complete small payload emitted','full_window_inverse':'attested by exact mapped sealed probe, full bytes absent from log','scope':'uncalibrated stored analysis only; no deck/source-load/paused/commands/pixels/audio acceptance'}

def validate_wave_capture(text,context,expected):
    try:return _check(text,context,expected)
    except MissingEvidence as error:return {'status':'INCOMPLETE','missing':[str(error)]}
    except (ValueError,KeyError,IndexError,TypeError,OverflowError) as error:return {'status':'FAIL','errors':[str(error)]}

def check_startup_prerequisites(context,expected,evidence):
    """No qualified Binding-data producer exists in the supplied handoff.

    This guard intentionally cannot qualify installation from booleans, flags,
    hook-table rows or symbol names. It never calls a private native function.
    """
    try:
        _validate_context(context,expected)
        if not isinstance(evidence,dict) or evidence.get('mode') not in ('opt_in','opt_out'):raise ValueError('fresh startup phase required')
        if evidence.get('profile')!=expected['profile']:raise ValueError('startup profile changed')
        flags=evidence.get('flags')
        required={'azmod.track-data.on','azmod.native-sources.on','azmod.native-connections.on'}
        if not isinstance(flags,dict) or not required<=set(flags)<=required|{'azmod.native-source-files.on'} or any(type(v) is not bool for v in flags.values()):raise ValueError('exact RAM startup flags required')
        if evidence['mode']=='opt_in' and not all(flags.values()):raise ValueError('incomplete startup opt-in gate')
        if evidence['mode']=='opt_out' and flags['azmod.native-connections.on']:raise ValueError('opt-out connections flag must be absent')
        return {'status':'INCOMPLETE','configuration':'recorded only, not installed-hook evidence','missing':['qualified symbol/code-bound read-only Binding layout and lifetime proof','current-process close/open entry branches and owned islands/replacements','genuine distinct original trampoline publication and armed binding snapshot'],'scope':'no installation, native connection lifecycle or source/load acceptance'}
    except (ValueError,KeyError,TypeError) as error:return {'status':'FAIL','errors':[str(error)]}


def wave_probe_archive(data,package_sha256,binary_sha256):
    """Read only an exact sealed wave package; does not approve or load it."""
    import hashlib,io,json,zipfile
    if type(data) is not bytes or len(data)>1048576 or any(not isinstance(v,str) or not re.fullmatch('[0-9a-f]{64}',v) for v in (package_sha256,binary_sha256)) or hashlib.sha256(data).hexdigest()!=package_sha256:raise ValueError('exact bounded wave package hash required')
    names={'azmod.json','sdk-wave-layer-probe.so','LICENSE','icon.png'}
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            entries=archive.infolist()
            if len(entries)!=4 or {e.filename for e in entries}!=names or any(e.is_dir() or e.file_size>262144 or (e.external_attr>>16)&0o170000 not in (0,0o100000) or e.flag_bits&1 for e in entries):raise ValueError('four regular wave package members required')
            files={name:archive.read(name) for name in names}
    except (zipfile.BadZipFile,RuntimeError,OSError) as error:raise ValueError('unreadable wave package') from error
    if hashlib.sha256(files['sdk-wave-layer-probe.so']).hexdigest()!=binary_sha256:raise ValueError('mapped wave probe binary hash mismatch')
    def object_fields(pairs):
        if len({k for k,v in pairs})!=len(pairs):raise ValueError('duplicate package metadata')
        return dict(pairs)
    meta=json.loads(files['azmod.json'],object_pairs_hook=object_fields)
    if (meta.get('id'),meta.get('version'),meta.get('firmware'))!=('sdk-wave-layer-probe','0.1.0',['any']) or type(meta.get('azmod')) is not int or meta['azmod']!=1 or type(meta.get('api')) is not int or meta['api']!=1 or meta.get('hooks') is not False:raise ValueError('reviewed wave mod identity/ABI/hooks required')
    return files

def wave_approved(text,sha):
    if not isinstance(text,str) or len(text.encode())>32768 or not re.fullmatch('[0-9a-f]{64}',sha):raise ValueError('bounded approval read required')
    return any(line.split()==['sdk-wave-layer-probe',sha] for line in text.splitlines())

def wave_stage_script(token):
    if not isinstance(token,str) or not re.fullmatch('[a-f0-9]{32}',token):raise ValueError('owned nonce required')
    root='/home/root/settings/az-usbload/community';target=root+'/sdk-wave-layer-probe';stage=root+'/.sdk-wave-'+token
    return ('set -e\numask 077\ntest ! -e '+target+'\nmkdir -p '+root+'\nmkdir '+stage+'\ntar -xf - -C '+stage+'\nmv '+stage+' '+target+'\n'
            'test -f '+root+'/enabled || : >'+root+'/enabled\n'
            'grep -qx sdk-wave-layer-probe '+root+'/enabled || printf \'sdk-wave-layer-probe\\n\' >>'+root+'/enabled\n'
            'touch /home/root/settings/azmod-logs.on\nsha256sum '+
            ' '.join(target+'/'+name for name in ('azmod.json','sdk-wave-layer-probe.so','LICENSE','icon.png'))+'\n')
