#!/usr/bin/env python3
"""Read the existing shim's JOG1 raster; it has no native deck or Slip field."""
import os
from pathlib import Path
import stat
import struct
from sdk_emulator_controls import MissingEvidence


def read_frame(path,previous_sequence,verify_identity):
    """Read once after a known sequence boundary, with no torn/stale fallback.

    The caller must pin its owned VM/process/module before and after the read.
    Sequence advancement only establishes a new raster since that boundary;
    JOG1 supplies neither a capture clock nor a deck/indicator contract.
    """
    if type(previous_sequence) is not int or not 0<=previous_sequence<2**32 or previous_sequence&1:
        raise ValueError('one prior even JOG1 sequence required')
    if not callable(verify_identity) or verify_identity() is not True:
        raise ValueError('pinned jog publisher identity refused')
    path=Path(path)
    fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    try:
        before=os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size!=1<<20:
            raise ValueError('owned one MiB jog backing required')
        header=os.pread(fd,16,0)
        if len(header)!=16:raise MissingEvidence('JOG1 header incomplete')
        magic,sequence,width,height,fmt=struct.unpack('<IIHHI',header)
        if (magic,width,height,fmt)!=(0x31474f4a,320,240,1):
            raise MissingEvidence('native JOG1 publisher unavailable or incompatible')
        if sequence&1 or sequence<=previous_sequence:
            raise MissingEvidence('new stable JOG1 frame unavailable')
        pixels=os.pread(fd,320*240*4,4096)
        final=os.pread(fd,16,0)
        if len(pixels)!=320*240*4 or final!=header:
            raise MissingEvidence('JOG1 frame changed during read')
        if verify_identity() is not True:raise ValueError('pinned jog publisher identity changed')
        after=os.fstat(fd);current=path.lstat()
        if (after.st_dev,after.st_ino,after.st_size)!=(before.st_dev,before.st_ino,before.st_size) or \
           (current.st_dev,current.st_ino,current.st_size)!=(before.st_dev,before.st_ino,before.st_size):
            raise ValueError('owned jog backing changed during capture')
        return dict(sequence=sequence,width=width,height=height,format=fmt,
                    pixels=pixels,deck=None,slip_enabled=None)
    finally:os.close(fd)
