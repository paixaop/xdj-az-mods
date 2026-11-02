from pathlib import Path
import sys, unittest, math
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
from sdk_timestamps import compare_timestamps

class TimestampTests(unittest.TestCase):
 def test_source_rates_and_symmetric_windows(self):
  for rate in (44100,48000,96000,192000):
   self.assertEqual(compare_timestamps(62048,62157,units_per_second=rate,sample_rate=rate,window_samples=256),0)
   self.assertEqual(compare_timestamps(62157,62048,units_per_second=rate,sample_rate=rate,window_samples=256),0)
   self.assertEqual(compare_timestamps(255,256,units_per_second=rate,sample_rate=rate,window_samples=256),0)
   self.assertEqual(compare_timestamps(1.,1.+.5/rate,units_per_second=1,sample_rate=rate),0)
   self.assertEqual(compare_timestamps(1.,1.+1.5/rate,units_per_second=1,sample_rate=rate),-1)
 def test_strict_boundary_and_wide_integer_clocks(self):
  self.assertEqual(compare_timestamps(0,256,units_per_second=44100,sample_rate=44100,window_samples=256),-1)
  self.assertEqual(compare_timestamps(256,0,units_per_second=44100,sample_rate=44100,window_samples=256),1)
  self.assertEqual(compare_timestamps(2**64-1,2**64-2),1)
  self.assertEqual(compare_timestamps(2**64-1,2**64-2,window_samples=2),0)
  self.assertEqual(compare_timestamps(0.,1/128,units_per_second=1,sample_rate=128),-1)
 def test_invalid_values_refuse(self):
  with self.assertRaisesRegex(ValueError,'explicit timestamp units'):
   compare_timestamps(.5,0.)
  for value in (math.nan,math.inf,True,'1'):
   with self.assertRaises(ValueError):compare_timestamps(value,0)
  for keyword in ('units_per_second','sample_rate','window_samples'):
   with self.assertRaises(ValueError):compare_timestamps(0,0,**{keyword:0})

if __name__=='__main__':unittest.main()
