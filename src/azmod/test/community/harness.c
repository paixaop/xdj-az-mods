/* SPDX-License-Identifier: GPL-3.0-only */
/* Test harness for community.c: the real host, compiled with stubs for the azmod internals it
 * borrows, loading real test mods (mods.c) from a temp stage. Run by run.sh in the build
 * container. Usage: harness <root> init|tick <ms>|tag|claim <id> <addr-kind>|starting */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include "menu.h"
#include "../../../common/aztimestamp.h"

#define AZMOD_STEM_DECKS 4
#define BPMSTATE_LIVE 0xa0
static void dbg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("dbg: ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
}
static _Atomic long g_fake_now = 0; /* advanced by main, read by the mods' threads */
static long now_ms(void) { return g_fake_now; }
static int fake_time_within(long deadline) {
    int order = compare_timestamps(now_ms(), deadline, 1000, 1000, 1);
    if (order == AZ_TIME_INVALID) abort();
    return order == AZ_TIME_BEFORE || order == AZ_TIME_EQUAL;
}
static int g_four_deck = 0;
static int g_cr_fd = -1; /* crash reporter not armed: community installs its own blame handler */
static int (*g_deck_has_track_fn)(int) = NULL;
static int bpmstate_sample(int d, double *live, double *track, uint64_t *generation) {
    (void) d;
    (void) live;
    (void) track;
    *generation = 0;
    return 0;
}
static int bpmstate_current(int deck, uint64_t generation) { (void)deck; (void)generation; return 0; }
static int deck_for_ds(long ds) { return ds == 0x1000 ? 2 : -1; }
static int deck_for_modeobj(long p) {
    (void) p;
    return -1;
}
#define AZM_HOOKED_MAX 512
enum {
    AZM_REFUSED_LATE = -3
};
static struct {
    uintptr_t addr;
    const char *who;
} g_hooked[AZM_HOOKED_MAX];
static int g_hooked_n = 0;
static const char *g_hook_owner = "AZ-MODS";
static int azm_install_hook(uintptr_t a, void *h, void **o) {
    (void) h;
    *o = (void *) a;
    g_hooked[g_hooked_n].addr = a;
    g_hooked[g_hooked_n].who = g_hook_owner;
    g_hooked_n++;
    return 1;
}
/* menu.c draw helpers: not exercised here */
static int g_headers; /* draw_header calls that reached the drawing code (the paint test) */
void azcm_draw_header(az_draw *d, const char *t) {
    (void) d;
    (void) t;
    g_headers++;
}
void azcm_draw_text(az_draw *d, int x, int y, const char *s, int px, uint32_t c) {
    (void) d;
    (void) x;
    (void) y;
    (void) s;
    (void) px;
    (void) c;
}
void azcm_fill_rect(az_draw *d, int x, int y, int w, int h, uint32_t c) {
    (void) d;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) c;
}
void azcm_draw_rect(az_draw *d, int x, int y, int w, int h, int t, uint32_t c) {
    (void) d;
    (void) x;
    (void) y;
    (void) w;
    (void) h;
    (void) t;
    (void) c;
}
void azcm_draw_checkbox(az_draw *d, int x, int y, int s, int on, int hi) {
    (void) d;
    (void) x;
    (void) y;
    (void) s;
    (void) on;
    (void) hi;
}
uint32_t azcm_color(int r) { return (uint32_t) r; }
int azcm_row_height(az_draw *d) {
    (void) d;
    return 48;
}
int azcm_text_width(const char *s, int px) { return (int) strlen(s) * px / 2; }

/* ep_mem.h's fault-free read for the crash reporter's frame walk (the real one checks EP147's
 * address ranges first; here any mapped address will do) */
#include <sys/uio.h>
static int crash_peek8(uintptr_t a, uintptr_t *out) {
    struct iovec l = {out, 8}, r = {(void *) a, 8};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == 8;
}
/* the settings file azmod.c owns (g_conf): the harness loads it at start, the flush saves it */
#include "../../azconf.c"
#define CONF_PATH CM_ROOT "/azmod.conf"
static struct azconf *g_conf;
static int g_conf_readonly;
static long g_rtick_n; /* pad_leds.c's repaint tick count (the quarantine's heartbeat): none here */
static __thread int g_cm_on_ui;
#define SDK_SURFACE_IMPL "test/community/sdk_surface_stub.c" /* no overlay thread here */
#define SDK_DECKUI_IMPL "test/community/sdk_deckui_stub.c"  /* no azstem, no EP147 here */
#define SDK_PROC_IMPL "test/community/sdk_proc_stub.c"      /* no audio, systemd or stick here */
#define SDK_ROWS_IMPL "test/community/sdk_rows_stub.c"      /* no feature table or settings here */
#include "../../community.c"
#include "../../sdk_input.c"   /* the frame and touch filters: plain logic, no EP147 needed */

