"""Admission of the public probe's independent BPM/slider rows."""
import importlib.util
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('tempo_public',ROOT/'tests/integration/sdk_emulator_public.py')
public=importlib.util.module_from_spec(spec);spec.loader.exec_module(public)
NOW=20_000_000_000

def rows(**changes):
 result=[]
 for deck in range(1,5):
  row=dict(v=1,seq=deck,deck=deck,returned=deck,query_ns=NOW-1000,rc=0,valid=64|131072,tempo_valid=1,tempo='128.500000',tempo_state=10+deck,tempo_rev=2,tempo_ns=1_000_000_000,slider_valid=1,slider_locked=0,slider_unit=20+deck,slider_rev=3,slider_ns=2_000_000_000)
  row.update(changes)
  result.append('SDK_PUBLIC_TEMPO '+' '.join(str(k)+'='+str(v) for k,v in row.items()))
 return '\n'.join(result)+'\n'
class Tempo(unittest.TestCase):
 def test_both_fields_and_retained_receipt(self):
  got=public.tempo_rows(rows(),NOW)
  self.assertEqual(set(got),set(range(1,5)))
  self.assertEqual(got[4]['tempo'],128.5)
  self.assertEqual(got[4]['slider_locked'],0)
 def test_unknown_stays_unknown(self):
  got=public.tempo_rows(rows(tempo_valid=0,tempo='0.000000',slider_valid=0,slider_locked=-1),NOW)
  self.assertFalse(got[1]['tempo_valid']);self.assertFalse(got[1]['slider_valid'])
 def test_false_scalar_or_receipt_claims_refused(self):
  for change in (dict(valid=0),dict(tempo='20.000000'),dict(tempo='400.000000'),dict(tempo='nan'),dict(tempo_state=0),dict(tempo_ns=NOW+1),dict(slider_locked=2),dict(slider_unit=0),dict(slider_ns=NOW+1),dict(rc=-1),dict(returned=4)):
   with self.subTest(change=change),self.assertRaises(ValueError):public.tempo_rows(rows(**change),NOW)
 def test_unknown_scalar_cannot_carry_known_value(self):
  for change in (dict(tempo_valid=0),dict(slider_valid=0)):
   with self.subTest(change=change),self.assertRaises(ValueError):public.tempo_rows(rows(**change),NOW)
 def test_fresh_query_not_native_receipt_age(self):
  with self.assertRaises(public.StaleObservation):public.tempo_rows(rows(query_ns=NOW-5_000_000_001),NOW)
  with self.assertRaises(ValueError):public.tempo_rows(rows(query_ns=NOW+10_000_001),NOW)
 def test_exact_rows_and_all_four_required(self):
  good=rows()
  for bad in ('',good.splitlines()[0],good.replace('seq=2','seq=1'),good.replace('tempo=128.500000','tempo=128.500000 extra=1'),good+'x'*262145):
   with self.subTest(bad=bad[:60]),self.assertRaises((ValueError,public.MissingEvidence)):public.tempo_rows(bad,NOW)
if __name__=='__main__':unittest.main()
