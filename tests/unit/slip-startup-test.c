/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../src/azmod/slip_diagnostic/diagnostic.h"
enum { EPB_V130=1, EPB_V200=2 };
static int g_ep147_build, enabled, starts, result, install_result=1, attr_error;
static atomic_int worker_done;
static FILE *records;
static int marker(const char *path,int mode) {
    assert(!strcmp(path,"/home/root/settings/azmod.slip-capture.on") && mode==F_OK);
    return enabled ? 0 : -1;
}
static FILE *azlog_open(const char *path,const char *mode) {
    assert(!strcmp(path,"/tmp/azmod-slip-capture.log") && !strcmp(mode,"a"));
    if(!records)return NULL;
    return fdopen(dup(fileno(records)),"a");
}
static int azm_install_hook_at(uintptr_t address,void *replacement,void **original,const char *owner) {
    assert(address==4096 && replacement && original && !strcmp(owner,"slip-diagnostic"));
    *original=(void *)(uintptr_t)8192;return install_result;
}
static int fixture_attributes(pthread_attr_t *value) { return attr_error==1?EINVAL:pthread_attr_init(value); }
static int detachstate(pthread_attr_t *value,int state) { return attr_error==2?EINVAL:pthread_attr_setdetachstate(value,state); }
#define access marker
#define pthread_attr_init fixture_attributes
#define pthread_attr_setdetachstate detachstate
#include "../../src/azmod/slip_diagnostic_startup.c"
#undef access
#undef pthread_attr_init
#undef pthread_attr_setdetachstate
static void *run(void *unused) { (void)unused;atomic_store(&worker_done,1);return NULL; }
int az_slip_diagnostic_start(const char *tag,const struct az_slip_diagnostic_host *host) {
    ++starts;
    const char *expected=g_ep147_build==EPB_V130?"ep147-79d43e5de779fc1b":g_ep147_build==EPB_V200?"ep147-9e9454e357389ece":"";
    assert(!strcmp(tag,expected));
    host->log("bound %s",tag);
    void *original=NULL;
    assert(host->install(4096,(void *)(uintptr_t)1,&original)==(install_result==1?0:-1));
    assert(original==(void *)(uintptr_t)8192);
    return result;
}
int main(void) {
    records=tmpfile();assert(records);
    slip_diagnostic_at_start();assert(!starts);
    enabled=1;g_ep147_build=EPB_V130;slip_diagnostic_at_start();assert(starts==1);
    g_ep147_build=EPB_V200;slip_diagnostic_at_start();assert(starts==2);
    g_ep147_build=99;result=-1;install_result=0;slip_diagnostic_at_start();assert(starts==3);
    fflush(records);rewind(records);char text[512]={0};assert(fread(text,1,sizeof text-1,records));
    assert(strstr(text,"bound ep147-79d43e5de779fc1b") && strstr(text,"SDK_SLIP_CAPTURE_FAILED"));
    fclose(records);records=NULL;slip_diagnostic_log("unavailable log");
    attr_error=1;assert(slip_diagnostic_worker(run,NULL)==EINVAL);
    attr_error=2;assert(slip_diagnostic_worker(run,NULL)==EINVAL);
    attr_error=0;assert(!slip_diagnostic_worker(run,NULL));
    for(unsigned i=0;i<1000 && !atomic_load(&worker_done);++i){struct timespec delay={0,1000000};nanosleep(&delay,NULL);}
    assert(atomic_load(&worker_done));
    puts("PASS core Slip startup: explicit switch, bindings, install errors, worker ownership and logs");
    return 0;
}