/* azstem's exports as the Reload confirm sees them: the stems-following per-deck export says
 * "empty" (a stick without a STEMS tree), azstem_decks_loaded answers from LoadedTrackInfo */
static int g_fake_decks = -1;
int azstem_decks_loaded(void) { return g_fake_decks; }
/* azstem's per-deck snapshot as deck_loaded / deck_title read it: CM_TEST_DECKS gives each
 * deck's state ('L' loaded, 'E' empty, anything else unknown); a loaded deck's title is
 * "Title <n>". No azstem_deck_track_name here: a stick without a STEMS tree. */
int azstem_deck_snapshot(int deck, struct azstem_deck_snap *out) {
    const char *st = getenv("CM_TEST_DECKS");
    if (!out || out->size < (int) sizeof *out || deck < 0 || deck > 3)
        return -1;
    memset(out, 0, sizeof *out);
    out->size = (int) sizeof *out;
    char c = st && (int) strlen(st) > deck ? st[deck] : 'U';
    out->loaded = c == 'L' ? 1 : c == 'E' ? 0
                                          : -1;
    if (out->loaded == 1)
        snprintf(out->title, sizeof out->title, "%s%d", getenv("CM_TEST_TITLE") ? getenv("CM_TEST_TITLE") : "Title ", deck + 1);
    return 0;
}
static int stems_export_empty(int d) {
    (void) d;
    return 0;
}
/* kept out of main(): "tagmp" re-protects pages at fixed offsets from main, so main's size matters */
static int anyloaded_cmd(int argc, char **argv) {
    g_deck_has_track_fn = stems_export_empty;
    if (!strcmp(argv[1], "anyloaded") && argc > 2) {        /* the Reload confirm's question */
        g_fake_decks = atoi(argv[2]);
        printf("%d\n", azcm_any_loaded());
        return 0;
    }
    int any = 0;                                           /* anyloaded-old: the stems export alone */
    for (int d = 0; d < AZMOD_STEM_DECKS; d++)
        if (g_deck_has_track_fn(d)) any = 1;
    printf("%d\n", any);
    return 0;
}

/* "tagmp" mis-flags a code page (PROT_READ). A page taken from main's neighbors holds
 * whatever the link put there, so a change anywhere could put code on it (a SIGSEGV, read as
 * "tag moved"). It is a dedicated page: page-aligned, a page long (64 KB
 * covers every page size), never called. */
__attribute__((noinline, aligned(65536))) static void tagmp_victim(void) { __asm__ volatile(".skip 65536"); }

