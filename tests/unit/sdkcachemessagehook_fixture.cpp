/* SPDX-License-Identifier: GPL-3.0-only */
/* Only the preexisting onMessage tail is extracted. Real copied-context helper
 * and C bridge are linked unchanged; the original destroys its genuine input. */
#include "azcachemessage.h"
#include "azsources.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#define REQUIRE(x,why) do{if(!(x)){std::fprintf(stderr,"FAIL message-hook: %s\n",why);std::abort();}}while(0)
namespace cm=azsdk_cache_message;
struct alignas(8) Arg { unsigned tag=1,pad=0,value=0;unsigned char tail[36]={}; };
struct Message {
 unsigned short kind=0x2102,reserved=0;unsigned magic=0x872349ae,sequence=13,pad=0;
 unsigned long unused[3]={};std::vector<Arg> args;
 explicit Message(unsigned context):args(2){args[0].value=(context<<24)|((context+1)<<16);args[1].value=117;}
};
static int g_sdk_sources_on,g_ep147_build,stage,source_calls,pst_calls;
static unsigned context_id;static bool throwing;
static void *expected_server,*expected_message;
static void original(void*server,void*message,unsigned parameter){
 REQUIRE(server==expected_server&&message==expected_message&&parameter==37&&stage==0,"actual native args/order unchanged");
 REQUIRE(static_cast<Message*>(message)->args[1].value==117,"original still receives unchanged live input");
 std::puts("NATIVE_HOOK_FORWARD_OK args=3 content=117");std::fflush(stdout);
 cm::Envelope e;
 REQUIRE(cm::current(server,e)==bool(g_sdk_sources_on),"native hook must enter copied message bridge before original");
 if(g_sdk_sources_on)REQUIRE(e.fields[1]==117&&e.client==context_id&&e.selector==context_id+1,"configured copied scalars inside original");
 delete static_cast<Message*>(message);expected_message=nullptr;stage=1;
 if(g_sdk_sources_on)REQUIRE(cm::current(server,e)&&e.fields[1]==117,"frame survives input destruction without reread");
 if(throwing)throw std::runtime_error("exact native hook exception");
}
static azsdk_source_message_original g_orig_mlserver_onmsg=original;
extern "C" int azsdk_sources_observe_owned_message(void*server,void*out,size_t size) noexcept {
 cm::Envelope e;REQUIRE(server==expected_server&&!out&&!size&&stage==1&&!cm::current(server,e),"source post-observation after original scope cleanup");
 ++source_calls;stage=2;return AZS_OK;
}
static void pst_msg_done(void*server){REQUIRE(server==expected_server&&stage==(g_sdk_sources_on?2:1),"playlist post-step keeps original order");++pst_calls;stage=3;}
static void hook_tail(void*server,void*msg,unsigned param){
/* ACTUAL_NATIVE_HOOK_TAIL */
int main(){
 static_assert(sizeof(Message)==64&&sizeof(Arg)==48,"native message prefix");
 for(g_ep147_build=1;g_ep147_build<=2;++g_ep147_build)for(context_id=0;context_id<4;++context_id)
  for(g_sdk_sources_on=0;g_sdk_sources_on<2;++g_sdk_sources_on)for(int fail=0;fail<2;++fail){
   int server;expected_server=&server;expected_message=new Message(context_id);throwing=fail;stage=source_calls=pst_calls=0;bool caught=false;
   try{hook_tail(expected_server,expected_message,37);}catch(const std::runtime_error&){caught=true;}
   cm::Envelope e;REQUIRE(caught==throwing&&!expected_message&&!cm::current(expected_server,e),"native exception/normal cleanup remains transparent");
   REQUIRE(throwing?(stage==1&&!source_calls&&!pst_calls):(stage==3&&source_calls==g_sdk_sources_on&&pst_calls==1),"post-steps only after normal native return");
  }
 std::puts("PASS message-hook: actual tail enters one bridge, preserves input/exception/postorder;4contexts/bothprofiles");
}
