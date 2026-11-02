#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify OWN helper symbols and actual compiled probe constants without loading ELF."""
import hashlib
from pathlib import Path
import re
import struct

FAMILIES={
    'loop':('azstem.so','azstem_loop_observation_v2','azsdk_loop_read'),
    'source':('azmod.so','azmod_dm_key','azsdk_sources_candidate')}

def _sha(data):return hashlib.sha256(data).hexdigest()

def build_id(data):
    """Read bounded file-backed ELF notes; never load the selected library."""
    if len(data)<64 or data[:7]!=b'\x7fELF\x02\x01\x01':raise ValueError('ELF64 little-endian library required')
    kind,machine=struct.unpack_from('<HH',data,16)
    if kind!=3 or machine!=183:raise ValueError('AArch64 shared library required')
    offset=struct.unpack_from('<Q',data,32)[0]
    size,count=struct.unpack_from('<HH',data,54)
    if size!=56 or not 1<=count<=256 or offset+size*count>len(data):raise ValueError('invalid ELF program headers')
    identities=[]
    for i in range(count):
        p=struct.unpack_from('<IIQQQQQQ',data,offset+i*size)
        if p[0]!=4:continue
        start,length=p[2],p[5]
        if length>65536 or start+length>len(data):raise ValueError('invalid ELF note range')
        end=start+length
        while start<end:
            if end-start<12:raise ValueError('short ELF note')
            names,desc,typ=struct.unpack_from('<III',data,start);start+=12
            name_end=start+names;description=(name_end+3)&~3
            next_note=(description+desc+3)&~3
            if next_note>end:raise ValueError('ELF note outside file range')
            if typ==3 and data[start:name_end]==b'GNU\0':
                if not 4<=desc<=64:raise ValueError('invalid build ID size')
                identities.append(data[description:description+desc].hex())
            start=next_note
    if len(identities)!=1:raise ValueError('one GNU build ID required')
    return identities[0]

def _artifact(path):
    path=Path(path)
    if path.is_symlink() or not path.is_file() or not 64<=path.stat().st_size<=64*1024*1024:
        raise ValueError('invalid binding artifact '+path.name)
    return path.read_bytes()

def _string(data,offset):
    if not 0<=offset<len(data):raise ValueError('invalid ELF string offset')
    end=data.find(b'\0',offset,min(len(data),offset+4097))
    if end<0:raise ValueError('unterminated ELF string')
    try:return data[offset:end].decode('ascii')
    except UnicodeDecodeError:raise ValueError('invalid ELF symbol string') from None

