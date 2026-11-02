#!/usr/bin/env python3
"""The retained shim's JOG1 raster is raw output, never a deck/Slip receipt."""
from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'integration'))
import sdk_emulator_jog as jog
from sdk_emulator_controls import MissingEvidence


def frame(sequence=4):
    data=bytearray(1<<20)
    struct.pack_into('<IIHHI',data,0,0x31474f4a,sequence,320,240,1)
    data[4096:4096+320*240*4]=bytes(range(256))*(320*240*4//256)
    return data


class JogTests(unittest.TestCase):
    def test_stable_advanced_frame_uses_only_initialized_header_and_raster(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'jog.shm';path.write_bytes(frame())
            guards=[]
            value=jog.read_frame(path,2,lambda:guards.append(True) or True)
            self.assertEqual(value['sequence'],4)
            self.assertEqual(value['pixels'],bytes(range(256))*(320*240*4//256))
            self.assertEqual((value['width'],value['height'],value['format']),(320,240,1))
            self.assertEqual(len(guards),2)
            self.assertIsNone(value['deck'])
            self.assertIsNone(value['slip_enabled'])

    def test_zero_odd_stale_reset_and_malformed_frame_refuse(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'jog.shm'
            samples=[bytes(1<<20),frame(0),frame(3),frame(2)]
            for offset,fmt,value in [(0,'I',0x4a4f4732),(8,'H',640),(10,'H',241),(12,'I',2)]:
                changed=frame();struct.pack_into('<'+fmt,changed,offset,value);samples.append(changed)
            samples.extend([frame()[:-1],frame()+b'x'])
            for i,data in enumerate(samples):
                path.write_bytes(data)
                with self.subTest(i=i),self.assertRaises((ValueError,MissingEvidence)):jog.read_frame(path,2,lambda:True)

    def test_torn_read_is_not_retried_or_promoted(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'jog.shm';path.write_bytes(frame())
            real=os.pread;calls=[]
            def read(fd,size,offset):
                calls.append((size,offset))
                data=real(fd,size,offset)
                if len(calls)==3:
                    changed=bytearray(data);struct.pack_into('<I',changed,4,6);return bytes(changed)
                return data
            with patch.object(jog.os,'pread',side_effect=read),self.assertRaises(MissingEvidence):
                jog.read_frame(path,2,lambda:True)
            self.assertEqual(len(calls),3)

    def test_process_change_and_backing_replacement_are_fatal(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'jog.shm';path.write_bytes(frame())
            states=iter([True,False])
            with self.assertRaises(ValueError):jog.read_frame(path,2,lambda:next(states))
            calls=[]
            def guard():
                calls.append(True)
                if len(calls)==2:
                    replacement=Path(t)/'new';replacement.write_bytes(frame());replacement.replace(path)
                return True
            with self.assertRaises(ValueError):jog.read_frame(path,2,guard)

    def test_symlink_wrong_process_and_invalid_boundary_refuse_before_frame_read(self):
        with tempfile.TemporaryDirectory() as t:
            path=Path(t)/'jog.shm';path.write_bytes(frame())
            link=Path(t)/'link';link.symlink_to(path)
            with self.assertRaises((OSError,ValueError)):jog.read_frame(link,2,lambda:True)
            with self.assertRaises(ValueError):jog.read_frame(path,2,lambda:False)
            for bad in (None,True,-2,1,2**32):
                with self.subTest(bad=bad),self.assertRaises(ValueError):jog.read_frame(path,bad,lambda:True)


if __name__=='__main__':unittest.main()
