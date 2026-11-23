#!/usr/bin/env python3
"""Extract the supported installed PVDI tensors and feature constants locally.

No TensorFlow dependency. The generated asset is excluded from version control.
"""
import argparse
import collections
import hashlib
from pathlib import Path
import struct


def fields(data):
    def varint(at):
        result = shift = 0
        while at < len(data) and shift < 70:
            byte = data[at]; at += 1
            result |= (byte & 127) << shift
            if byte < 128:
                return result, at
            shift += 7
        raise ValueError('Malformed protobuf varint')
    out = collections.defaultdict(list)
    at = 0
    while at < len(data):
        tag, at = varint(at)
        number, wire = tag >> 3, tag & 7
        if not number:
            raise ValueError('Invalid protobuf field')
        if wire == 0:
            value, at = varint(at)
        else:
            if wire == 2:
                size, at = varint(at)
            elif wire in (1, 5):
                size = 8 if wire == 1 else 4
            else:
                raise ValueError('Unsupported protobuf wire type')
            value = data[at:at + size]; at += size
            if len(value) != size:
                raise ValueError('Truncated protobuf')
        out[number].append(value)
    return out


def one(obj, field, default=None):
    return obj.get(field, [default])[0]


def extract(model, executable, output):
    raw = model.read_bytes()
    if hashlib.sha256(raw).hexdigest() != '4f1009aea543aaebd27a232d26de057780019e710a4c2075dd2e5ca9b01d4ef6':
        raise ValueError('Unsupported PVDI model: expected rekordbox 7.2.19')
    binary = executable.read_bytes()
    if hashlib.sha256(binary).hexdigest() != '2dfa9738b50e6c5ab3a6ca556dd7967c31ae4a6b06b52d0925f61fad8046f907':
        raise ValueError('Expected the matching thin arm64 executable')
    weights = {}
    for node in fields(raw)[1]:
        n = fields(node)
        if one(n, 2) != b'Const':
            continue
        name = one(n, 1).decode()
        attrs = {one(fields(a), 1): one(fields(a), 2) for a in n[5]}
        tensor = fields(one(fields(attrs[b'value']), 8))
        if one(tensor, 1) == 1 and 4 in tensor:
            weights[name] = one(tensor, 4)
    constants = list(struct.unpack_from('<240d', binary, 0x378ef70))
    grouped = struct.unpack_from('<56d', binary, 0x378f6f0)
    constants += [grouped[i + j] for i in range(0, 56, 4) for j in (0, 1)] + [1.7534915304167218]
    constants += [grouped[i + j] for i in range(0, 56, 4) for j in (2, 3)] + [-0.0024343021520163185]
    payload = bytearray(struct.pack('<298d', *constants))
    for direction in ('fw', 'bw'):
        for layer, width in enumerate((285, 512)):
            base = f'bidirectional_rnn/{direction}/multi_rnn_cell/cell_{layer}/basic_lstm_cell/'
            for suffix, count in (('kernel', width * 1024), ('bias', 1024)):
                value = weights[base + suffix]
                assert len(value) == count * 4
                payload.extend(value)
    for name, count in (('layer0/W', 512 * 64), ('layer0/Variable', 64), ('layer1/W', 128), ('layer1/Variable', 2)):
        value = weights['bilstm/fc_out/' + name]
        assert len(value) == count * 4
        payload.extend(value)
    assert len(payload) == 298 * 8 + 1669314 * 4
    checksum = 2166136261
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(struct.pack('<8s6I', b'AZPVDI01', len(payload), checksum, 0, 0, 0, 0) + payload)
    print(f'{output}: {output.stat().st_size} bytes, sha256={hashlib.sha256(output.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, help='Decoded raw GraphDef from the local PVDI extraction', default=Path('/Applications/rekordbox 7/rekordbox.app/Contents/Resources/vd_param_raw.pb'))
    parser.add_argument('--arm64-executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'assets/analysis/pvdi-model.bin')
    args = parser.parse_args()
    extract(args.model, args.arm64_executable, args.output)
