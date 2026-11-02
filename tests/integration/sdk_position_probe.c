/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only, opt-in transport witness. No native addresses or transport claims.
 * The source/header and exact provider hashes are recorded by the build runner.
 * Positions remain in azstem's engine domain, not the loaded file's sample rate. */
#define _GNU_SOURCE
#include "../../src/common/azdeckinfo.h"
#include "../../src/common/aztimestamp.h"
#include "../../src/azstem/ep147rate.h"
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <link.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef AZSDK_POSITION_HEADER_SHA256
#define AZSDK_POSITION_HEADER_SHA256 ""
#endif
#ifndef AZSDK_POSITION_PROVIDER_SHA256
#define AZSDK_POSITION_PROVIDER_SHA256 ""
#endif
#ifndef AZSDK_POSITION_BUILD_ID
#define AZSDK_POSITION_BUILD_ID ""
#endif
#define POSITION_HISTORY 12
#define POSITION_BYTES 65536
#define POSITION_PERIOD_NS UINT64_C(100000000)
/* Each explicit capture lease is bounded to 30 minutes. A new case can renew
 * it without restarting firmware; renewal discards the previous history. */
#if defined(AZSDK_POSITION_TEST) && defined(AZSDK_POSITION_TEST_WINDOW_NS)
#define POSITION_WINDOW_NS AZSDK_POSITION_TEST_WINDOW_NS
#else
#define POSITION_WINDOW_NS UINT64_C(1800000000000)
#endif
_Static_assert(sizeof(struct azstem_deck_snap)==296,"snapshot ABI changed");
_Static_assert(offsetof(struct azstem_deck_snap,playhead)==16,"snapshot ABI changed");
_Static_assert(offsetof(struct azstem_deck_snap,pos_s)==24,"snapshot ABI changed");
_Static_assert(offsetof(struct azstem_deck_snap,title)==40,"snapshot ABI changed");

struct position_deck { int read_rc,loaded; unsigned content_id; long long playhead; double pos_s; char title[AZSNAP_STR]; };
struct position_sample { uint64_t sequence,ns; struct position_deck decks[4]; };
struct position_client {
    azstem_deck_snapshot_fn snapshot;
    void *provider;
    locale_t numeric;
    struct position_sample samples[POSITION_HISTORY];
    size_t count;
    uint64_t sequence,last_ns;
    const char *status;
    dev_t published_device;
    ino_t published_inode;
    int owns_publication;
    char capture_nonce[33];
};
struct position_buffer { char data[POSITION_BYTES]; size_t used; int bad; };
static struct position_client position;
static pthread_t position_thread;
static atomic_int position_stop=ATOMIC_VAR_INIT(0);
static int position_started;
static pid_t position_owner;
#ifdef AZSDK_POSITION_TEST
static int position_test_write_failure;
#endif

static uint64_t position_clock(clockid_t clock) {
    struct timespec ts;
    if(clock_gettime(clock,&ts) || ts.tv_sec<0 || ts.tv_nsec<0 || ts.tv_nsec>=1000000000L ||
       (uint64_t)ts.tv_sec>UINT64_MAX/UINT64_C(1000000000)) return 0;
    uint64_t seconds=(uint64_t)ts.tv_sec*UINT64_C(1000000000);
    if(seconds>UINT64_MAX-(uint64_t)ts.tv_nsec) return 0;
    return seconds+(uint64_t)ts.tv_nsec;
}
static void position_clear(struct position_client *c,const char *status) {
    memset(c->samples,0,sizeof c->samples); c->count=0; c->status=status;
}
static void position_init(struct position_client *c) {
    memset(c,0,sizeof *c); c->status="unavailable";
    c->numeric=newlocale(LC_NUMERIC_MASK,"C",(locale_t)0);
}
static void position_dispose(struct position_client *c) {
    if(c->provider) dlclose(c->provider);
    if(c->numeric) freelocale(c->numeric);
    c->provider=NULL; c->numeric=(locale_t)0; c->snapshot=NULL;
}

