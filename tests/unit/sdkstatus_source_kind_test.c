/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../../src/common/azstatus_source_kind.h"
static int kind(int build,int member,int iface,int slot) {
    unsigned char id[12]={0};
    id[0]=(unsigned char)member;id[4]=(unsigned char)(iface|slot<<5);id[5]=1;id[6]=1;id[8]=36;
    return azn_source_kind(build,id);
}
int main(void) {
    for (int b=1;b<=2;b++) {
        assert(kind(b,1,2,0)==AZ_STATUS_SOURCE_USB1 && kind(b,1,2,1)==AZ_STATUS_SOURCE_USB2);
        assert(!kind(b,1,2,2) && !kind(b,0,2,0) && !kind(b,1,0,0));
        assert(kind(b,1,4,1)==AZ_STATUS_SOURCE_CLOUD);
        for (unsigned type=6;type<=8;type++) assert(kind(b,1,type,0)==AZ_STATUS_SOURCE_STREAMING);
        assert(kind(b,2,2,0)==AZ_STATUS_SOURCE_LINKED && kind(b,4,5,0)==AZ_STATUS_SOURCE_LINKED);
        assert(!kind(b,2,6,0) && !kind(b,7,2,0) && !kind(b,1,9,0));
    }
    assert(!kind(1,1,11,0) && kind(2,1,11,0)==AZ_STATUS_SOURCE_STREAMING);
    assert(!kind(3,1,2,0) && !azn_source_kind(2,NULL));
    puts("matched MediaID: two local USB slots, local services, known remote library and ambiguous cases passed");
}
