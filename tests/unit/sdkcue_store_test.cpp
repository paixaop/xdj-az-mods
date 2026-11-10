/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcuestore.h"
#include "../../src/common/azcuepersist.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace azcuestore;
namespace {
alignas(8) unsigned char manager[0x188], player_interface[8], service[8], player[0x200], source[0x70], pack[0xf0], publication[0x30], content[0x38];
NativeCue rows[2]; uintptr_t player_table[3], service_table[7], snapshot_table[2];
NativeProperty retained[9]; bool retained_valid=true;
alignas(8) unsigned char hot_publication[0x30], hot_content[0x38];
NativeCue hot_rows[2];
alignas(8) unsigned char delta_content[0x38], delta_hot_content[0x38];
NativeCue delta_rows[2], delta_hot_rows[2];
alignas(8) unsigned char where_from[0x20]; int where_refs, root_refs;
unsigned char identity[12]={1,2,3}; int sources,contents,players,cues,properties; bool bad_conversion;
int strings; std::string last_comment; uint32_t last_color;
void put(void *p,size_t off,uintptr_t value) { std::memcpy(static_cast<unsigned char *>(p)+off,&value,sizeof value); }
uint64_t get(const void *p,size_t off) { uint64_t value;std::memcpy(&value,static_cast<const unsigned char *>(p)+off,8);return value; }
NativePlayer capture(void *) { NativePlayer result; result.value=reinterpret_cast<uintptr_t>(player); ++players; return result; }
void destroy_player(void *) { --players; }
const void *position(const void *p) { return static_cast<const unsigned char *>(p)+0xd0; }
void copy_source(uintptr_t *out,const uintptr_t *in) { *out=*in; if (*out) ++sources; }
void copy_content(uintptr_t *out,const uintptr_t *in) { *out=*in; if (*out==reinterpret_cast<uintptr_t>(publication)||*out==reinterpret_cast<uintptr_t>(hot_publication)) ++root_refs;else if (*out) ++contents; }
void release(uintptr_t *p) { if (*p==reinterpret_cast<uintptr_t>(where_from)) --where_refs; else if (*p==reinterpret_cast<uintptr_t>(source)) --sources; else if (*p==reinterpret_cast<uintptr_t>(publication)||*p==reinterpret_cast<uintptr_t>(hot_publication)) --root_refs; else { assert(*p==reinterpret_cast<uintptr_t>(content) || *p==reinterpret_cast<uintptr_t>(hot_content) || *p==reinterpret_cast<uintptr_t>(delta_content) || *p==reinterpret_cast<uintptr_t>(delta_hot_content)); --contents; } *p=0; }
NativePlayer capture_where(const void *p) { assert(p==player);NativePlayer value;value.value=reinterpret_cast<uintptr_t>(where_from);++where_refs;return value; }
void *lookup(void *self,const void *key) { assert(self==service && get(key,0)==reinterpret_cast<uintptr_t>(where_from)); return pack; }
NativeCue get_cue(const uintptr_t *ref,size_t ordinal) { assert(ordinal<2); ++cues; if(*ref==reinterpret_cast<uintptr_t>(hot_content)) return hot_rows[ordinal];assert(*ref==reinterpret_cast<uintptr_t>(content));return rows[ordinal]; }
void destroy_cue(void *) { --cues; }
NativeProperty convert(const uintptr_t *ref,const void *p,const void *path,const void *row) {
    assert(*ref==reinterpret_cast<uintptr_t>(where_from) && p==pack && path==source+0x28);
    assert(get(reinterpret_cast<const void *>(*ref),8)==0x574845524546524fULL);
    assert(static_cast<const NativeCue *>(row)->bytes[0]<=8 || static_cast<const NativeCue *>(row)->bytes[0]==42 || static_cast<const NativeCue *>(row)->bytes[0]==43);
    last_comment = get(row,0x70) ? reinterpret_cast<const char *>(get(row,0x70)) : "";
    last_color = uint32_t(get(row,0x78));
    NativeProperty value={}; std::memcpy(value.bytes+0x30,identity,12);put(&value,0x58,reinterpret_cast<uintptr_t>(source));
    put(&value,0x40,get(row,0x10));put(&value,0x60,get(row,4)&0xffffffffu);put(&value,0x68,get(row,8));
    if (get(row,0x18)!=UINT64_C(0x7fffffffffffffff)) std::memcpy(value.bytes+0x88,identity,12);
    put(&value,0x98,get(row,0x20));put(&value,0xb8,get(row,4)&0xffffffffu);put(&value,0xc0,get(row,0x18));
    if (bad_conversion) {
        ++value.bytes[0x30];
    }
    ++properties;
    return value;
}
NativeCue serialize(unsigned kind,const void *property,bool hot) {
    assert(kind<=8 && hot==(kind!=0));NativeCue row={};row.bytes[0]=static_cast<unsigned char>(kind);
    const uint64_t rate=get(property,0x60)&0xffffffffu,in_usec=get(property,0x68),out_usec=get(property,0xc0);
    put(&row,4,rate);put(&row,8,in_usec);put(&row,0x10,uint64_t(double(rate*in_usec)*1e-6));
    put(&row,0x18,out_usec);put(&row,0x20,uint64_t(double(rate*out_usec)*1e-6));++cues;return row;
}
void destroy_property(void *) { --properties; }
bool has_cue(const void *p,int kind) { assert(p==player && kind>=0 && kind<=8);return retained_valid; }
const void *get_current_cue(const void *p,int kind) { assert(p==player && kind>=0 && kind<=8);return &retained[kind]; }
void copy_property(void *out,const void *in) { std::memcpy(out,in,sizeof(NativeProperty));++properties; }
NativeString allocate(uint64_t count) { NativeString text; text.data=std::malloc(count); if(text.data)++strings;return text; }
void destroy_string(void *p) { std::free(static_cast<NativeString *>(p)->data); --strings; }
void color(void *p,unsigned r,unsigned g,unsigned b) { uint32_t rgb=0xff000000u|(r<<16)|(g<<8)|b;std::memcpy(p,&rgb,4); }
Profile profile() { Profile p; p.copy_source=copy_source;p.copy_content=copy_content;p.release_ref=release;p.position=position;p.get_cue=get_cue;p.destroy_cue=destroy_cue;p.convert=convert;p.destroy_property=destroy_property;p.has_cue=has_cue;p.get_current_cue=get_current_cue;p.copy_property=copy_property;p.serialize_property=serialize;p.where_from=capture_where;p.string_allocate=allocate;p.string_destroy=destroy_string;p.color_construct=color;return p; }
void setup() {
    player_table[2]=reinterpret_cast<uintptr_t>(capture);service_table[6]=reinterpret_cast<uintptr_t>(lookup);snapshot_table[1]=reinterpret_cast<uintptr_t>(destroy_player);
    put(player_interface,0,reinterpret_cast<uintptr_t>(player_table));put(service,0,reinterpret_cast<uintptr_t>(service_table));put(player,0,reinterpret_cast<uintptr_t>(snapshot_table));
    put(player,0x18,uint32_t(44100));
    put(manager,0x158,reinterpret_cast<uintptr_t>(player_interface));put(manager,0x160,reinterpret_cast<uintptr_t>(service));
    std::memcpy(player+0xd8,identity,12);put(player,0x100,reinterpret_cast<uintptr_t>(source));put(source,0x58,reinterpret_cast<uintptr_t>(where_from));put(where_from,8,0x574845524546524fULL);
    std::memcpy(pack+0x80,player+0xd8,16);put(pack,0xe8,reinterpret_cast<uintptr_t>(publication));put(publication,0x28,reinterpret_cast<uintptr_t>(content));
    put(content,0x28,reinterpret_cast<uintptr_t>(rows));put(content,0x30,reinterpret_cast<uintptr_t>(rows+2));rows[0].bytes[0]=42;rows[1].bytes[0]=43;
    for (auto &cue:retained) std::memcpy(cue.bytes+0x30,identity,12);
}
void clean() { assert(!strings); assert(!sources&&!where_refs&&!root_refs&&!contents&&!players&&!cues&&!properties); }
}
int main() {
    azcue_native_timestamps a,b;
    a.sample_rate=96000;b.sample_rate=48000;
    a.in_us=b.in_us=1000000;a.in_sample=96001;b.in_sample=48000;
    a.out_us=b.out_us=a.out_sample=b.out_sample=UINT64_C(0x7fffffffffffffff);
    assert(azcue_timestamps_equal(a,b,48000));assert(azcue_timestamps_equal(b,a,48000));
    a.out_sample=UINT64_C(0xfffffffffffffff0);b.out_sample=0;
    assert(azcue_timestamps_equal(a,b,48000));assert(azcue_timestamps_equal(b,a,48000));
    b.out_us=0;assert(!azcue_timestamps_equal(a,b,48000));
    a.out_us=b.out_us=1000000;a.out_sample=96000;b.out_sample=48002;
    assert(!azcue_timestamps_equal(a,b,48000));
    a.out_us=b.out_us=a.out_sample=b.out_sample=UINT64_C(0x7fffffffffffffff);
    a.in_sample=96002;
    assert(!azcue_timestamps_equal(a,b,48000));assert(!azcue_timestamps_equal(b,a,48000));
    a.in_sample=96000;b.sample_rate=192000;b.in_sample=192001;
    assert(azcue_timestamps_equal(a,b,48000));assert(azcue_timestamps_equal(b,a,48000));
    b.in_sample=192004;assert(!azcue_timestamps_equal(a,b,48000));
    b.in_sample=192000;b.in_us+=5;assert(azcue_timestamps_equal(a,b,192000));
    b.in_us+=1;assert(!azcue_timestamps_equal(a,b,192000));
    assert(!azcue_timestamps_equal(a,b,0));
    b.in_us=a.in_us;b.sample_rate=0;assert(!azcue_timestamps_equal(a,b,48000));
    a.in_sample=b.in_sample=UINT64_C(0x7fffffffffffffff);
    assert(azcue_timestamps_equal(a,b,48000)); // Known cached clock, absent sample index.
    setup(); Profile p=profile(); RevisionState state; Snapshot snapshot;
    assert(snapshot.open(p,manager,1,identity,state));assert(snapshot.count()==2 && snapshot.revision()==1 && snapshot.track_id()[0]==1);
    Property property; assert(!snapshot.property(0,1,property));assert(!snapshot.property(1,2,property));assert(snapshot.property(2,1,property));assert(property.valid());assert(get(&property.native,0x58)==reinterpret_cast<uintptr_t>(source));
    Property copied;assert(property.copy_to(copied));copied.reset();
    put(&retained[0],0x40,150025);put(&retained[0],0x60,44100);put(&retained[0],0x68,3401927);
    put(&retained[0],0x98,450025);put(&retained[0],0xb8,44100);put(&retained[0],0xc0,10204648);
    Property original,canonical,again;assert(snapshot.current_property(0,original));
    assert(snapshot.canonical_property(0,original,canonical));assert(get(&canonical.native,0x58)==reinterpret_cast<uintptr_t>(source));assert(get(&original.native,0x40)==150025 && get(&canonical.native,0x40)==150024);
    assert(get(&original.native,0x98)==450025 && get(&canonical.native,0x98)==450024);
    assert(snapshot.canonical_property(8,canonical,again));assert(get(&again.native,0x40)==150024 && get(&again.native,0x98)==450024);
    assert(!snapshot.canonical_property(9,original,again));original.reset();canonical.reset();again.reset();
    assert(snapshot.current_property(8,copied));assert(copied.valid());copied.reset();
    assert(!snapshot.current_property(9,copied));retained_valid=false;assert(!snapshot.current_property(0,copied));retained_valid=true;
    ++retained[0].bytes[0x30];assert(!snapshot.current_property(0,copied));--retained[0].bytes[0x30];
    snapshot.reset();assert(property.valid());property.reset();clean();
    assert(snapshot.open(p,manager,1,identity,state) && snapshot.revision()==1);snapshot.reset();
    rows[1].bytes[3]=1;assert(snapshot.open(p,manager,1,identity,state) && snapshot.revision()==2);
    bad_conversion=true;assert(!snapshot.property(1,2,property));bad_conversion=false;
    pack[0x80]=10;OwnedCue row;assert(!snapshot.row(0,row));snapshot.reset();clean();pack[0x80]=1;
    pack[0x8c]=1;assert(snapshot.open(p,manager,1,identity,state));assert(snapshot.revision()==2);snapshot.reset();clean();pack[0x8c]=0;
    player[0xe4]=0xa5;assert(snapshot.open(p,manager,1,identity,state));assert(snapshot.revision()==2);
    pack[0x8d]=0x5a;assert(snapshot.row(0,row));row.reset();assert(snapshot.current_property(0,copied));copied.reset();snapshot.reset();clean();
    player[0xe4]=0;pack[0x8d]=0;
    pack[0x88]=1;assert(!snapshot.open(p,manager,1,identity,state));clean();pack[0x88]=0;
    unsigned char wrong[12]={9};assert(!snapshot.open(p,manager,1,wrong,state));clean();
    assert(snapshot.open(p,manager,1,nullptr,state));assert(!std::memcmp(snapshot.source_id(),identity,12));snapshot.reset();clean();
    put(content,0x30,reinterpret_cast<uintptr_t>(rows)+1);assert(!snapshot.open(p,manager,1,identity,state));clean();
    put(content,0x30,reinterpret_cast<uintptr_t>(rows+2));put(source,0x58,0);assert(!snapshot.open(p,manager,1,identity,state));clean();put(source,0x58,reinterpret_cast<uintptr_t>(where_from));put(where_from,8,0x574845524546524fULL);
    put(source,0x58,123);assert(!snapshot.open(p,manager,1,identity,state));clean();put(source,0x58,reinterpret_cast<uintptr_t>(where_from));
    assert(snapshot.open(p,manager,2,identity,state)&&snapshot.revision()==3);snapshot.reset();clean();
    RevisionState combined;
    put(pack,0xe0,reinterpret_cast<uintptr_t>(hot_publication));put(hot_publication,0x28,reinterpret_cast<uintptr_t>(hot_content));
    put(hot_content,0x28,reinterpret_cast<uintptr_t>(hot_rows));put(hot_content,0x30,reinterpret_cast<uintptr_t>(hot_rows+2));hot_rows[0].bytes[0]=43;hot_rows[1].bytes[0]=42;
    assert(snapshot.open(p,manager,2,nullptr,combined));assert(snapshot.count()==4 && snapshot.memory_count()==2 && snapshot.revision()==1);
    assert(snapshot.row(2,row) && row.native.bytes[0]==43);row.reset();assert(snapshot.property(3,1,property));property.reset();snapshot.reset();clean();
    hot_rows[1].bytes[4]=1;assert(snapshot.open(p,manager,2,nullptr,combined)&&snapshot.revision()==2);snapshot.reset();clean();
    rows[0].bytes[4]=1;assert(snapshot.open(p,manager,2,nullptr,combined)&&snapshot.revision()==3);snapshot.reset();clean();
    put(pack,0xe8,0);assert(snapshot.open(p,manager,2,nullptr,combined));assert(snapshot.count()==2 && snapshot.memory_count()==0);snapshot.reset();clean();
    put(pack,0xe0,0);put(pack,0xe8,reinterpret_cast<uintptr_t>(publication));assert(snapshot.open(p,manager,2,nullptr,combined));assert(snapshot.count()==2 && snapshot.memory_count()==2);snapshot.reset();clean();
    put(pack,0xe8,0);assert(snapshot.open(p,manager,2,nullptr,combined));assert(snapshot.count()==0);snapshot.reset();clean();
    put(pack,0xe0,reinterpret_cast<uintptr_t>(hot_publication));pack[0x8c]=1;assert(snapshot.open(p,manager,2,nullptr,combined));snapshot.reset();clean();pack[0x8c]=0;
    pack[0x88]=1;assert(!snapshot.open(p,manager,2,nullptr,combined));snapshot.reset();clean();pack[0x88]=0;
    put(pack,0xe8,reinterpret_cast<uintptr_t>(publication));
    put(pack,0xe0,0);RevisionState partial_state;assert(snapshot.open(p,manager,2,nullptr,partial_state));azcuepersist_witness absent_root={};assert(!snapshot.content_witness(snapshot.revision(),&absent_root));snapshot.reset();clean();put(pack,0xe0,reinterpret_cast<uintptr_t>(hot_publication));
    RevisionState witness_state;assert(snapshot.open(p,manager,2,nullptr,witness_state));
    azcuepersist_witness witness={};
    assert(!snapshot.content_witness(snapshot.revision()+1,&witness));assert(!witness.memory_content&&!witness.fingerprint);
    assert(snapshot.content_witness(snapshot.revision(),&witness));assert(witness.memory_content==reinterpret_cast<uintptr_t>(content)&&witness.hot_content==reinterpret_cast<uintptr_t>(hot_content)&&witness.fingerprint);
    assert(witness.memory_set==reinterpret_cast<uintptr_t>(publication)&&witness.hot_set==reinterpret_cast<uintptr_t>(hot_publication));
    assert(witness.source_info==reinterpret_cast<uintptr_t>(source)&&!std::memcmp(witness.source_id,identity,12));
    rows[0].bytes[7]^=1;assert(!snapshot.content_witness(snapshot.revision(),&witness));rows[0].bytes[7]^=1;
    hot_rows[0].bytes[7]^=1;assert(!snapshot.content_witness(snapshot.revision(),&witness));hot_rows[0].bytes[7]^=1;
    put(publication,0x28,reinterpret_cast<uintptr_t>(hot_content));assert(!snapshot.content_witness(snapshot.revision(),&witness));put(publication,0x28,reinterpret_cast<uintptr_t>(content));
    put(pack,0xe0,0);assert(!snapshot.content_witness(snapshot.revision(),&witness));put(pack,0xe0,reinterpret_cast<uintptr_t>(hot_publication));
    assert(snapshot.content_witness(snapshot.revision(),&witness));
    uintptr_t retained_refs[5]={};p.copy_source(&retained_refs[0],&witness.source_info);
    const uintptr_t borrowed_refs[4]={witness.memory_set,witness.hot_set,witness.memory_content,witness.hot_content};
    for(unsigned i=0;i<4;++i)p.copy_content(&retained_refs[i+1],&borrowed_refs[i]);
    snapshot.reset();assert(contents==2&&root_refs==2&&sources==1);
    for (auto &ref:retained_refs) { p.release_ref(&ref); }
    clean();
    static unsigned char mpeg_before[8]={1,2,3,4,5,6,7,8},mpeg_after[8];
    static unsigned char flac_before[24],flac_after[24];for(unsigned i=0;i<24;++i) { flac_before[i]=static_cast<unsigned char>(i+1);flac_after[i]=static_cast<unsigned char>(i+1); }
    std::memcpy(mpeg_after,mpeg_before,8);flac_after[23]=99;
    static char long_before[201],long_after[201];std::memset(long_before,'a',200);std::memset(long_after,'a',200);
    rows[0].bytes[0]=rows[1].bytes[0]=0;hot_rows[0].bytes[0]=1;hot_rows[1].bytes[0]=2;
    put(&hot_rows[0],0x78,0xff000000u);put(&hot_rows[1],0x78,0xff000000u);
    for (auto &row:rows) put(&row,4,44100);
    for (auto &row:hot_rows) put(&row,4,44100);
    rows[0].bytes[0x28]=0;put(&rows[0],0x70,reinterpret_cast<uintptr_t>(long_before));
    put(&rows[0],0x30,2);put(&rows[0],0x38,reinterpret_cast<uintptr_t>(mpeg_before));put(&rows[0],0x40,123);put(&rows[0],0x48,1);
    put(&rows[0],0x50,4);put(&rows[0],0x58,reinterpret_cast<uintptr_t>(flac_before));put(&rows[0],0x60,456);put(&rows[0],0x68,1);
    put(&hot_rows[0],0x30,2);put(&hot_rows[0],0x38,reinterpret_cast<uintptr_t>(mpeg_before));put(&hot_rows[0],0x40,123);put(&hot_rows[0],0x48,1);

    put(publication,0x28,reinterpret_cast<uintptr_t>(content));put(hot_publication,0x28,reinterpret_cast<uintptr_t>(hot_content));
    RevisionState delta_state;assert(snapshot.open(p,manager,2,nullptr,delta_state));azcuepersist_witness before_delta={};assert(snapshot.content_witness(snapshot.revision(),&before_delta));
    uintptr_t delta_refs[5]={};p.copy_source(&delta_refs[0],&before_delta.source_info);
    const uintptr_t delta_borrowed[4]={before_delta.memory_set,before_delta.hot_set,before_delta.memory_content,before_delta.hot_content};
    for(unsigned i=0;i<4;++i) { p.copy_content(&delta_refs[i+1],&delta_borrowed[i]); }
    snapshot.reset();std::memcpy(delta_rows,rows,sizeof rows);std::memcpy(delta_hot_rows,hot_rows,sizeof hot_rows);
    delta_rows[0].bytes[0x28]=1;put(&delta_rows[0],0x70,reinterpret_cast<uintptr_t>(long_after));
    put(&delta_rows[0],0x38,reinterpret_cast<uintptr_t>(mpeg_after));put(&delta_rows[0],0x40,987);
    put(&delta_rows[0],0x58,reinterpret_cast<uintptr_t>(flac_after));put(&delta_rows[0],0x60,654);
    put(&delta_hot_rows[0],0x38,reinterpret_cast<uintptr_t>(mpeg_after));put(&delta_hot_rows[0],0x40,987);

    for(auto &r:delta_rows) { r.bytes[1]=31;r.bytes[0x29]=12;r.bytes[0x34]=99;r.bytes[0x7f]=17; }
    put(delta_content,0x28,reinterpret_cast<uintptr_t>(delta_rows));put(delta_content,0x30,reinterpret_cast<uintptr_t>(delta_rows+2));
    put(delta_hot_content,0x28,reinterpret_cast<uintptr_t>(delta_hot_rows));put(delta_hot_content,0x30,reinterpret_cast<uintptr_t>(delta_hot_rows+2));
    put(publication,0x28,reinterpret_cast<uintptr_t>(delta_content));put(hot_publication,0x28,reinterpret_cast<uintptr_t>(delta_hot_content));
    auto check_delta=[&](bool expected) { assert(snapshot.open(p,manager,2,nullptr,delta_state));assert(snapshot.auto_only_changed(before_delta,1,true,44100)==expected);snapshot.reset(); };
    check_delta(true);long_after[180]='b';check_delta(false);long_after[180]='a';
    uint64_t cached_before=get(&delta_rows[0],8);
    put(&delta_rows[0],8,cached_before+22);check_delta(true);
    put(&delta_rows[0],8,cached_before+23);check_delta(false);
    put(&delta_rows[0],8,cached_before);check_delta(true);
    delta_rows[1].bytes[0x28]^=1;check_delta(false);delta_rows[1].bytes[0x28]^=1;
    delta_hot_rows[0].bytes[0x78]^=1;check_delta(false);delta_hot_rows[0].bytes[0x78]^=1;
    delta_rows[0].bytes[0x10]^=1;check_delta(false);delta_rows[0].bytes[0x10]^=1;
    delta_rows[0].bytes[0x30]=3;check_delta(false);delta_rows[0].bytes[0x30]=2;
    delta_rows[0].bytes[0x48]=2;check_delta(false);delta_rows[0].bytes[0x48]=1;
    mpeg_after[7]^=1;check_delta(false);mpeg_after[7]^=1;
    flac_after[19]^=1;check_delta(false);flac_after[19]^=1;
    flac_after[23]^=1;check_delta(true);flac_after[23]^=1;
    delta_rows[0].bytes[0x7c]^=1;check_delta(false);delta_rows[0].bytes[0x7c]^=1;
    assert(snapshot.open(p,manager,2,nullptr,delta_state));assert(!snapshot.auto_only_changed(before_delta,1,false,44100));assert(!snapshot.auto_only_changed(before_delta,3,true,44100));assert(!snapshot.auto_only_changed(before_delta,1,true,0));snapshot.reset();
    // Metadata confirmation checks every typed stored value, including full
    // comments and seek variants, without converting unrelated properties.
    delta_rows[0].bytes[0x28]=0;
    const char *edited="metadata caf\xc3\xa9";
    auto check_metadata=[&](unsigned kind,unsigned entry,const char *text,unsigned rgb,bool expected) {
        assert(snapshot.open(p,manager,2,nullptr,delta_state));
        assert(snapshot.metadata_only_changed(before_delta,kind,entry,text,rgb,44100)==expected);
        snapshot.reset();
    };
    put(&delta_rows[1],0x70,reinterpret_cast<uintptr_t>(edited));
    check_metadata(0,2,edited,0,true);
    long_after[180]='b';check_metadata(0,2,edited,0,false);long_after[180]='a';
    delta_rows[0].bytes[0x28]^=1;check_metadata(0,2,edited,0,false);delta_rows[0].bytes[0x28]^=1;
    delta_hot_rows[0].bytes[0x78]^=1;check_metadata(0,2,edited,0,false);delta_hot_rows[0].bytes[0x78]^=1;
    delta_rows[1].bytes[0x2a]^=1;check_metadata(0,2,edited,0,false);delta_rows[1].bytes[0x2a]^=1;
    put(&delta_rows[1],8,22);check_metadata(0,2,edited,0,true);
    put(&delta_rows[1],8,23);check_metadata(0,2,edited,0,false);put(&delta_rows[1],8,0);
    mpeg_after[7]^=1;check_metadata(0,2,edited,0,false);mpeg_after[7]^=1;
    delta_rows[0].bytes[0x30]=3;check_metadata(0,2,edited,0,false);delta_rows[0].bytes[0x30]=2;
    check_metadata(0,3,edited,0,false);check_metadata(8,8,edited,0,false);
    put(&delta_rows[1],0x70,0);
    put(&delta_hot_rows[0],0x70,reinterpret_cast<uintptr_t>(edited));check_metadata(1,1,edited,0,true);
    check_metadata(1,2,edited,0,false);
    put(&delta_hot_rows[0],0x70,0);
    put(&delta_hot_rows[0],0x78,0xff123456u);check_metadata(1,1,nullptr,0x123456,true);
    check_metadata(1,1,nullptr,0x123457,false);
    delta_hot_rows[0].bytes[0x7c]^=1;check_metadata(1,1,nullptr,0x123456,false);delta_hot_rows[0].bytes[0x7c]^=1;
    put(&delta_hot_rows[0],0x78,0xff000000u);
    // Actual Memory readback publishes the requested opaque RGB and its native
    // palette index together. Uncolored black can use the zero color word.
    put(&delta_rows[1],0x78,0xff00c3ffu);delta_rows[1].bytes[0x7c]=6;check_metadata(0,2,nullptr,0x00c3ff,true);
    delta_rows[0].bytes[0x7c]^=1;check_metadata(0,2,nullptr,0x00c3ff,false);delta_rows[0].bytes[0x7c]^=1;
    delta_rows[1].bytes[0x7c]=5;check_metadata(0,2,nullptr,0x00c3ff,false);delta_rows[1].bytes[0x7c]=6;
    put(&delta_rows[1],0x78,0x00c3ffu);check_metadata(0,2,nullptr,0x00c3ff,false);
    put(&delta_rows[1],0x78,0x7f00c3ffu);check_metadata(0,2,nullptr,0x00c3ff,false);
    put(&delta_rows[1],0x78,0u);delta_rows[1].bytes[0x7c]=0;check_metadata(0,2,nullptr,0,true);
    put(&delta_rows[1],0x78,0xff000000u);check_metadata(0,2,nullptr,0,true);
    delta_rows[1].bytes[0x7c]=6;check_metadata(0,2,nullptr,0,false);
    put(&delta_rows[1],0x78,0xff00c3ffu);
    long_after[180]='b';check_metadata(0,2,nullptr,0x00c3ff,false);long_after[180]='a';
    for(auto &ref:delta_refs) { p.release_ref(&ref); }clean();
    // Production construction: zero is present, source rate comes from the
    // owned PlayerState, seek variants start empty, and native conversion owns refs.
    put(pack,0xe8,reinterpret_cast<uintptr_t>(publication));put(pack,0xe0,reinterpret_cast<uintptr_t>(hot_publication));
    put(publication,0x28,reinterpret_cast<uintptr_t>(content));put(hot_publication,0x28,reinterpret_cast<uintptr_t>(hot_content));
    put(content,0x28,reinterpret_cast<uintptr_t>(rows));put(content,0x30,reinterpret_cast<uintptr_t>(rows));
    put(hot_content,0x28,reinterpret_cast<uintptr_t>(hot_rows));put(hot_content,0x30,reinterpret_cast<uintptr_t>(hot_rows));
    RevisionState timed_state;
    const uint32_t rates[] = {44100,48000,96000,192000};
    put(pack,0xbe,uint32_t(16777216)); // Native musical key data must never supply Hertz.
    for (uint32_t rate : rates) {
        std::memcpy(player+0x18,&rate,4);
        assert(snapshot.open(p,manager,4,nullptr,timed_state));
        assert(snapshot.source_rate()==rate);
        Property timed;
        assert(snapshot.timed_property(0,0.,0.,false,0xff0000,"Zero caf\xc3\xa9",timed));
        assert(compare_timestamps(get(&timed.native,0x40),0,rate,rate,1)==AZ_TIME_EQUAL);
        assert(last_comment=="Zero caf\xc3\xa9" && last_color==0xffff0000 && !strings);
        assert(snapshot.timed_property(8,1.234567,9.876543,true,0x123456,"Loop",timed));
        assert(compare_timestamps(double(get(&timed.native,0x40))/rate,1.234567,1,rate,1)==AZ_TIME_EQUAL);
        assert(compare_timestamps(double(get(&timed.native,0x98))/rate,9.876543,1,rate,1)==AZ_TIME_EQUAL);
        assert(last_color==0xff123456 && last_comment=="Loop");
        const double frames[] = {0.005,0.49,0.5,0.995,100.005,100.49,100.5,100.995,12345678.005,12345678.49,12345678.51,12345678.995};
        const uint64_t nearest[] = {0,0,1,1,100,100,101,101,12345678,12345678,12345679,12345679};
        for(unsigned i=0;i<sizeof frames/sizeof *frames;++i) {
            const double start_s=frames[i]/rate,end_s=(frames[i]+300)/rate;
            for(bool loop:{false,true}) {
                assert(snapshot.timed_property(8,start_s,end_s,loop,0x123456,"Fractional frame",timed));
                const int selected=compare_timestamps(get(&timed.native,0x40),nearest[i],rate,rate,1);
                assert(selected!=AZ_TIME_INVALID && selected==AZ_TIME_EQUAL);
                Property canonical;
                assert(snapshot.canonical_property(8,timed,canonical));
                const int roundtrip=compare_timestamps(get(&canonical.native,0x40),nearest[i],rate,rate,1);
                assert(roundtrip!=AZ_TIME_INVALID && roundtrip==AZ_TIME_EQUAL);
                if(loop) {
                    const int selected_end=compare_timestamps(get(&timed.native,0x98),nearest[i]+300,rate,rate,1);
                    const int roundtrip_end=compare_timestamps(get(&canonical.native,0x98),nearest[i]+300,rate,rate,1);
                    assert(selected_end!=AZ_TIME_INVALID && selected_end==AZ_TIME_EQUAL);
                    assert(roundtrip_end!=AZ_TIME_INVALID && roundtrip_end==AZ_TIME_EQUAL);
                }
            }
        }
        assert(!snapshot.timed_property(8,9007199254740991./rate,0.,false,0,"",timed));
        assert(!snapshot.timed_property(8,9007199254740991./1000000,0.,false,0,"",timed));
        assert(!snapshot.timed_property(0,1.,2.,true,0x123456,"Invalid memory RGB",timed));
        assert(!snapshot.timed_property(0,-1.,0.,false,0,"",timed));
        assert(!snapshot.timed_property(0,1.,1.,true,0,"",timed));
        bad_conversion=true;assert(!snapshot.timed_property(0,1.,0.,false,0,"",timed));bad_conversion=false;
        snapshot.reset();timed.reset();clean();
    }
    uint32_t unknown=0;std::memcpy(player+0x18,&unknown,4);
    assert(snapshot.open(p,manager,4,nullptr,timed_state));
    Property missing;assert(!snapshot.timed_property(1,0.,0.,false,0,"",missing));snapshot.reset();clean();
    // Production baseline comparison admits exactly one addition and no
    // changes to the other cue collection, independent of row ordering.
    uint32_t rate=48000;std::memcpy(player+0x18,&rate,4);
    put(pack,0xe8,reinterpret_cast<uintptr_t>(publication));put(pack,0xe0,reinterpret_cast<uintptr_t>(hot_publication));
    put(content,0x28,reinterpret_cast<uintptr_t>(rows));put(content,0x30,reinterpret_cast<uintptr_t>(rows+1));
    put(hot_content,0x28,reinterpret_cast<uintptr_t>(hot_rows));put(hot_content,0x30,reinterpret_cast<uintptr_t>(hot_rows));
    rows[0]={};rows[0].bytes[0]=0;put(&rows[0],4,rate);put(&rows[0],8,1000000);put(&rows[0],16,48000);
    put(&rows[0],24,UINT64_C(0x7fffffffffffffff));put(&rows[0],32,UINT64_C(0x7fffffffffffffff));
    RevisionState creation_state;assert(snapshot.open(p,manager,6,nullptr,creation_state));
    azcuepersist_witness before_create={};assert(snapshot.content_witness(snapshot.revision(),&before_create));
    Property wanted;assert(snapshot.timed_property(0,2.,3.,true,0,"",wanted));
    delta_rows[0]=rows[0];delta_rows[1]=serialize(0,&wanted.native,false);destroy_cue(nullptr);
    snapshot.reset();
    put(delta_content,0x28,reinterpret_cast<uintptr_t>(delta_rows));put(delta_content,0x30,reinterpret_cast<uintptr_t>(delta_rows+2));
    put(publication,0x28,reinterpret_cast<uintptr_t>(delta_content));
    assert(snapshot.open(p,manager,6,nullptr,creation_state));assert(snapshot.creation_only_changed(before_create,0,wanted,rate));snapshot.reset();
    delta_rows[0].bytes[0x78]^=1;
    assert(snapshot.open(p,manager,6,nullptr,creation_state));assert(!snapshot.creation_only_changed(before_create,0,wanted,rate));snapshot.reset();
    delta_rows[0].bytes[0x78]^=1;
    NativeCue swap=delta_rows[0];delta_rows[0]=delta_rows[1];delta_rows[1]=swap;
    assert(snapshot.open(p,manager,6,nullptr,creation_state));assert(snapshot.creation_only_changed(before_create,0,wanted,rate));snapshot.reset();
    wanted.reset();clean();
    // A track can have no publication roots yet. Creation records explicit
    // absence and later proves the first native root contains only its cue.
    put(pack,0xe8,0);put(pack,0xe0,0);
    assert(snapshot.open(p,manager,7,nullptr,creation_state));
    assert(!snapshot.content_witness(snapshot.revision(),&before_create));
    assert(snapshot.content_witness(snapshot.revision(),&before_create,true));
    assert(!before_create.memory_set&&!before_create.hot_set);
    const char label[]="Native label \342\234\223";const uint32_t rgb=0x00c3ff;
    assert(snapshot.timed_property(0,0.,0.,false,rgb,label,wanted));
    rows[0]=serialize(0,&wanted.native,false);destroy_cue(nullptr);snapshot.reset();
    put(&rows[0],32,UINT64_C(0xfffffffffffffff0)); // Native point's unused OUT sample cache.
    put(publication,0x28,reinterpret_cast<uintptr_t>(content));put(content,0x30,reinterpret_cast<uintptr_t>(rows+1));put(pack,0xe8,reinterpret_cast<uintptr_t>(publication));
    assert(snapshot.open(p,manager,7,nullptr,creation_state));
    assert(!snapshot.creation_only_changed(before_create,0,wanted,rate,label,rgb));snapshot.reset(); // Serializer's empty label is insufficient.
    put(&rows[0],0x70,reinterpret_cast<uintptr_t>(label));
    put(&rows[0],0x78,uint32_t(0xff000000u|rgb));rows[0].bytes[0x7c]=6;
    put(&rows[0],0x48,uint32_t(0x301)); // Native tag0 union copy leaves its inactive length undefined.
    assert(snapshot.open(p,manager,7,nullptr,creation_state));
    assert(snapshot.creation_only_changed(before_create,0,wanted,rate,label,rgb));snapshot.reset();
    rows[0].bytes[0x7c]=0;
    assert(snapshot.open(p,manager,7,nullptr,creation_state));
    assert(!snapshot.creation_only_changed(before_create,0,wanted,rate,label,rgb));snapshot.reset();rows[0].bytes[0x7c]=6;
    rows[0].bytes[0x30]=1;
    assert(snapshot.open(p,manager,7,nullptr,creation_state));
    assert(!snapshot.creation_only_changed(before_create,0,wanted,rate,label,rgb));snapshot.reset();rows[0].bytes[0x30]=0;
    put(&rows[0],24,1000000);
    assert(snapshot.open(p,manager,7,nullptr,creation_state));
    assert(!snapshot.creation_only_changed(before_create,0,wanted,rate,label,rgb));snapshot.reset();wanted.reset();clean();
    std::puts("SDK_CUE_STORE_OK");
}
