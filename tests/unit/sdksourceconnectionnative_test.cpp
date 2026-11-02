/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual SourceState + exact native memory-only connections. A missing
 * startup bridge only forwards genuine native APIs, without an observer model. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include "../../src/azmod/sdk_native_sources.cpp"
using SourceInstaller=int(*)(std::uintptr_t,void*,void**,void*);
extern "C" int azsdk_sources_install_connections(SourceInstaller,void*) noexcept __attribute__((weak));
extern "C" int azsdk_test_connection_island(std::uintptr_t,void*,void**);
#define REQUIRE(x,why) do{if(!(x)){std::fprintf(stderr,"FAIL source-connection-native: %s\n",why);std::abort();}}while(0)
namespace {
int installer(std::uintptr_t entry,void*replacement,void**original,void*){
 return azsdk_test_connection_island(entry,replacement,original);
}
}
int main(int argc,char**argv){
 REQUIRE(argc==3,"explicit native module and build");int build=std::atoi(argv[2]);REQUIRE(build==1||build==2,"supported build label");
 void*module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!module){std::fprintf(stderr,"DLOPEN_FAILURE %s\n",dlerror());return 77;}
 auto open=reinterpret_cast<int(*)(const char*,sqlite3**,int,const char*)>(dlsym(module,"sqlite3_open_v2"));
 auto close=reinterpret_cast<int(*)(sqlite3*)>(dlsym(module,"sqlite3_close"));REQUIRE(open&&close,"exact native public APIs");
 const bool late=std::getenv("AZSDK_TEST_LATE_PHYSICAL")!=nullptr;
 REQUIRE(azsdk_sources_configure(1,build,late?0u:unsigned(AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER))==AZS_OK,
         "SourceState configured before actual native installation");
 REQUIRE(bool(azsdk_sources_physical_ready())==!late,"physical metadata remains independently opt-in");
 sqlite3*baseline=nullptr;REQUIRE(open(":memory:",&baseline,6,nullptr)==0&&baseline&&close(baseline)==0,"genuine native memory baseline");
 std::printf("NATIVE_SOURCE_STATE_BASELINE_OK memory=1 initial_physical=%u\n",!late);std::fflush(stdout);
 auto bridge=azsdk_sources_install_connections;if(bridge)(void)bridge(installer,nullptr);
 sqlite3*db=nullptr;REQUIRE(open(":memory:",&db,6,nullptr)==0&&db,"genuine native open after startup bridge");
 azsdk_connection_epoch::Receipt receipt={0};
 REQUIRE(source_state().connection_epochs.read(db,receipt)&&receipt.generation,"missing SourceState actual native connection installation bridge");
 auto generation=receipt.generation;
 REQUIRE(azsdk_sources_configure(0,build,0)==AZS_UNSUPPORTED,"source diagnostics disable independently");
 REQUIRE(close(db)==0&&!source_state().connection_epochs.read(db,receipt),"installed close retirement cannot be disabled with diagnostics");
 db=nullptr;REQUIRE(open(":memory:",&db,6,nullptr)==0&&source_state().connection_epochs.read(db,receipt)&&receipt.generation>generation,"startup bridge retains one canonical nonreset state while diagnostics off");
 REQUIRE(azsdk_sources_configure(1,build,AZS_CAPTURE_PHYSICAL_ON_LIBRARY_WORKER)==AZS_OK&&azsdk_sources_physical_ready(),"first or reused physical view consumes sealed proof after our own code redirects");
 REQUIRE(close(db)==0,"final native disposable connection cleanup");
 std::puts("PASS source-connection-native: shared proof, canonical process state, actual native startup forwarding/retirement; no EP147/file DB/key");
}
