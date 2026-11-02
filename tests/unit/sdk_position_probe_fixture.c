#define AZSDK_POSITION_TEST 1
#include "../integration/sdk_position_probe.c"
#include <assert.h>

static const char *mode;
static int tick;
static int mock_snapshot(int deck,struct azstem_deck_snap *out) {
    assert(deck>=0 && deck<4 && out->size==sizeof *out);
    errno=EDOM; /* The client must preserve its own calling thread's errno. */
    int size=out->size; memset(out,0,sizeof *out); out->size=size; out->loaded=1;
    snprintf(out->title,sizeof out->title,"Deck %d",deck+1);
    out->playhead=(long long)(deck+1)*AZ_EP147_RATE+(long long)tick*(AZ_EP147_RATE/10);
    out->pos_s=(double)out->playhead/AZ_EP147_RATE;
    if(!strcmp(mode,"escape")) strcpy(out->title,"quote\" slash\\ newline\n\xe6\x97\xa5");
    if(!strcmp(mode,"nan")) out->pos_s=NAN;
    if(!strcmp(mode,"negative")) { out->playhead=-1; out->pos_s=-1; }
    if(!strcmp(mode,"unloaded")) out->loaded=0;
    if(!strcmp(mode,"unknown")) out->loaded=-1;
    if(!strcmp(mode,"bad_title")) memset(out->title,'x',sizeof out->title);
    if(!strcmp(mode,"bad_utf8")) { out->title[0]=(char)0xff; out->title[1]=0; }
    if(!strcmp(mode,"bad_size")) out->size=4;
    if(!strcmp(mode,"bad_loaded")) out->loaded=19;
    if(!strcmp(mode,"failure") && deck==2) return -1;
    if(!strcmp(mode,"identity") && tick) strcpy(out->title,"new title");
    return 0;
}
int main(int argc,char **argv) {
    assert(argc>=2); mode=argv[1]; struct position_client c; position_init(&c); c.snapshot=mock_snapshot;
    if(!strcmp(mode,"renew")) {
        assert(argc==3);int dir=open(argv[2],O_RDONLY|O_DIRECTORY);assert(dir>=0);
        char previous[33]={0};const char *name="capture.renew";
        int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL,0644);assert(fd>=0);
        assert(fchmod(fd,0644)==0);
        assert(write(fd,"11111111111111111111111111111111\n",33)==33);close(fd);
        assert(position_renew(dir,name,previous)==0);
        assert(fchmodat(dir,name,0600,0)==0);assert(position_renew(dir,name,previous)==1);
        assert(position_renew(dir,name,previous)==0);
        fd=openat(dir,name,O_WRONLY|O_TRUNC);assert(fd>=0);
        assert(write(fd,"gggggggggggggggggggggggggggggggg\n",33)==33);close(fd);
        assert(position_renew(dir,name,previous)==0);
        fd=openat(dir,name,O_WRONLY|O_TRUNC);assert(fd>=0);
        assert(write(fd,"22222222222222222222222222222222\n",33)==33);close(fd);
        assert(linkat(dir,name,dir,"linked",0)==0);assert(position_renew(dir,name,previous)==0);
        assert(unlinkat(dir,"linked",0)==0);assert(position_renew(dir,name,previous)==1);
        assert(symlinkat(name,dir,"symlink")==0);assert(position_renew(dir,"symlink",previous)==0);
        close(dir);position_dispose(&c);puts("{\"renew\":true}");return 0;
    }
    if(!strcmp(mode,"loaded_session")) {
        /* The supported 900-second acceptance session includes startup, USB
         * discovery, four asynchronous loads and the control sequence. */
        uint64_t start=UINT64_C(1000000000);
        assert(position_window(start,start+UINT64_C(900000000000),start,1)==1);
        assert(position_window(start,start+UINT64_C(1799999999999),start,1)==1);
        assert(position_window(start,start+UINT64_C(1800000000000),start,1)==0);
        position_dispose(&c); puts("{\"loaded_session\":true}"); return 0;
    }
    if(!strcmp(mode,"window")) {
        assert(position_window(100,100,0,100)==1);
        assert(position_window(100,100+POSITION_WINDOW_NS-1,100,200)==1);
        assert(position_window(100,100+POSITION_WINDOW_NS,100,200)==0);
        assert(position_window(100,99,0,200)==-1);
        assert(position_window(100,101,102,200)==-1);
        assert(position_window(100,101,100,0)==-1);
        assert(position_window(0,101,0,200)==-1);
        position_dispose(&c); puts("{\"window\":true}"); return 0;
    }
    int steps=!strcmp(mode,"history") ? 16:!strcmp(mode,"identity") ? 2:1;
    for(tick=0;tick<steps;tick++) { errno=113; position_collect(&c,UINT64_C(10000000000)+(uint64_t)tick*POSITION_PERIOD_NS); assert(errno==113); }
    if(!strcmp(mode,"failure_after")) { mode="failure"; position_collect(&c,UINT64_C(11000000000)); }
    if(!strcmp(mode,"backward")) position_collect(&c,UINT64_C(9000000000));
    struct position_buffer b;
    uint64_t boot=!strcmp(mode,"boot_error") ? 0:UINT64_C(20000000000);
    errno=127;
    size_t size=position_render(&c,getpid(),c.last_ns,boot,&b);
    assert(size && size<POSITION_BYTES && errno==127);
    if(argc==3) {
        int fd=open(argv[2],O_RDONLY|O_DIRECTORY); assert(fd>=0);
        char target[80]; snprintf(target,sizeof target,"az-sdk-position.%ld.json",(long)getpid());
        if(!strcmp(mode,"symlink") || !strcmp(mode,"hardlink") || !strcmp(mode,"preexisting")) {
            int s=openat(fd,"sentinel",O_WRONLY|O_CREAT|O_EXCL,0600); assert(s>=0);
            assert(write(s,"sentinel",8)==8); close(s);
            if(!strcmp(mode,"symlink")) assert(symlinkat("sentinel",fd,target)==0);
            else if(!strcmp(mode,"hardlink")) assert(linkat(fd,"sentinel",fd,target,0)==0);
            else assert(renameat(fd,"sentinel",fd,target)==0);
            assert(position_publish(&c,fd,getpid(),b.data,size)==-1);
            struct stat st; assert(fstatat(fd,target,&st,AT_SYMLINK_NOFOLLOW)==0);
            if(!strcmp(mode,"symlink")) assert(S_ISLNK(st.st_mode));
            else { int check=openat(fd,target,O_RDONLY); char data[8]; assert(check>=0 && read(check,data,8)==8 && !memcmp(data,"sentinel",8)); close(check); }
        } else {
            errno=117; assert(position_publish(&c,fd,getpid(),b.data,size)==0 && errno==117);
            struct stat st; assert(fstatat(fd,target,&st,AT_SYMLINK_NOFOLLOW)==0 && (st.st_mode&0777)==0600);
            if(!strcmp(mode,"write_failure")) {
                position_test_write_failure=1; errno=119; assert(position_publish(&c,fd,getpid(),b.data,size)==-1 && errno==119);
                assert(fstatat(fd,target,&st,0)<0 && errno==ENOENT);
            }
            if(!strcmp(mode,"oversized")) {
                assert(position_publish(&c,fd,getpid(),b.data,POSITION_BYTES+1)==-1);
                assert(fstatat(fd,target,&st,0)<0 && errno==ENOENT);
            }
        }
        close(fd);
    }
    fwrite(b.data,1,size,stdout); position_dispose(&c); return 0;
}
