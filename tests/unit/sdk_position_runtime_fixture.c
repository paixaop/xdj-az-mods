#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char **argv) {
    const char *directory=getenv("AZSDK_POSITION_TEST_DIR"); assert(directory);
    void *probe=NULL;
    if(argc==4) {
        void *provider=dlopen(argv[2],RTLD_NOW|RTLD_GLOBAL); assert(provider);
        probe=dlopen(argv[3],RTLD_NOW|RTLD_LOCAL); assert(probe);
        assert(dlclose(provider)==0); /* The publisher must retain its own reference. */
    }
    if(argc>1 && !strcmp(argv[1],"fork")) {
        pid_t child=fork(); assert(child>=0);
        if(child==0) exit(0); /* Copied destructor must not join the parent's worker. */
        int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    struct timespec delay={0,100000000L};
    if(argc>1 && !strcmp(argv[1],"renew")) {
        char path[512],before[65536],after[65536];
        snprintf(path,sizeof path,"%s/az-sdk-position.%ld.json",directory,(long)getpid());
        for(int i=0;i<9;i++) nanosleep(&delay,NULL);
        FILE *f=fopen(path,"rb");assert(f);size_t size=fread(before,1,sizeof before-1,f);fclose(f);before[size]=0;
        char marker[512];snprintf(marker,sizeof marker,"%s/az-sdk-position.%ld.renew",directory,(long)getpid());
        int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);
        assert(write(fd,"11111111111111111111111111111111\n",33)==33);close(fd);
        struct timespec resumed={0,250000000L};nanosleep(&resumed,NULL);
        f=fopen(path,"rb");assert(f);size=fread(after,1,sizeof after-1,f);fclose(f);after[size]=0;
        printf("{\"before\":%s,\"after\":%s}\n",before,after);
        if(probe) assert(dlclose(probe)==0);
        return 0;
    }
    for(int i=0;i<6;i++) {
        nanosleep(&delay,NULL);
        if(i==2 && argc>1 && !strcmp(argv[1],"stop")) {
            char marker[512]; snprintf(marker,sizeof marker,"%s/az-sdk-position.%ld.stop",directory,(long)getpid());
            int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0); close(fd);
        }
    }
    char path[512]; snprintf(path,sizeof path,"%s/az-sdk-position.%ld.json",directory,(long)getpid());
    FILE *f=fopen(path,"rb");
    if(!f) puts("{\"missing\":true}");
    else { char buf[4096]; size_t n; while((n=fread(buf,1,sizeof buf,f))) fwrite(buf,1,n,stdout); fclose(f); }
    if(probe) assert(dlclose(probe)==0);
    return 0;
}