static int position_utf8(const char *s,size_t size) {
    for(size_t i=0;i<size;) {
        unsigned char first=(unsigned char)s[i++];
        if(first<0x80) continue;
        unsigned extra,code,min;
        if(first>=0xc2 && first<=0xdf) { extra=1; code=first&31; min=0x80; }
        else if(first>=0xe0 && first<=0xef) { extra=2; code=first&15; min=0x800; }
        else if(first>=0xf0 && first<=0xf4) { extra=3; code=first&7; min=0x10000; }
        else return 0;
        if(extra>size-i) return 0;
        while(extra--) { unsigned char next=(unsigned char)s[i++]; if((next&0xc0)!=0x80) return 0; code=(code<<6)|(next&63); }
        if(code<min || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return 0;
    }
    return 1;
}
static void position_add(struct position_buffer *b,const char *format,...) {
    if(b->bad) return;
    va_list args; va_start(args,format);
    int n=vsnprintf(b->data+b->used,sizeof b->data-b->used,format,args); va_end(args);
    if(n<0 || (size_t)n>=sizeof b->data-b->used) b->bad=1;
    else b->used+=(size_t)n;
}
static void position_string(struct position_buffer *b,const char *s,size_t maximum) {
    position_add(b,"\"");
    for(size_t i=0;i<maximum && s[i];i++) {
        unsigned char ch=(unsigned char)s[i];
        if(ch=='"' || ch=='\\') position_add(b,"\\%c",ch);
        else if(ch<32 || ch==127) position_add(b,"\\u%04x",(unsigned)ch);
        else position_add(b,"%c",ch);
    }
    position_add(b,"\"");
}

static int position_collect(struct position_client *c,uint64_t now) {
    int saved_errno=errno,result=0;
    if(c->sequence==UINT64_MAX) goto unavailable;
    ++c->sequence;
    if(!c->snapshot || !c->numeric || !now || compare_timestamps(now,c->last_ns,1000000000,1000000000,1)<=AZ_TIME_EQUAL) goto unavailable;
    c->last_ns=now;
    struct position_sample sample; memset(&sample,0,sizeof sample);
    sample.sequence=c->sequence; sample.ns=now;
    for(int deck=0;deck<4;deck++) {
        struct azstem_deck_snap snap; memset(&snap,0,sizeof snap);
        snap.size=sizeof snap; snap.loaded=-1; snap.playhead=-1; snap.pos_s=-1;
        int rc=c->snapshot(deck,&snap);
        size_t title_size=strnlen(snap.title,sizeof snap.title);
        if(rc || snap.size!=sizeof snap || snap.loaded<-1 || snap.loaded>1 ||
           title_size==sizeof snap.title || !position_utf8(snap.title,title_size)) goto unavailable;
        struct position_deck *copy=&sample.decks[deck];
        copy->read_rc=rc; copy->loaded=snap.loaded; copy->content_id=snap.content_id;
        copy->playhead=snap.loaded==1 ? snap.playhead:-1;
        copy->pos_s=snap.loaded==1 ? snap.pos_s:-1;
        if(snap.loaded==1) memcpy(copy->title,snap.title,title_size+1);
    }
    if(c->count) {
        const struct position_sample *previous=&c->samples[c->count-1];
        for(int deck=0;deck<4;deck++) {
            if(previous->decks[deck].loaded!=sample.decks[deck].loaded ||
               previous->decks[deck].content_id!=sample.decks[deck].content_id ||
               strcmp(previous->decks[deck].title,sample.decks[deck].title)) {
                position_clear(c,"unavailable"); break;
            }
        }
    }
    if(c->count==POSITION_HISTORY) {
        memmove(c->samples,c->samples+1,(POSITION_HISTORY-1)*sizeof *c->samples); --c->count;
    }
    c->samples[c->count++]=sample; c->status="ok"; result=1; goto out;
unavailable: position_clear(c,"unavailable");
out: errno=saved_errno; return result;
}

static size_t position_render(struct position_client *c,pid_t pid,uint64_t now,uint64_t boottime,
                              struct position_buffer *b) {
    int saved_errno=errno; memset(b,0,sizeof *b);
    if(!now || !boottime) position_clear(c,"unavailable");
    locale_t previous=(locale_t)0;
    if(c->numeric) previous=uselocale(c->numeric);
    position_add(b,"{\"schema\":1,\"pid\":%ld,\"sequence\":%" PRIu64 ",\"native_monotonic_ns\":%" PRIu64 ",\"native_boottime_ns\":%" PRIu64 ",\"status\":",(long)pid,c->sequence,now,boottime);
    position_string(b,c->status,20);
    position_add(b,",\"capture_nonce\":\"%s\"",c->capture_nonce);
    position_add(b,",\"engine_rate\":%d,\"abi_size\":%zu,\"header_sha256\":\"%s\",\"expected_provider_sha256\":\"%s\",\"provider_build_id\":\"%s\",\"samples\":[",AZ_EP147_RATE,sizeof(struct azstem_deck_snap),AZSDK_POSITION_HEADER_SHA256,AZSDK_POSITION_PROVIDER_SHA256,c->provider ? AZSDK_POSITION_BUILD_ID:"");
    for(size_t n=0;n<c->count;n++) {
        const struct position_sample *s=&c->samples[n];
        position_add(b,"%s{\"sequence\":%" PRIu64 ",\"native_monotonic_ns\":%" PRIu64 ",\"decks\":[",n ? ",":"",s->sequence,s->ns);
        for(int deck=0;deck<4;deck++) {
            const struct position_deck *d=&s->decks[deck];
            position_add(b,"%s{\"deck\":%d,\"read_rc\":%d,\"loaded\":%d,\"title\":",deck ? ",":"",deck,d->read_rc,d->loaded);
            position_string(b,d->title,sizeof d->title);
            position_add(b,",\"playhead\":");
            if(d->loaded==1 && d->playhead>=0) position_add(b,"%lld",d->playhead); else position_add(b,"null");
            position_add(b,",\"pos_s\":");
            if(c->numeric && d->loaded==1 && isfinite(d->pos_s) && d->pos_s>=0) position_add(b,"%.17g",d->pos_s); else position_add(b,"null");
            position_add(b,"}");
        }
        position_add(b,"]}");
    }
    position_add(b,"]}\n");
    if(c->numeric) uselocale(previous);
    errno=saved_errno; return b->bad ? 0:b->used;
}

static int position_target(int directory,const char *target,struct stat *s) {
    if(fstatat(directory,target,s,AT_SYMLINK_NOFOLLOW)<0) return errno==ENOENT ? 0:-1;
    return S_ISREG(s->st_mode) && s->st_uid==geteuid() && s->st_nlink==1 &&
           (s->st_mode&0777)==0600 ? 1:-1;
}
static void position_remove_owned(struct position_client *c,int directory,const char *target) {
    struct stat s;
    if(c->owns_publication && position_target(directory,target,&s)==1 &&
       s.st_dev==c->published_device && s.st_ino==c->published_inode) unlinkat(directory,target,0);
    c->owns_publication=0;
}
static int position_publish(struct position_client *c,int directory,pid_t pid,const char *bytes,size_t size) {
    int saved_errno=errno,result=-1,file=-1,owns_temporary=0;
    char target[80],temporary[128]={0}; struct stat existing,created;
    snprintf(target,sizeof target,"az-sdk-position.%ld.json",(long)pid);
    if(pid<2 || !bytes || !size || size>POSITION_BYTES) goto failed;
    int target_state=position_target(directory,target,&existing);
    if(target_state<0 || (target_state==1 && (!c->owns_publication ||
       existing.st_dev!=c->published_device || existing.st_ino!=c->published_inode))) goto failed;
    for(unsigned attempt=0;attempt<4;attempt++) {
        uint64_t nonce;
        if(syscall(SYS_getrandom,&nonce,sizeof nonce,1)!=(long)sizeof nonce) goto failed;
        snprintf(temporary,sizeof temporary,".az-sdk-position.%ld.%016" PRIx64 ".tmp",(long)pid,nonce);
        file=openat(directory,temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(file>=0) { owns_temporary=1; break; }
        if(errno!=EEXIST) goto failed;
        temporary[0]=0;
    }
    if(file<0) goto failed;
    if(fchmod(file,0600) || fstat(file,&created) || !S_ISREG(created.st_mode) ||
       created.st_uid!=geteuid() || created.st_nlink!=1) goto failed;
    size_t offset=0;
    while(offset<size) {
        ssize_t n;
#ifdef AZSDK_POSITION_TEST
        if(position_test_write_failure) { errno=ENOSPC; n=-1; }
        else
#endif
        n=write(file,bytes+offset,size-offset);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) goto failed;
        offset+=(size_t)n;
    }
    target_state=position_target(directory,target,&existing);
    if(target_state<0 || (target_state==1 && (!c->owns_publication ||
       existing.st_dev!=c->published_device || existing.st_ino!=c->published_inode))) goto failed;
    if(renameat(directory,temporary,directory,target)) goto failed;
    temporary[0]=0; c->published_device=created.st_dev; c->published_inode=created.st_ino;
    c->owns_publication=1; result=0; goto out;
failed:
    position_clear(c,"unavailable"); position_remove_owned(c,directory,target);
out:
    if(file>=0) close(file);
    if(owns_temporary && temporary[0]) unlinkat(directory,temporary,0);
    errno=saved_errno; return result;
}

struct position_resolver { uintptr_t anchor; unsigned char expected[64]; size_t size; void *pin; };
static int position_hex(const char *s,unsigned char *out,size_t *size) {
    size_t n=strlen(s); if(!n || (n&1) || n>128) return 0;
    for(size_t i=0;i<n;i+=2) {
        unsigned value=0;
        for(size_t j=0;j<2;j++) { unsigned char ch=(unsigned char)s[i+j]; if(ch>='0' && ch<='9') value=(value<<4)|(ch-'0'); else if(ch>='a' && ch<='f') value=(value<<4)|(ch-'a'+10); else return 0; }
        out[i/2]=(unsigned char)value;
    }
    *size=n/2; return 1;
}
static int position_span(uintptr_t base,uint64_t offset,uint64_t size,uintptr_t *a,uintptr_t *z) {
    if(offset>UINTPTR_MAX-base) return 0;
    *a=base+(uintptr_t)offset;
    if(size>UINTPTR_MAX-*a) return 0;
    *z=*a+(uintptr_t)size; return 1;
}
static int position_provider(struct dl_phdr_info *info,size_t bytes,void *opaque) {
    struct position_resolver *r=opaque;
    if(bytes<offsetof(struct dl_phdr_info,dlpi_phnum)+sizeof info->dlpi_phnum ||
       !info->dlpi_phdr || !info->dlpi_phnum || info->dlpi_phnum>128) return 0;
    int executable=0; size_t total=0,records=0,matched=0,ids=0;
    for(size_t i=0;i<info->dlpi_phnum;i++) {
        const ElfW(Phdr) *p=&info->dlpi_phdr[i]; uintptr_t a,z;
        if(p->p_type==PT_LOAD && position_span(info->dlpi_addr,p->p_vaddr,p->p_memsz,&a,&z) &&
           (p->p_flags&(PF_R|PF_W|PF_X))==(PF_R|PF_X) && r->anchor>=a && r->anchor<z) executable=1;
    }
    if(!executable) return 0;
    Dl_info symbol;
    if(!dladdr((void *)r->anchor,&symbol) || symbol.dli_saddr!=(void *)r->anchor ||
       !symbol.dli_sname || strcmp(symbol.dli_sname,"azstem_deck_snapshot")) return 1;
    for(size_t i=0;i<info->dlpi_phnum;i++) {
        const ElfW(Phdr) *p=&info->dlpi_phdr[i];
        if(p->p_type!=PT_NOTE || !p->p_filesz) continue;
        if(p->p_filesz>16384 || total>65536-p->p_filesz) return 1;
        total+=(size_t)p->p_filesz;
        uintptr_t a,z; if(!position_span(info->dlpi_addr,p->p_vaddr,p->p_filesz,&a,&z)) return 1;
        int readable=0;
        for(size_t j=0;j<info->dlpi_phnum;j++) {
            const ElfW(Phdr) *load=&info->dlpi_phdr[j]; uintptr_t x,y;
            if(load->p_type==PT_LOAD && (load->p_flags&PF_R) && position_span(info->dlpi_addr,load->p_vaddr,load->p_memsz,&x,&y) && a>=x && z<=y) readable=1;
        }
        if(!readable) return 1;
        size_t offset=0;
        while(offset<(size_t)p->p_filesz) {
            if(++records>256 || (size_t)p->p_filesz-offset<sizeof(ElfW(Nhdr))) return 1;
            ElfW(Nhdr) note; memcpy(&note,(void *)(a+offset),sizeof note); offset+=sizeof note;
            uint64_t name=((uint64_t)note.n_namesz+3)&~UINT64_C(3), data=((uint64_t)note.n_descsz+3)&~UINT64_C(3);
            if(name>(size_t)p->p_filesz-offset || data>(size_t)p->p_filesz-offset-name) return 1;
            if(note.n_type==NT_GNU_BUILD_ID && note.n_namesz==4 && !memcmp((void *)(a+offset),"GNU",4)) {
                ids++; if(note.n_descsz==r->size && !memcmp((void *)(a+offset+name),r->expected,r->size)) matched++;
            }
            offset+=(size_t)(name+data);
        }
    }
    if(ids==1 && matched==1 && info->dlpi_name && info->dlpi_name[0])
        r->pin=dlopen(info->dlpi_name,RTLD_NOW|RTLD_NOLOAD|RTLD_LOCAL);
    return 1;
}
static void position_resolve(struct position_client *c) {
    int saved_errno=errno; struct position_resolver r; memset(&r,0,sizeof r);
    if(strlen(AZSDK_POSITION_HEADER_SHA256)!=64 || strlen(AZSDK_POSITION_PROVIDER_SHA256)!=64 ||
       !position_hex(AZSDK_POSITION_BUILD_ID,r.expected,&r.size)) goto out;
    void *symbol=dlsym(RTLD_DEFAULT,"azstem_deck_snapshot"); /* One lookup per process. */
    if(!symbol) goto out;
    r.anchor=(uintptr_t)symbol;
    dl_iterate_phdr(position_provider,&r);
    if(r.pin) { c->provider=r.pin; c->snapshot=(azstem_deck_snapshot_fn)symbol; }
out: errno=saved_errno;
}

static int position_process(void) {
    char path[512]; ssize_t n=readlink("/proc/self/exe",path,sizeof path-1);
    if(n<=0 || n>=(ssize_t)sizeof path-1) return 0;
    path[n]=0; const char *base=strrchr(path,'/'); return base && !strcmp(base+1,"EP147");
}
static int position_window(uint64_t start,uint64_t now,uint64_t previous,uint64_t boot) {
    if(!start || !now || !boot || compare_timestamps(now,start,1000000000,1000000000,1)==AZ_TIME_BEFORE || compare_timestamps(now,previous,1000000000,1000000000,1)==AZ_TIME_BEFORE) return -1;
    return compare_timestamps(now-start,POSITION_WINDOW_NS,1000000000,1000000000,1)==AZ_TIME_BEFORE;
}
static int position_renew(int directory,const char *marker,char previous[33]) {
    int fd=openat(directory,marker,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) return 0;
    struct stat st; char nonce[34];
    int valid=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        (st.st_mode&0777)==0600 && st.st_nlink==1 && st.st_size==33;
    ssize_t size=valid ? read(fd,nonce,sizeof nonce):-1;
    close(fd);
    if(size!=33 || nonce[32]!='\n') return 0;
    for(unsigned i=0;i<32;i++) if(!((nonce[i]>='0' && nonce[i]<='9') || (nonce[i]>='a' && nonce[i]<='f'))) return 0;
    nonce[32]=0;
    if(!memcmp(nonce,previous,33)) return 0;
    memcpy(previous,nonce,33); return 1;
}
static void *position_worker(void *unused) {
    (void)unused; const char *directory="/tmp";
#ifdef AZSDK_POSITION_TEST
    const char *test=getenv("AZSDK_POSITION_TEST_DIR"); if(test && test[0]) directory=test;
#endif
    int fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return NULL;
    char marker[80],renewal[80]; snprintf(marker,sizeof marker,"az-sdk-position.%ld.stop",(long)position_owner);
    snprintf(renewal,sizeof renewal,"az-sdk-position.%ld.renew",(long)position_owner);
    uint64_t start=position_clock(CLOCK_MONOTONIC),previous=start;
    for(;;) {
        uint64_t now=position_clock(CLOCK_MONOTONIC),boot=position_clock(CLOCK_BOOTTIME);
        int window=position_window(start,now,previous,boot);
        if(window>=0) previous=now;
        struct stat stop; int stopped=atomic_load_explicit(&position_stop,memory_order_acquire) ||
            (fstatat(fd,marker,&stop,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(stop.st_mode) && stop.st_uid==geteuid() && (stop.st_mode&0777)==0600);
        if(!stopped && window>=0 && position_renew(fd,renewal,position.capture_nonce)) {
            start=now;position_clear(&position,"unavailable");window=1;
        }
        if(window<0) position_clear(&position,"unavailable");
        else if(stopped || !window) position_clear(&position,"stopped");
        else position_collect(&position,now);
        struct position_buffer document;
        size_t size=position_render(&position,position_owner,now,boot,&document);
        if(position_publish(&position,fd,position_owner,document.data,size)) position_clear(&position,"unavailable");
        if(stopped) break;
        struct timespec period={0,(long)POSITION_PERIOD_NS};
        while(nanosleep(&period,&period)<0 && errno==EINTR && !atomic_load_explicit(&position_stop,memory_order_acquire)) {
            uint64_t interrupted=position_clock(CLOCK_MONOTONIC);
            if(position_window(start,interrupted,now,boot)<0) break;
        }
    }
    close(fd); return NULL;
}
__attribute__((constructor)) static void position_start(void) {
    int saved_errno=errno; const char *flag=getenv("AZSDK_POSITION_PROBE");
    if(!flag || strcmp(flag,"1") || !position_process()) goto out;
    position_owner=getpid(); position_init(&position); position_resolve(&position);
    if(pthread_create(&position_thread,NULL,position_worker,NULL)==0) position_started=1;
    else position_dispose(&position);
out: errno=saved_errno;
}
__attribute__((destructor)) static void position_end(void) {
    int saved_errno=errno;
    if(position_started && getpid()==position_owner) {
        atomic_store_explicit(&position_stop,1,memory_order_release); pthread_join(position_thread,NULL);
        position_dispose(&position);
    }
    errno=saved_errno;
}
