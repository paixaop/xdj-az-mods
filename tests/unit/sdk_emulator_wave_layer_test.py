#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Synthetic log/context TDD only; no compiler, Docker, target or native I/O."""
import copy,hashlib,importlib.util,io,json,sys,warnings,zipfile
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/integration'))
spec=importlib.util.spec_from_file_location('wave_checker',ROOT/'tests/integration/sdk_emulator_wave_layer.py');checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)
EXPECTED={'profile':'130','owner_nonce':'a'*32,'core_build_id':'b'*40,'core_sha256':'c'*64,'mapped_probe_sha256':'d'*64,'probe_package_sha256':'e'*64,'public_header_sha256':'f'*64,'core_source_manifest_sha256':'1'*64,'source_port':0}
def context():
 return dict(EXPECTED,old_process={'pid':'100','birth':'10'},new_process={'pid':'101','birth':'20'},pin_before={'pid':'101','birth':'20'},pin_after={'pid':'101','birth':'20'},old_absent=True,approval_origin='actual_ui',approved_probe_sha256='d'*64,apply_count=1,loader='normal_sdk_entry_library_service',capture_monotonic_ns=3_000_000_000)
def fnv(data):
 h=14695981039346656037
 for b in data:h=((h^b)*1099511628211)&((1<<64)-1)
 return f'{h:016x}'