static int status_cadence_cmd(void) {
    if (ds_publish(2,1,DS_BOUNDARY,ds_now(),NULL,0,NULL,NULL,0,0)!=AZ_OK) return 2;
    az_status_coord_v1 head={0,0,44100,0};
    const struct ds_time_axis clock={44100, NULL, 64, 44100, NULL};
    struct ds_field f={AZ_STATUS_POSITION,AZ_STATUS_POSITION_HEAD,
        {.info={AZ_AVAIL_READY,AZ_PROV_NATIVE,1,ds_now()},.scope=AZ_STATUS_LOAD_SCOPE,
         .scope_id=1,.provider_id=1,.count=1},&head,&clock};
    if (ds_publish(2,1,0,ds_now(),&f,1,NULL,NULL,0,0)!=AZ_OK) return 3;
    sdk_status_ui_drain();
    head.seconds=1; head.samples=44100;
    if (ds_publish(2,1,DS_MOTION,ds_now(),&f,1,NULL,NULL,0,0)!=AZ_OK) return 4;
    sdk_status_ui_drain();
    /* Producers stop. The two actual timerfd expirations each drive a new UI
     * drain without a tick, sleep loop, further sample or mod getter. */
    struct pollfd timer={.fd=ds_timer_fd,.events=POLLIN};
    for (unsigned i=0;i<2;i++) {
        if (poll(&timer,1,1500)!=1 || !(timer.revents&POLLIN)) return 5;
        sdk_status_ui_drain();
    }
    head.seconds=9; head.samples=9*44100;
    if (ds_publish(2,1,DS_MOTION,ds_now(),&f,1,NULL,NULL,0,0)!=AZ_OK) return 6;
    head.seconds=20; head.samples=20*44100;
    az_status_occurrence_v1 seek={.size=sizeof seek,.version=AZ_DATA_VERSION,.kind=AZ_STATUS_SEEK,
                                   .endpoints=AZ_STATUS_OCCURRENCE_TO,.ordinal=1,.to=head};
    if (ds_publish(2,1,0,ds_now(),&f,1,&seek,NULL,0,0)!=AZ_OK) return 7;
    sdk_status_ui_drain();
    return poll(&timer,1,80)==0 ? 0 : 8;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage\n");
        return 2;
    }
    g_conf = azconf_new();
    if (g_conf) azconf_load(g_conf, CONF_PATH);
    if (!strcmp(argv[1], "vernewer") && argc > 3) {
        printf("%d\n", azcm_ver_newer(argv[2], argv[3]));
        return 0;
    }
    if (!strcmp(argv[1], "verdiffer") && argc > 3) {
        printf("%d\n", strcmp(argv[2], argv[3]) != 0);
        return 0;
    }   /* control: plain inequality */
    if (!strcmp(argv[1], "sha") && argc > 2) {
        char h[65];
        printf("%s\n", cm_sha256_file(argv[2], h) ? "ERR" : h);
        return 0;
    }
    if (!strcmp(argv[1], "shastr") && argc > 2) {
        char h[65];
        azsha256_hex(argv[2], strlen(argv[2]), h);
        printf("%s\n", h);
        return 0;
    }
    if (!strcmp(argv[1], "tag")) {
        cm_derive_fwtag();
        printf("%s\n", g_cm_fwtag);
        return 0;
    }
    if (!strncmp(argv[1], "anyloaded", 9)) return anyloaded_cmd(argc, argv);
    if (!strcmp(argv[1], "tagmp")) {                       /* the tag must not move when code pages are re-protected */
        cm_derive_fwtag();
        char a[32];
        snprintf(a, sizeof a, "%s", g_cm_fwtag);
        long pg = sysconf(_SC_PAGESIZE);
        uintptr_t p = ((uintptr_t) &main) & ~(uintptr_t) (pg - 1);
        mprotect((void *) p, (size_t) pg, PROT_READ | PROT_WRITE | PROT_EXEC);   /* what a hook install does */
        uintptr_t v = (uintptr_t) &tagmp_victim;                                   /* never runs: see its comment */
        mprotect((void *) v, (size_t) pg, PROT_READ | PROT_EXEC | PROT_WRITE);
        mprotect((void *) v, (size_t) pg, PROT_READ);                               /* and a mis-flagged page */
        g_cm_fwtag[0] = 0;
        g_cm_text_lo = g_cm_text_hi = 0;
        cm_derive_fwtag();
        printf("%s %s %s\n", a, g_cm_fwtag, strcmp(a, g_cm_fwtag) ? "MOVED" : "STABLE");
        return 0;
    }
    if (!strcmp(argv[1], "addrs")) {                       /* in-text addresses for the claim test */
        uintptr_t m = ((uintptr_t) &main + 64) & ~(uintptr_t) 3;
        printf("%lx %lx\n", (unsigned long) m, (unsigned long) (m + 64));
        return 0;
    }
    /* a core-owned hook inside our own text, so "already patched by AZ-MODS" can be tested */
    {
        uintptr_t m = ((uintptr_t) &main + 64) & ~(uintptr_t) 3;
        void *o;
        azm_install_hook(m + 64, NULL, &o);
    }
    g_cm_on_ui=1;
    community_init();
    for (int i = 0; i < azcm_count(); i++) {
        azcm_info_t in;
        azcm_info(i, &in);
        printf("MOD %s state=%d screen=%d hooks=%d name=%s| reason=%s\n", in.id, in.state, in.has_screen, in.hooks, in.name, in.reason);
        printf("NAME %s %s\n", in.id, in.name);
        printf("FROM %s %s\n", in.id, in.from[0] ? in.from : "-");
        if (g_cm[i].sha[0]) printf("SHA %s %s\n", in.id, g_cm[i].sha);
        for (int k = 0; k < 4; k++) {
            char kr[4] = {'r', (char) ('0' + k), 0, 0};
            pthread_mutex_lock(&g_cm_mu); /* as the services do: a mod's thread may be setting */
            int v = cm_cfg_get_u(in.id, kr, -7);
            pthread_mutex_unlock(&g_cm_mu);
            if (v != -7) printf("CLAIM %s %d %d\n", in.id, k, v);
        }
    }
    printf("SUSPENDED %d\nLIVE %d\nNEW %d\n", azcm_suspended(), g_cm_live, azcm_new_count());
    if (argc>2 && !strcmp(argv[2],"cadence")) return status_cadence_cmd();
    if (argc>2 && !strcmp(argv[2],"status")) {
        /* Same UI path used by the overlay, with no community_tick call. */
        int loaded=ds_publish(1,1,DS_BOUNDARY,ds_now(),NULL,0,NULL,NULL,0,0);
        if (loaded!=AZ_OK) return 3;
        for (int i=0;i<3;i++) {
            int engaged=i%2;
            struct ds_field field={AZ_STATUS_LOOP,AZ_STATUS_LOOP_ENGAGED,
                {.info={AZ_AVAIL_READY,AZ_PROV_NATIVE,i+1,ds_now()},.scope=AZ_STATUS_LOAD_SCOPE,
                 .scope_id=1,.provider_id=1,.count=1},&engaged, NULL};
            if (ds_publish(1,1,0,ds_now(),&field,1,NULL,NULL,0,0)!=AZ_OK) return 4;
        }
        struct pollfd wake={ds_event_fd,POLLIN,0};
        if (poll(&wake,1,0)!=1) return 5;
        sdk_status_ui_drain();
        return 0;
    }
    if (argc > 4 && !strcmp(argv[2], "allowx")) {         /* another writer (az-community) adds a line after this start read the list */
        FILE *f = fopen(CM_APPROVED, "a");
        if (f) {
            fprintf(f, "%s\n", argv[4]);
            fclose(f);
        }
        argv[2] = "allow";
    }
    if (argc > 3 && !strcmp(argv[2], "allow")) {          /* the DJ pressed Allow on a mod's page */
        int k = -1;
        for (int i = 0; i < azcm_count(); i++)
            if (!strcmp(g_cm[i].id, argv[3])) k = i;
        int rc = azcm_allow(k);
        azcm_info_t in;
        memset(&in, 0, sizeof in);
        if (k >= 0) azcm_info(k, &in);
        printf("ALLOW %d allowed=%d NEW %d\n", rc, in.allowed, azcm_new_count());
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "reload")) {
        azcm_reload_requested();
        return 0;
    }   /* the DJ pressed Reload */
    if (argc > 2 && !strcmp(argv[2], "paint")) {          /* each page paint, then ticks: a kept context must draw nothing */
        int painted = 0;
        for (int i = 0; i < azcm_count(); i++) {
            azcm_info_t in;
            if (azcm_info(i, &in) || in.state != AZCM_LOADED || !in.has_screen) continue;
            az_draw d;
            memset(&d, 0, sizeof d);
            az_rect a = {0, 0, 100, 100};
            azcm_screen_draw(i, &d, a);
            painted++;
        }
        int in_paint = g_headers;
        for (g_fake_now = 1; fake_time_within(1000); g_fake_now += 10) community_tick();
        printf("PAINT painted=%d in_paint=%d after_ticks=%d\n", painted, in_paint, g_headers - in_paint);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "selfcrash")) {      /* a fault in "EP147", not in a mod */
        volatile int *z = (volatile int *) 0;
        *z = 2;
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "crash")) {          /* tick until the crash mod faults */
        for (g_fake_now = 1; fake_time_within(5000); g_fake_now += 10) community_tick();
        printf("NO CRASH\n");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "run")) {            /* simulate 95 s of ticks */
        for (g_fake_now = 1; fake_time_within(95000); g_fake_now += 10) community_tick();
        printf("CFGWRITES %ld\n", g_cm_cfg_writes);
        printf("STARTING_EXISTS %d\n", access(CM_STARTING, F_OK) == 0);
        unsigned char fr[128] = {0};
        printf("FRAME %d byte0=%d\n", community_frame(AZ_BUS_ERP_LED, fr, 0x78), fr[0]);
        g_fake_now += 100;
        community_tick();             /* the tick after: on_frame's queued log lines */
        g_fake_now += 6000;
        community_tick();            /* past the settings rate limit */
    }
    return 0;
}
