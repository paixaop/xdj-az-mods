/* SPDX-License-Identifier: GPL-3.0-only
 * Native persistent panel receiver: pinned process, one device write per frame.
 * Protocol: 9-byte header [F|C|Q][big-endian uint64 sequence], then F has 153
 * payload bytes. Responses are bounded ASCII READY/OK/ERR lines. Input stream
 * fragmentation never changes /dev/subucom_ctrl write boundaries. CHECK never
 * writes the device. CLOSE/EOF clear only buses used by this receiver, using
 * the driver's documented one-byte clear API, while the pinned birth matches.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "../../src/common/aztimestamp.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef SDK_PANEL_DEVICE
#define SDK_PANEL_DEVICE "/dev/subucom_ctrl"
#endif
#ifndef SDK_PANEL_PROC_ROOT
#define SDK_PANEL_PROC_ROOT "/proc"
#endif
#ifndef SDK_PANEL_IDLE_MS
#define SDK_PANEL_IDLE_MS 300000
#endif
#define FRAME_SIZE 153
static volatile sig_atomic_t interrupted;
static unsigned long pinned_pid;
static uint64_t pinned_birth;
static int device_fd=-1;
static unsigned used_buses;
static int64_t lifetime;

static int64_t now_ms(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts) || ts.tv_sec<0 || ts.tv_nsec<0 || ts.tv_nsec>=1000000000) return -1;
    return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static void stop(int signal_number) { (void)signal_number;interrupted=1; }
static int wait_fd(int fd,short events,int64_t deadline)
{
    struct pollfd p={fd,events,0};
    for(;;) {
        int64_t now=now_ms(),left;
        if(interrupted || now<0 || compare_timestamps(now,deadline,1000,1000,1)>=AZ_TIME_EQUAL || compare_timestamps(now,lifetime,1000,1000,1)>=AZ_TIME_EQUAL) return -1;
        left=deadline-now;if(compare_timestamps(left,lifetime-now,1000,1000,1)==AZ_TIME_AFTER) left=lifetime-now;
        int rc=poll(&p,1,left>INT_MAX?INT_MAX:(int)left);
        if(rc<0 && errno==EINTR) continue;
        if(rc<=0 || p.revents&POLLNVAL) return -1;
        if(p.revents&(events|POLLHUP|POLLERR)) return 0;
    }
}
static int read_exact(unsigned char *data,size_t size,int idle)
{
    size_t got=0;int64_t deadline=now_ms()+(idle?SDK_PANEL_IDLE_MS:2000);
    while(got<size) {
        if(wait_fd(STDIN_FILENO,POLLIN,deadline)) return -1;
        ssize_t n=read(STDIN_FILENO,data+got,size-got);
        if(n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if(!n) return got?-1:0;
        if(n<0) return -1;
        got+=(size_t)n;
        if(idle) { deadline=now_ms()+2000;idle=0; }
    }
    return 1;
}
static int reply(const char *text)
{
    size_t sent=0,size=strlen(text);int64_t deadline=now_ms()+5000;
    while(sent<size) {
        if(wait_fd(STDOUT_FILENO,POLLOUT,deadline)) return -1;
        ssize_t n=write(STDOUT_FILENO,text+sent,size-sent);
        if(n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        if(n<=0) return -1;
        sent+=(size_t)n;
    }
    return 0;
}
static int fail(uint64_t sequence,const char *reason)
{
    char text[128];snprintf(text,sizeof text,"ERR %" PRIu64 " %s\n",sequence,reason);reply(text);return -1;
}
static int parse_positive(const char *text,uint64_t maximum,uint64_t *value)
{
    if(!text || !*text || *text=='0') return -1;
    for(const char *p=text;*p;p++) if(!isdigit((unsigned char)*p)) return -1;
    errno=0;char *end;uintmax_t number=strtoumax(text,&end,10);
    if(errno || *end || !number || number>maximum) return -1;
    *value=(uint64_t)number;return 0;
}
static int birth_matches(void)
{
    char path[256],text[8193];snprintf(path,sizeof path,SDK_PANEL_PROC_ROOT "/%lu/stat",pinned_pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return 0;
    size_t size=0;
    while(size<sizeof text-1) {
        ssize_t n=read(fd,text+size,sizeof text-1-size);
        if(n<0 && errno==EINTR) continue;
        if(n<0) { close(fd);return 0; }
        if(!n) break;
        size+=(size_t)n;
    }
    close(fd);if(size==sizeof text-1) return 0;text[size]=0;
    char *open_paren=strchr(text,'('),*close_paren=strrchr(text,')');
    if(!open_paren || !close_paren || close_paren<open_paren || open_paren==text || open_paren[-1]!=' ') return 0;
    open_paren[-1]=0;uint64_t read_pid;
    if(parse_positive(text,INT_MAX,&read_pid) || read_pid!=pinned_pid) return 0;
    char *state,*save=NULL;unsigned field=3;
    for(char *token=strtok_r(close_paren+1," \t\r\n",&save);token;token=strtok_r(NULL," \t\r\n",&save),field++) {
        if(field==3) { state=token;if(strlen(state)!=1 || state[0]=='Z' || state[0]=='X' || state[0]=='x') return 0; }
        if(field==22) { uint64_t birth;return !parse_positive(token,UINT64_MAX,&birth) && birth==pinned_birth; }
    }
    return 0;
}
static int device_write(const unsigned char *frame,size_t size)
{
    if(!birth_matches()) return -2;
    int64_t deadline=now_ms()+5000;
    for(;;) {
        if(wait_fd(device_fd,POLLOUT,deadline)) return -1;
        /* A partial write is fatal. Never append its remainder as another
         * device operation, and never merge two panel frames in one write. */
        ssize_t n=write(device_fd,frame,size);
        if(n<0 && (errno==EINTR || errno==EAGAIN)) continue;
        return n==(ssize_t)size?0:-1;
    }
}
static int clear_used(void)
{
    for(unsigned bus=0;bus<4;bus++) if(used_buses&(1U<<bus)) {
        unsigned char neutral=(unsigned char)bus;
        if(device_write(&neutral,1)) return -1;
        used_buses&=~(1U<<bus);
    }
    return 0;
}
int main(int argc,char **argv)
{
    uint64_t pid;int result=1;char text[128];uint64_t prior=0;
    if(argc!=3 || parse_positive(argv[1],INT_MAX,&pid) || parse_positive(argv[2],UINT64_MAX,&pinned_birth)) return 2;
    pinned_pid=(unsigned long)pid;
    int64_t start=now_ms();if(start<0) return 2;lifetime=start+900000;
    struct sigaction sa;memset(&sa,0,sizeof sa);sa.sa_handler=stop;sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM,&sa,NULL);sigaction(SIGHUP,&sa,NULL);sigaction(SIGINT,&sa,NULL);signal(SIGPIPE,SIG_IGN);
    if(!birth_matches()) { fail(0,"process");return 1; }
    device_fd=open(SDK_PANEL_DEVICE,O_WRONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    struct stat st;
    if(device_fd<0 || fstat(device_fd,&st)) { fail(0,"device");goto done; }
#ifdef SDK_PANEL_ALLOW_REGULAR
    if(!S_ISCHR(st.st_mode) && !S_ISREG(st.st_mode)) { fail(0,"device_type");goto done; }
#else
    if(!S_ISCHR(st.st_mode) || st.st_uid!=0) { fail(0,"device_type");goto done; }
#endif
    if(fcntl(STDOUT_FILENO,F_SETFL,fcntl(STDOUT_FILENO,F_GETFL)|O_NONBLOCK)<0) { fail(0,"stdout");goto done; }
    snprintf(text,sizeof text,"READY 1 %lu %" PRIu64 "\n",pinned_pid,pinned_birth);
    if(reply(text)) goto done;
    for(;;) {
        unsigned char header[9],frame[FRAME_SIZE];int got=read_exact(header,sizeof header,1);
        if(!got) { result=clear_used()?1:0;goto done; }
        if(got<0) { fail(prior,"input");goto done; }
        uint64_t sequence=0;for(unsigned i=1;i<9;i++) sequence=(sequence<<8)|header[i];
        if(!sequence || prior==UINT64_MAX || sequence!=prior+1) { fail(sequence,"sequence");goto done; }
        prior=sequence;
        if(header[0]!='F' && header[0]!='C' && header[0]!='Q') { fail(sequence,"action");goto done; }
        if(!birth_matches()) { fail(sequence,"process");goto done; }
        if(header[0]=='F') {
            if(read_exact(frame,sizeof frame,0)!=1 || frame[0]>3) { fail(sequence,"frame");goto done; }
            used_buses|=1U<<frame[0];
            int wrote=device_write(frame,sizeof frame);
            if(wrote) { fail(sequence,wrote==-2?"process":"write");goto done; }
        } else if(header[0]=='Q' && clear_used()) { fail(sequence,"clear");goto done; }
        snprintf(text,sizeof text,"OK %" PRIu64 "\n",sequence);
        if(reply(text)) goto done;
        if(header[0]=='Q') { result=0;goto done; }
    }
done:
    /* An interrupted stream cannot leave a held button behind. Never touch a
     * replacement process: each clear repeats the native birth check. */
    if(device_fd>=0) { interrupted=0;lifetime=now_ms()+5000;clear_used();close(device_fd); }
    return result;
}
