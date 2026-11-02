"""Public native/BPM observation admission; synthetic data, no target I/O."""
import copy,importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('native_public',ROOT/'tests/integration/sdk_emulator_public.py');p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
NOW=20_000_000_000
def data():
 return [dict(v=1,seq=d,deck=d,returned=d,query_ns=NOW-1000,rc=0,valid=1024|262144|64|131072,master_valid=1,master=int(d==2),master_rev=9,master_ns=1_000_000_000,load_valid=1,native_load=1,load_unit=100+d,load_epoch=1,load_ns=18_000_000_000,source_load=0,tempo_valid=1,tempo='128.500000',tempo_state=10+d,tempo_rev=2,tempo_ns=1_000_000_000,slider_valid=1,slider_locked=0,slider_unit=20+d,slider_rev=3,slider_ns=2_000_000_000) for d in range(1,5)]
def text(rows):return '\n'.join('SDK_PUBLIC_NATIVE '+' '.join(str(k)+'='+str(v) for k,v in r.items()) for r in rows)+'\n'
class Native(unittest.TestCase):
 def read(self,rows=None):return p.native_rows(text(data() if rows is None else rows),NOW)
 def test_all_fields_and_shared_master(self):
  got=self.read();self.assertEqual(p.confirmed_master(got),2);self.assertEqual(got[4]['tempo'],128.5)
  self.assertTrue(p.native_load_after(got[4],17_000_000_000));self.assertFalse(p.native_load_after(got[4],18_000_000_000))
 def test_source_qualified_generation_and_track_cannot_leak(self):
  for change in (dict(source_load=1),dict(valid=1024|262144|2)):
   r=data();r[0].update(change)
   with self.subTest(change=change),self.assertRaises(ValueError):self.read(r)
 def test_native_receipt_guards(self):
  for change in (dict(valid=0),dict(native_load=0),dict(load_unit=0),dict(load_epoch=0),dict(load_ns=NOW+1),dict(master=2),dict(master_rev=0),dict(master_ns=NOW+1),dict(returned=2),dict(rc=-1)):
   r=data();r[0].update(change)
   with self.subTest(change=change),self.assertRaises(ValueError):self.read(r)
 def test_unknown_remains_unknown_and_master_cohort_is_required(self):
  r=data()
  for row in r:row.update(master_valid=0,master=-1,load_valid=0,native_load=0)
  got=self.read(r);self.assertFalse(p.native_load_after(got[1],1))
  with self.assertRaises(p.MissingEvidence):p.confirmed_master(got)
  r=data();r[0]['master_rev']+=1
  with self.assertRaises(p.MissingEvidence):p.confirmed_master(self.read(r))
  r=data();r[0]['master']=1
  with self.assertRaises(ValueError):p.confirmed_master(self.read(r))
 def test_combined_tempo_guards_still_apply(self):
  for change in (dict(tempo='nan'),dict(tempo='400.000000'),dict(tempo_state=0),dict(slider_locked=2),dict(slider_ns=NOW+1)):
   r=data();r[0].update(change)
   with self.subTest(change=change),self.assertRaises(ValueError):self.read(r)
 def test_exact_sequence_decks_clock_and_record_bounds(self):
  for change in (dict(seq=2),dict(query_ns=NOW+10_000_001),dict(extra=1)):
   r=data();r[0].update(change)
   with self.subTest(change=change),self.assertRaises(ValueError):self.read(r)
  with self.assertRaises(p.MissingEvidence):self.read(data()[:3])
  r=data()
  for row in r:row.update(query_ns=NOW-5_000_000_001,load_ns=10_000_000_000)
  with self.assertRaises(p.StaleObservation):self.read(r)
  with self.assertRaises(ValueError):p.native_rows('x'*524289,NOW)
if __name__=='__main__':unittest.main()
