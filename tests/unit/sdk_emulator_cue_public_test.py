"""Public cue callback/query admission, with synthetic records only."""
import copy
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
import sdk_emulator_public as p
NOW=200_000_000_000
def data():return [dict(v=1,seq=d,deck=d,returned=d,query_ns=NOW-1000,rc=0,valid=524288,cue_valid=1,cue_unit=d,cue_rev=2,cue_ns=NOW-5000,event_ns=NOW-4000,source_load=0) for d in range(1,5)]
def text(rows):return '\n'.join('SDK_PUBLIC_CUE '+' '.join(str(k)+'='+str(v) for k,v in r.items()) for r in rows)+'\n'
class Cue(unittest.TestCase):
    def test_four_public_queries_and_genuine_post_load_callback_are_separate_requirements(self):
        rows=p.cue_rows(text(data()),NOW);self.assertEqual(set(rows),{1,2,3,4})
        for row in rows.values():self.assertTrue(p.cue_witness(row,NOW-10000))
        for field in ('query_ns','cue_ns','event_ns'):
            row=copy.deepcopy(rows[1]);row[field]=NOW-10000
            self.assertFalse(p.cue_witness(row,NOW-10000))
        rows=data();rows[0]['event_ns']=0
        got=p.cue_rows(text(rows),NOW);self.assertFalse(p.cue_witness(got[1],1))
    def test_unavailable_does_not_become_a_callback_or_revision(self):
        rows=data();rows[0].update(cue_valid=0,cue_unit=0,cue_rev=0,cue_ns=0,event_ns=0,valid=0)
        got=p.cue_rows(text(rows),NOW);self.assertFalse(p.cue_witness(got[1],1))
    def test_clock_deck_validity_source_and_counter_guards(self):
        for change in (dict(v=2),dict(seq=2),dict(returned=2),dict(valid=0),dict(cue_unit=0),dict(cue_rev=0),dict(cue_ns=0),dict(event_ns=-1),dict(cue_ns=NOW),dict(event_ns=NOW),dict(query_ns=NOW+10_000_001),dict(source_load=1),dict(valid=524288|2),dict(extra=1)):
            rows=data();rows[0].update(change)
            with self.subTest(change=change),self.assertRaises(ValueError):p.cue_rows(text(rows),NOW)
    def test_fresh_query_all_four_and_bounded_record_requirements(self):
        with self.assertRaises(p.MissingEvidence):p.cue_rows(text(data()[:3]),NOW)
        rows=data()
        for row in rows:row.update(query_ns=NOW-5_000_000_001,cue_ns=1,event_ns=2)
        with self.assertRaises(p.StaleObservation):p.cue_rows(text(rows),NOW)
        with self.assertRaises(ValueError):p.cue_rows('x'*524289,NOW)
if __name__=='__main__':unittest.main()
