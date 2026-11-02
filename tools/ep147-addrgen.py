#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the existing EP_ tables from reviewed mappings and exact stock ELFs.

This is an explicit-mapping validator, not an automatic firmware re-anchorer.
Mapping data does not certify a native ABI, ownership or hook readiness.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

VERSIONS = ('1.30', '2.00')
LIBRARIES = ('azmod', 'azstem')
NAME = re.compile(r'EP_([0-9a-f]+)\Z')
LITERALS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)

class MappingError(ValueError):
    pass

def number(value):
    if not isinstance(value, str) or not re.fullmatch(r'0x[0-9a-f]+', value):
        raise MappingError('address must be a lowercase hexadecimal string')
    result = int(value, 16)
    if not 0 < result <= 0xffffffff:
        raise MappingError('zero/out-of-range address')
    return result

def comment(value):
    if not isinstance(value, str) or not value or any(x in value for x in ('\n', '\r', '/*', '*/')):
        raise MappingError('invalid mapping basis/comment')
    return value

class Image:
    def __init__(self, path, expected):
        self.path = Path(path)
        if self.path.stat().st_size != expected['size']:
            raise MappingError('firmware identity/size mismatch: ' + str(self.path))
        self.data = self.path.read_bytes()
        if (hashlib.sha256(self.data).hexdigest() != expected['sha256'] or
                hashlib.md5(self.data).hexdigest() != expected['md5']):
            raise MappingError('firmware identity/hash mismatch: ' + str(self.path))
        b = self.data
        if len(b) < 64 or b[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', b, 18)[0] != 183:
            raise MappingError('expected a little-endian ELF64 AArch64 executable')
        phoff = struct.unpack_from('<Q', b, 32)[0]
        stride, count = struct.unpack_from('<HH', b, 54)
        if stride < 56 or not 0 < count <= 4096 or phoff + stride * count > len(b):
            raise MappingError('invalid ELF program header table')
        self.loads = []
        for index in range(count):
            kind, flags, offset, address, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', b, phoff + index * stride)
            if kind != 1:
                continue
            if filesz > memsz or offset + filesz > len(b) or address + memsz > 1 << 64:
                raise MappingError('invalid ELF load range')
            self.loads.append((address, offset, filesz, memsz, flags))
        if not self.loads:
            raise MappingError('ELF has no load segments')

    def mapped(self, address):
        # Legacy entries include image-end sentinels, not only dereferenceable objects.
        return any(base <= address <= base + memsz for base, _, _, memsz, _ in self.loads)

    def code(self, start, end):
        if start & 3 or end & 3 or not 16 <= end - start <= 1024 * 1024:
            raise MappingError('invalid executable function range')
        matches = [(offset + start - base, end - start) for base, offset, filesz, _, flags in self.loads
                   if flags & 1 and base <= start and end <= base + filesz]
        if len(matches) != 1:
            raise MappingError('function is not in one executable file-backed range')
        offset, size = matches[0]
        return self.data[offset:offset + size]

    def file_bytes(self,address,size):
        matches=[offset+address-base for base,offset,filesz,_,_ in self.loads
                 if base<=address and address+size<=base+filesz]
        if len(matches)!=1:raise MappingError('Slip metadata outside file-backed image')
        return self.data[matches[0]:matches[0]+size]

def layout(root):
    for name in ('src', 'guest'):
        path = Path(root) / name
        if all((path / lib).is_dir() for lib in LIBRARIES):
            return path
    raise MappingError('no src/ or guest/ library source directories')

def read_catalog(path):
    value = json.loads(Path(path).read_text())
    if value.get('schema') != 1 or set(value.get('builds', {})) != set(VERSIONS) or set(value.get('libraries', {})) != set(LIBRARIES):
        raise MappingError('unsupported mapping schema/build/library set')
    return value

def load_images(catalog, firmware):
    return {version: Image(Path(firmware) / ('EP147-' + version), catalog['builds'][version]) for version in VERSIONS}

def validate(catalog, images, source):
    runtime = (source / 'common/ep147build.h').read_text()
    for version in VERSIONS:
        known = catalog['builds'][version]
        row = r'\{"' + re.escape(known['md5']) + r'",\s*' + str(known['size']) + r'L,\s*EPB_V' + version.replace('.', '')
        if not re.search(row, runtime):
            raise MappingError('mapping identity differs from runtime build identification: ' + version)
    mappings = {}
    for lib in LIBRARIES:
        entries = {}
        for name, row in catalog['libraries'][lib].items():
            match = NAME.fullmatch(name)
            if not match:
                raise MappingError('invalid EP_ name: ' + name)
            first, second = int(match.group(1), 16), number(row['v200'])
            if not first or not images['1.30'].mapped(first) or not images['2.00'].mapped(second):
                raise MappingError('legacy mapping outside known image: ' + name)
            entries[name] = (first, second, comment(row['basis']))
        mappings[lib] = entries
    kind_values = {name: int(value) for name, value in re.findall(
        r'^#define (AZCE_KIND_[A-Z_]+) (\d+)$', (source / 'common/azcacheentry.h').read_text(), re.M)}
    cache_addresses = {version: set() for version in VERSIONS}
    selected_kinds = set()
    for name, row in catalog['cache_entries'].items():
        if not NAME.fullmatch(name) or set(row['profiles']) != set(VERSIONS):
            raise MappingError('incomplete cache mapping: ' + name)
        kind = kind_values.get(row.get('kind'), 0)
        if not 0 < kind < kind_values['AZCE_KIND_COUNT'] or kind in selected_kinds:
            raise MappingError('missing/duplicate cache original slot: ' + name)
        selected_kinds.add(kind)
        if row.get('hook') not in ('default', 'island'):
            raise MappingError('invalid cache hook mode: ' + name)
        addresses = []
        for version in VERSIONS:
            item = row['profiles'][version]
            start, end = number(item['entry']), number(item['end'])
            if start in cache_addresses[version]:
                raise MappingError('duplicate cache entry address: ' + name + ' ' + version)
            cache_addresses[version].add(start)
            if version == '1.30' and start != int(NAME.fullmatch(name).group(1), 16):
                raise MappingError('cache alias/address mismatch: ' + name)
            code = images[version].code(start, end)
            if (not re.fullmatch(r'[0-9a-f]{32}', item['first16']) or code[:16].hex() != item['first16'] or
                    not re.fullmatch(r'[0-9a-f]{64}', item['sha256']) or hashlib.sha256(code).hexdigest() != item['sha256']):
                raise MappingError('cache function signature mismatch: ' + name + ' ' + version)
            addresses.append(start)
        entry = (addresses[0], addresses[1], 'reviewed cache: ' + comment(row['role']) + '; exact function bytes')
        if name in mappings['azmod'] and mappings['azmod'][name][:2] != entry[:2]:
            raise MappingError('conflicting cache mapping: ' + name)
        mappings['azmod'][name] = entry
    if selected_kinds != set(range(1, kind_values['AZCE_KIND_COUNT'])):
        raise MappingError('incomplete cache original slot set')
    promotion = catalog.get('reply_promotion', {})
    if promotion.get('control_register') != 21:
        raise MappingError('promotion control register must be reviewed X21')
    if set(promotion.get('profiles', {})) != set(VERSIONS):
        raise MappingError('incomplete promotion profiles')
    for version in VERSIONS:
        row = promotion['profiles'][version]
        start, end, pc = number(row['entry']), number(row['end']), number(row['return_pc'])
        code = images[version].code(start, end)
        if len(code) > 65536 or not re.fullmatch(r'[0-9a-f]{64}', row['sha256']) or hashlib.sha256(code).hexdigest() != row['sha256']:
            raise MappingError('promotion function signature mismatch: ' + version)
        if pc & 3 or not start + 4 <= pc <= end or struct.unpack_from('<I', code, pc - start - 4)[0] != 0xd63f0060:
            raise MappingError('promotion return must follow reviewed BLR X3 call: ' + version)
    validate_slip(catalog,images,mappings)
    return mappings

def validate_slip(catalog,images,mappings):
    slip=catalog.get('slip_observer',{})
    roles=['source-constructor','parent-publication','source-disposal','parent-disposal']
    rows=slip.get('entries',[])
    if [row.get('role') for row in rows]!=roles or set(slip.get('profiles',{}))!=set(VERSIONS):
        raise MappingError('incomplete Slip lifecycle bindings')
    registration=slip.get('registration',{})
    if registration.get('role')!='weak-listener-append':raise MappingError('missing Slip weak append API')
    for n,row in enumerate(rows+[registration]):
        name=row.get('name','')
        if not NAME.fullmatch(name) or set(row.get('profiles',{}))!=set(VERSIONS):raise MappingError('invalid Slip address alias')
        if row.get('scope')!=('entry-prefix' if n==3 else 'function'):raise MappingError('invalid Slip signature scope')
        addresses=[]
        for version in VERSIONS:
            item=row['profiles'][version];start,end=number(item['entry']),number(item['end'])
            code=images[version].code(start,end)
            if n==3 and len(code)!=16:raise MappingError('Slip parent disposal binds only its entry prefix')
            if code[:16].hex()!=item['first16'] or hashlib.sha256(code).hexdigest()!=item['sha256']:
                raise MappingError('Slip signature mismatch: '+row['role']+' '+version)
            addresses.append(start)
        if int(NAME.fullmatch(name).group(1),16)!=addresses[0]:raise MappingError('Slip alias/address mismatch')
        existing=mappings['azmod'].get(name)
        if existing and existing[:2]!=tuple(addresses):raise MappingError('conflicting Slip mapping')
        mappings['azmod'][name]=(addresses[0],addresses[1],'reviewed Slip '+row['role']+'; '+row['scope']+' signature')
    for version in VERSIONS:
        image=images[version];p=slip['profiles'][version];owner=p['owner_scope']
        producer=p['producer'];ps,pe=number(producer['entry']),number(producer['end'])
        body=image.code(ps,pe)
        if len(body)>65536 or hashlib.sha256(body).hexdigest()!=producer['sha256'] or body[12:16]!=struct.pack('<I',0xaa0003f3):
            raise MappingError('Slip producer body signature/source-register mismatch')
        sites=p['getter_sites']
        if len(sites)!=3 or len(set(sites))!=3:raise MappingError('Slip getter call set mismatch')
        for site,load in zip(sites,(0xf9401021,0xf9403421,0xf9403821)):
            pc=number(site)
            if not ps<=pc<=pe-8 or image.file_bytes(pc,8)!=struct.pack('<II',load,0xd63f0020):
                raise MappingError('Slip getter load/call mismatch')
        begin,end=number(owner['entry']),number(owner['end']);body=image.code(begin,end)
        if body[:16].hex()!=owner['first16'] or hashlib.sha256(body).hexdigest()!=owner['sha256']:
            raise MappingError('Slip owner publication signature mismatch')
        def bl(pc,target):
            word=struct.unpack('<I',image.file_bytes(pc-4,4))[0]
            imm=word&0x3ffffff;imm=imm-(1<<26) if imm&(1<<25) else imm
            if word&0xfc000000!=0x94000000 or pc-4+imm*4!=target:raise MappingError('Slip owning call mismatch')
        pc=number(p['construct_return']);parent=rows[1]['profiles'][version]
        if not number(parent['entry'])<pc<number(parent['end']):raise MappingError('Slip constructor return outside parent')
        bl(pc,number(rows[0]['profiles'][version]['entry']))
        calls=p['parent_returns']
        if len(calls)!=4 or len(set(calls))!=4:raise MappingError('Slip four-deck caller set mismatch')
        for deck,address in enumerate(calls):
            pc=number(address)
            if not begin<pc<end:raise MappingError('Slip parent call outside owned aggregate')
            bl(pc,number(parent['entry']))
            words=struct.unpack('<8I',image.file_bytes(pc-36,32))
            if 0x52800005+(deck<<5) not in words:raise MappingError('Slip literal logical-deck argument mismatch')
        for key,name,slot in [('source_control_vtable','St23_Sp_counted_ptr_inplaceIN3gui9SlipStateESaIS1_ELN9__gnu_cxx12_Lock_policyE2EE',2),
                              ('parent_control_vtable','St23_Sp_counted_ptr_inplaceIN3gui15PlayerInfoStateESaIS1_ELN9__gnu_cxx12_Lock_policyE2EE',3)]:
            vt=number(p[key]);top,rtti=struct.unpack('<QQ',image.file_bytes(vt-16,16))
            text=struct.unpack('<Q',image.file_bytes(rtti+8,8))[0]
            actual=image.file_bytes(text,len(name)+2).split(b'\0')[0].lstrip(b'*').decode('ascii')
            dispose=struct.unpack('<Q',image.file_bytes(vt+16,8))[0]
            if top or actual!=name or dispose!=number(rows[slot]['profiles'][version]['entry']):
                raise MappingError('Slip genuine inplace control/type/disposal mismatch')

def reject_assembly_addresses(text, known, assembly_file=False):
    blocks = [text] if assembly_file else []
    masked = LITERALS.sub(lambda match: ' ' * len(match.group()), text)
    for match in re.finditer(r'\b(?:__asm__|asm)\s*(?:(?:__volatile__|volatile)\s*)?\(', masked):
        depth, end = 1, match.end()
        while end < len(masked) and depth:
            depth += (masked[end] == '(') - (masked[end] == ')')
            end += 1
        if depth:
            raise MappingError('unterminated inline assembly')
        blocks.append(text[match.end():end - 1])
    instruction = re.compile(r'\bmov([zk])\s+([xw]\d+)\s*,\s*#?(0x[0-9a-f]+|\d+)'
                             r'(?:\s*,\s*lsl\s*#?(0x[0-9a-f]+|\d+))?', re.I)
    for block in blocks:
        registers = {}
        for match in instruction.finditer(block):
            operation, register, immediate, shift = match.groups()
            value, shift = int(immediate, 0), int(shift or '0', 0)
            if value > 0xffff or shift not in (0, 16, 32, 48):
                continue
            if operation.lower() == 'z':
                registers[register] = value << shift
            elif register in registers:
                registers[register] = (registers[register] & ~(0xffff << shift)) | (value << shift)
            if registers.get(register) in known:
                raise MappingError('inline assembly constructs an EP147 address; pass an EP_ value from C')

def used_names(source, library, known_addresses=None):
    names = set()
    for path in sorted((source / library).rglob('*')):
        if path.name in ('ep147addr.h', 'sdk_cache_targets.h','sdk_slip_targets.h') or path.suffix not in ('.c', '.h', '.cpp', '.S'):
            continue
        text = path.read_text()
        if known_addresses:
            reject_assembly_addresses(text, known_addresses, path.suffix == '.S')
        if 'EP_' in text:
            names.update(re.findall(r'\bEP_[0-9a-f]+\b', LITERALS.sub(' ', text)))
    return names

def render(library, names, entries):
    ordered = sorted(names, key=lambda n: int(NAME.fullmatch(n).group(1), 16))
    if not ordered:
        raise MappingError('library has no address references: ' + library)
    missing = names - set(entries)
    if missing:
        raise MappingError('missing both-build mapping for: ' + ', '.join(sorted(missing)))
    text = '''/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated per firmware build. Do not edit by hand. %d EP147 addresses for %s.
 * tools/ep147-addrgen.py validates explicit mappings against the exact stock ELFs.
 * EP_ names use their 1.30 values; unknown builds remain inert. Mapping validation
 * establishes addresses and bytes, never a native ABI, ownership or hook readiness.
 */
#ifndef EP147ADDR_H
#define EP147ADDR_H
#include "../common/ep147build.h"

enum {
''' % (len(ordered), library)
    text += ''.join('    EPI_' + name[3:] + ',\n' for name in ordered) + '    EPI_N\n};\n'
    for index, version in enumerate(('130', '200')):
        text += 'static const uint32_t ep147_addr_v' + version + '[EPI_N] = {\n'
        for name in ordered:
            text += '    0x%07x,' % entries[name][index]
            if index == 1:
                text += '   /* ' + entries[name][2] + ' */'
            text += '\n'
        text += '};\n'
    text += '''static uintptr_t ep147_addr[EPI_N];

/* 1 = every address this library uses exists in `build`; 0 = stay inert. */
static int ep147_addr_init(int build){
    const uint32_t *t = build == EPB_V130 ? ep147_addr_v130 : build == EPB_V200 ? ep147_addr_v200 : 0;
    if(!t) return 0;
    for(int i = 0; i < EPI_N; i++){ if(!t[i]) return 0; }
    for(int i = 0; i < EPI_N; i++) ep147_addr[i] = t[i];
    return 1;
}

'''
    text += ''.join('#define ' + name + ' ((uintptr_t)ep147_addr[EPI_' + name[3:] + '])\n' for name in ordered)
    return text + '#endif\n'

def render_cache_targets(catalog):
    rows = sorted(catalog['cache_entries'].items(), key=lambda item: int(NAME.fullmatch(item[0]).group(1), 16))
    count = len(rows)
    text = '''/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated by tools/ep147-addrgen.py; do not edit. These checked target bytes
 * do not establish native signatures, object ownership or observer readiness. */
#ifndef SDK_CACHE_TARGETS_H
#define SDK_CACHE_TARGETS_H
#include "ep147addr.h"
#include "../common/azcacheentry.h"
#include <stddef.h>
#include <string.h>
struct sdk_cache_target {
    unsigned slot, island_only;
    uintptr_t address;
    unsigned char expected[16];
};
enum { SDK_CACHE_TARGET_COUNT = %d };
static int sdk_cache_target_profile(int build, struct sdk_cache_target *out, size_t capacity) {
    const uint32_t *table = build == EPB_V130 ? ep147_addr_v130 : build == EPB_V200 ? ep147_addr_v200 : 0;
    if (!out || capacity < SDK_CACHE_TARGET_COUNT || !table) return 0;
    /* A requested signature profile must match the already-initialized runtime
     * table. This function never reinitializes or changes address selection. */
    for (int i = 0; i < EPI_N; ++i) if (!ep147_addr[i] || ep147_addr[i] != table[i]) return 0;
    const uintptr_t addresses[SDK_CACHE_TARGET_COUNT] = {
''' % count
    text += ''.join('        ' + name + ',\n' for name, _ in rows) + '    };\n'
    text += '    static const unsigned slots[SDK_CACHE_TARGET_COUNT] = {\n'
    text += ''.join('        ' + row['kind'] + ',\n' for _, row in rows) + '    };\n'
    text += '    static const unsigned island[SDK_CACHE_TARGET_COUNT] = {\n'
    text += ''.join('        ' + str(int(row['hook'] == 'island')) + ',\n' for _, row in rows) + '    };\n'
    text += '    static const unsigned char signatures[2][SDK_CACHE_TARGET_COUNT][16] = {\n'
    for version in VERSIONS:
        text += '        {\n'
        for _, row in rows:
            text += '            {' + ','.join('0x%02x' % value for value in bytes.fromhex(row['profiles'][version]['first16'])) + '},\n'
        text += '        },\n'
    text += '''    };
    const unsigned profile = build == EPB_V130 ? 0 : 1;
    for (unsigned i = 0; i < SDK_CACHE_TARGET_COUNT; ++i) {
        out[i].slot = slots[i];
        out[i].island_only = island[i];
        out[i].address = addresses[i];
        memcpy(out[i].expected, signatures[profile][i], sizeof out[i].expected);
    }
    return SDK_CACHE_TARGET_COUNT;
}
#endif
'''
    return text

def render_reply_code(catalog):
    text = '''/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated by tools/ep147-addrgen.py; do not edit. RVAs compare the captured
 * native caller and full original promotion code; they are never callable
 * targets. Code identity alone supplies no object ownership or source identity. */
#ifndef AZCACHEREPLYCODE_H
#define AZCACHEREPLYCODE_H
#include <stdint.h>
#include <stddef.h>
struct azsdk_cache_reply_code {
    uintptr_t begin_rva, return_rva;
    size_t length;
    char sha256[65];
};
static int azsdk_cache_reply_code_profile(int build, struct azsdk_cache_reply_code *out) {
    static const struct azsdk_cache_reply_code profiles[2] = {
'''
    for version in VERSIONS:
        row = catalog['reply_promotion']['profiles'][version]
        start, end, pc = number(row['entry']), number(row['end']), number(row['return_pc'])
        text += '        {0x%x,0x%x,%d,"%s"},\n' % (start, pc, end - start, row['sha256'])
    return text + '''    };
    if (!out || (build != 1 && build != 2)) return 0;
    *out = profiles[build - 1];
    return 1;
}
#endif
'''

def render_slip_targets(catalog):
    slip=catalog['slip_observer'];rows=slip['entries']
    text='''/* SPDX-License-Identifier: GPL-3.0-only */
/* Generated by tools/ep147-addrgen.py; do not edit. Four lifecycle entry bridges
 * plus the native multi-listener append API. Parent-disposal binding covers
 * its entry prefix; other code ranges cover their complete reviewed FDEs.
 * Return PCs/vtables are comparison metadata, never arbitrary call targets. */
#ifndef SDK_SLIP_TARGETS_H
#define SDK_SLIP_TARGETS_H
#include "ep147addr.h"
#include "../common/azslipobserver.h"
static int sdk_slip_target_profile(int build,struct azsdk_slip_native_profile *out) {
    const uint32_t *table=build==EPB_V130?ep147_addr_v130:build==EPB_V200?ep147_addr_v200:0;
    if(!out || !table)return 0;
    for(int i=0;i<EPI_N;i++)if(!ep147_addr[i] || ep147_addr[i]!=table[i])return 0;
    static const struct azsdk_slip_native_profile profiles[2]={
'''
    for version in VERSIONS:
        p=slip['profiles'][version];owner=p['owner_scope'];registration=slip['registration']['profiles'][version]
        text+='        {.expected={\n'
        for row in rows:text+='            {'+','.join('0x%02x'%x for x in bytes.fromhex(row['profiles'][version]['first16']))+'},\n'
        text+='        },.code_bytes={'+','.join(str(number(row['profiles'][version]['end'])-number(row['profiles'][version]['entry'])) for row in rows)+'},\n'
        text+='        .code_sha256={'+','.join('"'+row['profiles'][version]['sha256']+'"' for row in rows)+'},\n'
        text+='        .owner_scope='+owner['entry']+',.owner_scope_bytes='+str(number(owner['end'])-number(owner['entry']))+',\n'
        text+='        .owner_scope_sha256="'+owner['sha256']+'",\n'
        text+='        .construct_return='+p['construct_return']+',.parent_returns={'+','.join(p['parent_returns'])+'},\n'
        text+='        .source_control_vtable='+p['source_control_vtable']+',.parent_control_vtable='+p['parent_control_vtable']+',\n'
        text+='        .registration='+registration['entry']+',.registration_bytes='+str(number(registration['end'])-number(registration['entry']))+',\n'
        text+='        .registration_sha256="'+registration['sha256']+'",\n'
        producer=p['producer']
        text+='        .producer='+producer['entry']+',.getter_sites={'+','.join(p['getter_sites'])+'},\n'
        text+='        .producer_bytes='+str(number(producer['end'])-number(producer['entry']))+',\n'
        text+='        .producer_sha256="'+producer['sha256']+'"},\n'
    text+='''    };
    *out=profiles[build==EPB_V130?0:1];
'''
    for i,row in enumerate(rows):text+='    out->targets[%d]=%s;\n'%(i,row['name'])
    text+='    out->registration='+slip['registration']['name']+';\n'
    return text+'''    return 1;
}
#endif
'''

def generate(root, catalog, images):
    source = layout(root)
    mappings = validate(catalog, images, source)
    known = {address for entries in mappings.values() for row in entries.values() for address in row[:2]}
    result = {}
    for lib in LIBRARIES:
        names = used_names(source, lib, known)
        # Reviewed cache bindings are declarative references for the private
        # dispatcher, even before their guarded native installation is enabled.
        if lib == 'azmod':
            names.update(catalog['cache_entries'])
            names.update(row['name'] for row in catalog['slip_observer']['entries'])
            names.add(catalog['slip_observer']['registration']['name'])
        result[source / lib / 'ep147addr.h'] = render(lib, names, mappings[lib])
    result[source / 'azmod/sdk_cache_targets.h'] = render_cache_targets(catalog)
    result[source / 'common/azcachereplycode.h'] = render_reply_code(catalog)
    result[source / 'azmod/sdk_slip_targets.h'] = render_slip_targets(catalog)
    return result

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--mapping', type=Path)
    parser.add_argument('--firmware-dir', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args(argv)
    try:
        catalog = read_catalog(args.mapping or args.root / 'tools/ep147-addresses.json')
        firmware = args.firmware_dir or args.root.parent / 'anchor/firmware'
        headers = generate(args.root, catalog, load_images(catalog, firmware))
        if args.check:
            stale = [path for path, text in headers.items() if not path.exists() or path.read_text() != text]
            for path in stale:
                print('stale generated header: ' + str(path), file=sys.stderr)
            return int(bool(stale))
        # Every image/mapping/reference is validated before any generated output
        # is replaced. Never leave a partially constructed individual header.
        for path, text in headers.items():
            temporary = path.with_name(path.name + '.new')
            temporary.write_text(text)
            temporary.replace(path)
        return 0
    except (MappingError, KeyError, OSError, ValueError, struct.error) as error:
        print('address generation refused: ' + str(error), file=sys.stderr)
        return 2

if __name__ == '__main__':
    sys.exit(main())