RAW=bytes((255,128,1));LAYER=bytes((0x63,0x38,255,128,1,0))
def fixture(terminal_range=True,total=1000):
 records=[];ref=dict(source=101,generation=202,library=303,track=9);ticket=0;clock=1_000_000_000;accepted=0
 def row(event,**fields):records.append(dict(event=event,**fields))
 def query(encoding,first,requested,rc=0,accept=True):
  nonlocal ticket,clock,accepted
  clock+=1000
  if accept:ticket+=1;accepted+=1
  tid=ticket if accept else 0
  row('query',query_ns=clock,ticket=tid,encoding=encoding,first=first,requested=requested,rc=rc,**ref);return tid
 def view(tid,encoding,first,count):row('view',ticket=tid,encoding=encoding,first=first,count=count,total=total,stride=3 if encoding==3 else 6,bytes=count*(3 if encoding==3 else 6),availability=2,provenance=2 if encoding==3 else 3,revision=77,observed_ns=123,**ref,start_s=f'{first/150:.9f}',step_s='0.006666666667')
 row('start',window=64,max_polls=256,max_calls=256,deadline_ns=30_000_000_000);row('resolve',**ref)
 t=query(3,0,1);view(t,3,0,1);row('release',ticket=t,rc=0)
 for first,requested in [(total//2,64),(total-1,4096)]:
  count=min(total-first,requested);a=query(3,first,requested);b=query(8,first,requested)
  view(a,3,first,count);view(b,8,first,count)
  raw=RAW*count;layer=LAYER*count
  row('payload',first=first,count=count,raw_ticket=a,layered_ticket=b,raw_fnv64=fnv(raw),layered_fnv64=fnv(layer),raw_hex=raw[:18].hex(),layered_hex=layer[:18].hex())
  row('release',ticket=b,rc=0);row('release',ticket=a,rc=0)
 for encoding in (3,8):
  query(encoding,0,0,-3,False);query(encoding,0,4097,-8,False)
  t=query(encoding,total,1,-3,terminal_range)
  if t:row('release',ticket=t,rc=0)
 query(99,0,1,-2,False)
 row('final',status='PASS',reason='verified',calls=40,accepted=accepted,released=accepted,outstanding=0)
 return records

def text(records):return '\n'.join('SDK_PUBLIC_WAVE v=1 seq=%d port=0 '%(i+1)+' '.join(k+'='+str(v) for k,v in row.items()) for i,row in enumerate(records))+'\n'
class Wave(unittest.TestCase):
 def result(self,records=None,ctx=None):return checker.validate_wave_capture(text(fixture() if records is None else records),context() if ctx is None else ctx,EXPECTED)
 def test_pending_call_budget_keeps_cleanup_reserve_bounded_per_owned_ticket(self):
  records=fixture()[:3];records[2]['rc']=1;ticket=records[2]['ticket']
  records += [dict(event='release',ticket=ticket,rc=0),dict(event='final',status='INCOMPLETE',reason='call_budget',calls=257,accepted=1,released=1,outstanding=0)]
  self.assertEqual(self.result(records)['status'],'INCOMPLETE')
  records[-1]['calls']=281
  self.assertEqual(self.result(records)['status'],'FAIL')
 def test_exact_synthetic_good_and_immediate_refusals(self):
  for terminal in (False,True):self.assertEqual(self.result(fixture(terminal))['status'],'PASS')
 def test_missing_encoding8_cannot_be_hidden_by_final_PASS(self):
  records=fixture();records=[r for r in records if r.get('encoding')!=8]
  self.assertEqual(self.result(records)['status'],'FAIL')
 def test_payload_inverse_and_reserved_zero(self):
  for change in ('reserved','inverse','unused','id','hash'):
   records=fixture();row=next(r for r in records if r['event']=='payload');data=bytearray.fromhex(row['layered_hex'])
   if change=='reserved':data[5]=1
   elif change=='inverse':data[2]^=1
   elif change=='unused':data[0]=0x02;data[1]=0x10
   elif change=='id':data[0]=0x67
   else:
    row=next(r for r in reversed(records) if r['event']=='payload');row['layered_fnv64']='0'*16
   if change!='hash':row['layered_hex']=data.hex()
   with self.subTest(change=change):self.assertEqual(self.result(records)['status'],'FAIL')
 def test_ref_geometry_provenance_and_axis(self):
  for field,value in [('source',999),('generation',999),('track',99),('provenance',2),('stride',3),('bytes',1),('count',65),('first',0),('total',999),('availability',1),('step_s','0.000022675737')]:
   records=fixture();next(r for r in records if r['event']=='view' and r['encoding']==8)[field]=value
   with self.subTest(field=field):self.assertEqual(self.result(records)['status'],'FAIL')
 def test_analysis_timestamp_uses_its_actual_frame_window(self):
  for shift,status in ((.002,'PASS'),(-.002,'PASS'),(.01,'FAIL'),(-.01,'FAIL')):
   records=fixture();row=next(r for r in records if r['event']=='view' and r['first']==500)
   row['start_s']=f'{row["first"]/150+shift:.9f}'
   with self.subTest(shift=shift):self.assertEqual(self.result(records)['status'],status)
 def test_view_clock_future_boundary_uses_clock_comparator(self):
  for extra,status in ((0,'PASS'),(1,'FAIL')):
   records=fixture();next(r for r in records if r['event']=='view')['observed_ns']=3_010_000_000+extra
   with self.subTest(extra=extra):self.assertEqual(self.result(records)['status'],status)
 def test_wrong_refusal_status_missing_bounds_and_outstanding_ticket(self):
  for mode in ('status','missing','outstanding','double_release','orphan_view'):
   records=fixture()
   if mode=='status':next(r for r in records if r['event']=='query' and r['encoding']==99)['rc']=-3
   elif mode=='missing':records=[r for r in records if not(r['event']=='query' and r['requested']==4097)]
   elif mode=='outstanding':records[-1]['outstanding']=1
   elif mode=='double_release':records.insert(-1,copy.deepcopy(next(r for r in records if r['event']=='release')))
   else:next(r for r in records if r['event']=='view')['ticket']=999
   with self.subTest(mode=mode):self.assertEqual(self.result(records)['status'],'FAIL')
 def test_wrong_process_source_build_or_lifecycle(self):
  for field,value in [('mapped_probe_sha256','0'*64),('core_build_id','0'*40),('core_source_manifest_sha256','0'*64),('source_port',1),('apply_count',2),('approval_origin','file_edit'),('old_absent',False)]:
   ctx=context();ctx[field]=value
   with self.subTest(field=field):self.assertEqual(self.result(ctx=ctx)['status'],'FAIL')
  for field in ('new_process','pin_before','pin_after'):
   ctx=context();ctx[field]={'pid':'102','birth':'21'}
   with self.subTest(field=field):self.assertEqual(self.result(ctx=ctx)['status'],'FAIL')
 def test_final_incomplete_missing_final_and_fail_are_not_promoted(self):
  for status in ('INCOMPLETE','FAIL'):
   records=fixture();records[-1].update(status=status,reason='layered_unavailable')
   self.assertEqual(self.result(records)['status'],status)
  self.assertEqual(self.result(fixture()[:-1])['status'],'INCOMPLETE')
 def test_duplicate_unknown_fields_sequence_and_bounds(self):
  good=text(fixture())
  for bad in (good.replace('seq=2 ','seq=1 ',1),good.replace('encoding=8 ','encoding=8 surprise=1 ',1),good.replace('event=start ','event=start v=1 ',1),good+'x'*40000,good.replace('query_ns=1000001000','query_ns=999999999999999999999999')):
   with self.subTest(bad=bad[:80]):self.assertEqual(checker.validate_wave_capture(bad,context(),EXPECTED)['status'],'FAIL')
 def test_future_query_or_backward_time_is_refused(self):
  for value in (4_000_000_000,1):
   records=fixture();[r for r in records if r['event']=='query'][1]['query_ns']=value
   self.assertEqual(self.result(records)['status'],'FAIL')
 def test_small_clipped_windows_and_all_zero_inverse(self):
  for total in (1,2,3,4,5,1000):self.assertEqual(self.result(fixture(total=total))['status'],'PASS')
  zero=fixture(total=1)
  for row in zero:
   if row['event']=='payload':
    row.update(raw_hex='000000',layered_hex='081000000000',raw_fnv64=fnv(bytes(3)),layered_fnv64=fnv(bytes((8,16,0,0,0,0))))
  self.assertEqual(self.result(zero)['status'],'PASS')
 def test_released_ticket_id_may_be_reused_only_in_new_lineage(self):
  records=fixture();old=next(r['ticket'] for r in records if r['event']=='query' and r['first']==500 and r['encoding']==3)
  for row in records:
   if row.get('ticket')==old:row['ticket']=1
   if row.get('raw_ticket')==old:row['raw_ticket']=1
  self.assertEqual(self.result(records)['status'],'PASS')
 def test_no_new_payload_claim_when_full_window_bytes_are_not_logged(self):
  result=self.result();self.assertEqual(result['status'],'PASS')
  self.assertFalse(result['windows'][0]['full_payload_independently_available'])
  self.assertTrue(result['windows'][1]['full_payload_independently_available'])
  self.assertEqual(result['windows'][0]['independent_sample_columns'],3)
 def test_incomplete_still_rejects_observed_reference_and_clock_corruption(self):
  for mode in ('source','clock','reserved'):
   records=fixture();records[-1].update(status='INCOMPLETE',reason='release')
   if mode=='source':next(r for r in records if r['event']=='query')['source']=999
   elif mode=='clock':next(r for r in records if r['event']=='query')['query_ns']=9_000_000_000
   else:
    row=next(r for r in records if r['event']=='payload');data=bytearray.fromhex(row['layered_hex']);data[5]=1;row['layered_hex']=data.hex()
   with self.subTest(mode=mode):self.assertEqual(self.result(records)['status'],'FAIL')
 def test_missing_service_and_unlogged_cleanup_lineage_stay_incomplete(self):
  records=[dict(event='final',status='INCOMPLETE',reason='service',calls=0,accepted=0,released=0,outstanding=0)]
  self.assertEqual(self.result(records)['status'],'INCOMPLETE')
  records=fixture()[:2]+[dict(event='release',ticket=999,rc=0),dict(event='final',status='INCOMPLETE',reason='clock',calls=4,accepted=1,released=1,outstanding=0)]
  self.assertEqual(self.result(records)['status'],'INCOMPLETE')
 def test_strict_context_integer_ranges_and_boolean_counter(self):
  for mutate in ('pid','birth','apply','now'):
   ctx=context()
   if mutate=='pid':ctx['new_process']['pid']='2147483648';ctx['pin_before']=ctx['pin_after']=ctx['new_process']
   elif mutate=='birth':ctx['old_process']['birth']=str(1<<64)
   elif mutate=='apply':ctx['apply_count']=True
   else:ctx['capture_monotonic_ns']=True
   with self.subTest(mutate=mutate):self.assertEqual(self.result(ctx=ctx)['status'],'FAIL')
 def test_startup_flags_health_table_and_symbols_never_qualify_installation(self):
  for mode in ('opt_in','opt_out'):
   e={'mode':mode,'profile':'130','flags':{'azmod.track-data.on':True,'azmod.native-sources.on':True,'azmod.native-connections.on':mode=='opt_in'},'health':'PASS','hook_table':['close island','open island'],'binding_symbol_present':True,'installed':True,'armed':True}
   result=checker.check_startup_prerequisites(context(),EXPECTED,e)
   self.assertEqual(result['status'],'INCOMPLETE');self.assertTrue(result['missing'])
 def test_startup_wrong_gate_profile_and_process_refuse(self):
  valid={'mode':'opt_in','profile':'130','flags':{'azmod.track-data.on':True,'azmod.native-sources.on':True,'azmod.native-connections.on':True}}
  for mutation in ('flag','profile','process'):
   e=copy.deepcopy(valid);ctx=context()
   if mutation=='flag':e['flags']['azmod.native-sources.on']=False
   elif mutation=='profile':e['profile']='200'
   else:ctx['pin_after']['birth']='21'
   with self.subTest(mutation=mutation):self.assertEqual(checker.check_startup_prerequisites(ctx,EXPECTED,e)['status'],'FAIL')
 def test_optional_physical_flag_is_configuration_only(self):
  e={'mode':'opt_in','profile':'130','flags':{'azmod.track-data.on':True,'azmod.native-sources.on':True,'azmod.native-connections.on':True,'azmod.native-source-files.on':True}}
  self.assertEqual(checker.check_startup_prerequisites(context(),EXPECTED,e)['status'],'INCOMPLETE')

 def test_impossible_final_counters_are_not_hidden_by_incomplete(self):
  for field,value in [('outstanding',3),('calls',305),('accepted',13),('released',99)]:
   records=fixture();records[-1].update(status='INCOMPLETE',reason='release');records[-1][field]=value
   with self.subTest(field=field):self.assertEqual(self.result(records)['status'],'FAIL')
 def test_wrong_unsigned_width_refuses_even_in_incomplete_prefix(self):
  records=fixture();records[-1].update(status='INCOMPLETE',reason='deadline');next(r for r in records if r['event']=='query')['first']=1<<32
  self.assertEqual(self.result(records)['status'],'FAIL')

class Archive(unittest.TestCase):
 def package(self,metadata=None,binary=b'fixture-elf',extra=None):
  stream=io.BytesIO();meta={'azmod':1,'id':'sdk-wave-layer-probe','version':'0.1.0','api':1,'hooks':False,'firmware':['any']}
  if metadata:meta.update(metadata)
  with zipfile.ZipFile(stream,'w') as z:
   for name,data in [('azmod.json',json.dumps(meta).encode()),('sdk-wave-layer-probe.so',binary),('LICENSE',b'fixture'),('icon.png',b'fixture')]:z.writestr(name,data)
   if extra:
    with warnings.catch_warnings():
     warnings.simplefilter('ignore',UserWarning);z.writestr(*extra)
  return stream.getvalue()
 def check(self,data):return checker.wave_probe_archive(data,hashlib.sha256(data).hexdigest(),hashlib.sha256(b'fixture-elf').hexdigest())
 def test_exact_feature_package(self):self.assertEqual(set(self.check(self.package())),{'azmod.json','sdk-wave-layer-probe.so','LICENSE','icon.png'})
 def test_wrong_binary_or_manifest_refuses(self):
  for data in [self.package(binary=b'wrong')]+[self.package({k:v}) for k,v in [('id','sdk-state-probe'),('hooks',True),('api',True),('firmware',['200'])]]:
   with self.subTest(sha=hashlib.sha256(data).hexdigest()),self.assertRaises(ValueError):self.check(data)
 def test_extra_path_duplicate_and_symlink_refuse(self):
  for name in ('../escape','azmod.json'):
   with self.subTest(name=name),self.assertRaises(ValueError):self.check(self.package(extra=(name,b'x')))
  base=zipfile.ZipFile(io.BytesIO(self.package()));stream=io.BytesIO()
  with zipfile.ZipFile(stream,'w') as z:
   for name in base.namelist():
    entry=zipfile.ZipInfo(name);entry.create_system=3;entry.external_attr=(0o120777 if name=='sdk-wave-layer-probe.so' else 0o100600)<<16
    z.writestr(entry,base.read(name))
  with self.assertRaises(ValueError):self.check(stream.getvalue())
 def test_package_hash_and_approval_are_feature_specific(self):
  with self.assertRaises(ValueError):checker.wave_probe_archive(self.package(),'0'*64,'0'*64)
  sha='a'*64
  self.assertTrue(checker.wave_approved('sdk-wave-layer-probe '+sha+'\n',sha))
  self.assertFalse(checker.wave_approved('sdk-state-probe '+sha+'\n',sha))
  self.assertFalse(checker.wave_approved('sdk-wave-layer-probe '+'b'*64+'\n',sha))
 def test_staging_is_only_owned_ram_normal_loader_not_approval_or_apply(self):
  script=checker.wave_stage_script('a'*32)
  self.assertIn('/home/root/settings/az-usbload/community/sdk-wave-layer-probe',script)
  self.assertNotIn('approved',script);self.assertNotIn('az-community.req',script);self.assertNotIn('/media/usb',script)
  with self.assertRaises(ValueError):checker.wave_stage_script('a; reboot')
if __name__=='__main__':unittest.main()