class Elf:
    def __init__(self,data):
        self.data=data;self.identity=build_id(data)
        phoff=struct.unpack_from('<Q',data,32)[0]
        phsize,phcount=struct.unpack_from('<HH',data,54)
        self.programs=[struct.unpack_from('<IIQQQQQQ',data,phoff+i*phsize) for i in range(phcount)]
        shoff=struct.unpack_from('<Q',data,40)[0]
        shsize,shcount,strings=struct.unpack_from('<HHH',data,58)
        if shsize!=64 or not 1<=shcount<=8192 or not 0<strings<shcount or shoff+shsize*shcount>len(data):
            raise ValueError('invalid binding ELF section headers')
        self.sections=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*shsize) for i in range(shcount)]
        names=self.section_data(self.sections[strings],3,8*1024*1024)
        self.section_names=[_string(names,s[0]) for s in self.sections]

    def section_data(self,section,kind,limit):
        if section[1]!=kind or section[5]>limit or section[4]+section[5]>len(self.data):
            raise ValueError('invalid binding ELF section range')
        return self.data[section[4]:section[4]+section[5]]

    def function(self,name,table):
        matches=[]
        for section in self.sections:
            if section[1]!=table:continue
            if section[9]!=24 or section[5]%24 or not 0<section[6]<len(self.sections):
                raise ValueError('invalid binding ELF symbol table')
            symbols=self.section_data(section,table,24*65536)
            strings=self.section_data(self.sections[section[6]],3,8*1024*1024)
            for offset in range(0,len(symbols),24):
                key,info,other,index,rva,size=struct.unpack_from('<IBBHQQ',symbols,offset)
                if _string(strings,key)!=name:continue
                if info&15!=2 or not 0<index<len(self.sections) or rva%4 or not 4<=size<=1024*1024:
                    raise ValueError('invalid binding function '+name)
                if table==11 and info>>4 not in (1,2):raise ValueError('binding anchor is not exported '+name)
                matches.append({'name':name,'rva':rva,'size':size,'code_sha256':_sha(self.code(rva,size))})
        if len(matches)!=1:raise ValueError('one exact binding function required '+name)
        return matches[0]

    def code(self,rva,size):
        ranges=[]
        for p in self.programs:
            if p[0]!=1 or p[1]&7!=5:continue
            if p[2]+p[5]>len(self.data) or p[5]>p[6]:raise ValueError('invalid binding RX range')
            if p[3]<=rva and rva+size<=p[3]+p[5]:
                offset=p[2]+rva-p[3];ranges.append(self.data[offset:offset+size])
        if len(ranges)!=1:raise ValueError('binding function must be in one file-backed RX range')
        return ranges[0]

    def compiled_binding(self):
        rows=[s for name,s in zip(self.section_names,self.sections) if name=='.azsdk.binding']
        if len(rows)!=1:raise ValueError('one compiled probe binding section required')
        section=rows[0]
        if section[2]!=2 or section[5]!=281:raise ValueError('invalid compiled probe binding section')
        data=self.section_data(section,1,281)
        magic,size,version,delta,identity,anchor,target=struct.unpack('<8sIIq129s64s64s',data)
        if magic!=b'AZSDKPB1' or size!=281 or version!=1:raise ValueError('invalid compiled probe binding ABI')
        def fixed(raw):
            value=_string(raw,0)
            if raw[len(value)+1:]!=b'\0'*(len(raw)-len(value)-1):raise ValueError('invalid compiled binding padding')
            return value
        identity,anchor,target=map(fixed,(identity,anchor,target))
        if not re.fullmatch('[0-9a-f]{8,128}',identity) or len(identity)%2:raise ValueError('invalid compiled binding build ID')
        return {'build_id':identity,'delta':delta,'anchor':anchor,'target':target}

def provider_binding(package,family):
    """Derive a binding only from the full/stripped provider pair, never a number."""
    if family not in FAMILIES:raise ValueError('unknown probe binding family')
    library,anchor_name,target_name=FAMILIES[family];package=Path(package)
    full_data=_artifact(package/(library+'.full'));stripped_data=_artifact(package/library)
    full,stripped=map(Elf,(full_data,stripped_data))
    if full.identity!=stripped.identity:raise ValueError('full/stripped binding build ID mismatch '+family)
    anchor=full.function(anchor_name,2);target=full.function(target_name,2)
    if stripped.function(anchor_name,11)!=anchor:raise ValueError('full/stripped binding anchor mismatch '+family)
    if _sha(stripped.code(target['rva'],target['size']))!=target['code_sha256']:
        raise ValueError('full/stripped binding target mismatch '+family)
    delta=target['rva']-anchor['rva']
    return {'build_id':full.identity,'delta':delta,'anchor':anchor,'target':target,
            'full_sha256':_sha(full_data),'stripped_sha256':_sha(stripped_data)}

def binding_proof(package,family):
    """Check compiled probe constants against exact provider symbols and code."""
    proof=provider_binding(package,family)
    probe_data=_artifact(Path(package)/(family+'-probe.so'))
    expected={'build_id':proof['build_id'],'delta':proof['delta'],
              'anchor':proof['anchor']['name'],'target':proof['target']['name']}
    if Elf(probe_data).compiled_binding()!=expected:raise ValueError('compiled probe binding mismatch '+family)
    return dict(proof,probe_sha256=_sha(probe_data))
