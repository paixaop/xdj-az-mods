/* SPDX-License-Identifier: GPL-3.0-only */
/* Independent C native-message layout, protected lifetime, and caller threads. */
#include "native_abi_fixture.h"
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#define REQUIRE(x, why)                                                                   \
    do {                                                                                  \
        if (!(x)) {                                                                       \
            fprintf(stderr, "FAIL cache-message: %s (%s:%d)\n", why, __FILE__, __LINE__); \
            abort();                                                                      \
        }                                                                                 \
    } while (0)
#ifndef AZCACHE_MESSAGE_HEADER
#define AZCACHE_MESSAGE_HEADER "../../src/common/azcachemessage.h"
#endif
#if __has_include(AZCACHE_MESSAGE_HEADER)
#include AZCACHE_MESSAGE_HEADER
#define MESSAGE_IMPLEMENTATION 1
#else
#include "../../src/common/azcachebirth.h"
#define MESSAGE_IMPLEMENTATION 0
struct azcm_borrow {
    unsigned build;
    const void *server, *message;
    uint32_t parameter;
};
struct azcm_envelope {
    uint16_t kind;
    uint32_t sequence, parameter, present, fields[8];
    unsigned client, selector;
};
static void azcm_forward_owned(const struct azcm_borrow *b, struct azcb_forward f) {
    (void) b;
    f.call(f.context);
}
static bool azcm_current(const void *s, struct azcm_envelope *e) {
    (void) s;
    (void) e;
    return false;
}
#endif
static unsigned profile;
struct Argument {
    uint32_t type, reserved, value;
    unsigned char pad[36];
} __attribute__((aligned(8)));
struct Arguments {
    struct Argument *begin, *end, *capacity;
};
struct Message {
    uint16_t kind, reserved;
    uint32_t magic, sequence, reserved_c;
    uint64_t reserved_10, reserved_18, reserved_20;
    struct Arguments arguments;
};
struct Owned {
    void *page;
    struct Message *message;
    bool destroyed;
};
static void resize(struct Message *m, size_t n) {
    size_t old = m->arguments.begin ? (size_t) (m->arguments.end - m->arguments.begin) : 0;
    struct Argument *p = calloc(n, sizeof *p);
    REQUIRE(p, "argument allocation");
    if (old) memcpy(p, m->arguments.begin, (old < n ? old : n) * sizeof *p);
    for (size_t i = old; i < n; i++) p[i].type = 1;
    free(m->arguments.begin);
    m->arguments = (struct Arguments) {p, p + n, p + n};
}
static void owned_init(struct Owned *o, unsigned client, unsigned selector, unsigned content, uint32_t seq) {
    *o = (struct Owned) {0};
    o->page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(o->page != MAP_FAILED, "native-shaped owned message allocation");
    o->message = o->page;
    *o->message = (struct Message) {.kind = 0x2102, .magic = 0x872349ae, .sequence = seq};
    resize(o->message, 2);
    o->message->arguments.begin[0].value = (client << 24) | (selector << 16);
    o->message->arguments.begin[1].value = content;
    REQUIRE(offsetof(struct Message, arguments) == 0x28, "native vector member offset");
}
static void owned_protect(struct Owned *o) {
    free(o->message->arguments.begin);
    o->destroyed = true;
    REQUIRE(!mprotect(o->page, 4096, PROT_NONE), "native handler frees/protects input before return");
}
static void owned_drop(struct Owned *o) {
    if (!o->destroyed) free(o->message->arguments.begin);
    REQUIRE(!munmap(o->page, 4096), "message mapping cleanup");
}
static void absent(const void *server) {
    struct azcm_envelope e = {.kind = 0xfedc, .fields = {[7] = 0xabcdef}};
    REQUIRE(!azcm_current(server, &e) && e.kind == 0xfedc && e.fields[7] == 0xabcdef, "absent scope leaves output unchanged");
}
struct Four {
    int *server;
    struct Owned *owned;
    unsigned i, client, forwards;
};
static void four_forward(void *opaque) {
    struct Four *c = opaque;
    ++c->forwards;
    REQUIRE(c->owned->message->kind == 0x2102 && c->owned->message->arguments.begin[1].value == 101 + c->i, "native sees unchanged original message and arguments");
    printf("NATIVE_MESSAGE_FORWARD_OK build=%u context=%u implementation=%d\n", profile, c->i, MESSAGE_IMPLEMENTATION);
    fflush(stdout);
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(c->server, &e), "missing owned pre-forward message scalar snapshot");
    REQUIRE(e.kind == 0x2102 && e.sequence == UINT32_MAX - c->i && e.parameter == 0x80000102u + c->i && e.present == 3 && e.fields[1] == 101 + c->i && e.client == c->client && e.selector == c->i + 1, "copied supported scalar context is exact");
    int foreign;
    absent(&foreign);
    owned_protect(c->owned);
    struct azcm_envelope after = {0};
    REQUIRE(azcm_current(c->server, &after) && after.fields[1] == 101 + c->i, "snapshot never follows message after native destruction");
}
static void four_contexts(void) {
    int servers[4] = {0};
    const unsigned clients[] = {0, 3, 1, 2};
    for (unsigned i = 0; i < 4; i++) {
        struct Owned o __attribute__((cleanup(owned_drop)));
        owned_init(&o, clients[i], i + 1, 101 + i, UINT32_MAX - i);
        absent(&servers[i]);
        struct Four c = {&servers[i], &o, i, clients[i], 0};
        struct azcm_borrow b = {profile, &servers[i], o.message, 0x80000102u + i};
        azcm_forward_owned(&b, (struct azcb_forward) {four_forward, &c});
        REQUIRE(c.forwards == 1 && o.destroyed, "original handler forwarded exactly once");
        absent(&servers[i]);
    }
}
struct Nested {
    int server;
    struct azcm_borrow inner;
};
static void nested_inner(void *opaque) {
    struct Nested *c = opaque;
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(&c->server, &e) && e.fields[1] == 22, "nested frame distinct");
}
static void raise_native(void *opaque) {
    (void) opaque;
    fixture_raise();
}
static void nested_throw(void *opaque) {
    struct Nested *c = opaque;
    azcm_forward_owned(&c->inner, (struct azcb_forward) {raise_native, NULL});
}
static void nested_outer(void *opaque) {
    struct Nested *c = opaque;
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(&c->server, &e) && e.fields[1] == 11, "outer frame visible");
    azcm_forward_owned(&c->inner, (struct azcb_forward) {nested_inner, c});
    REQUIRE(azcm_current(&c->server, &e) && e.fields[1] == 11, "normal nesting restores outer scalar frame");
    unsigned before = fixture_exception_deletions;
    REQUIRE(!az_native_try(nested_throw, c) && fixture_exception_deletions == before + 1 && azcm_current(&c->server, &e) && e.fields[1] == 11, "native unwind propagates and restores exact outer frame");
}
static void nested_and_exception(void) {
    struct Owned outer __attribute__((cleanup(owned_drop))), inner __attribute__((cleanup(owned_drop)));
    owned_init(&outer, 0, 2, 11, 9);
    owned_init(&inner, 3, 8, 22, 10);
    struct Nested c = {0};
    c.inner = (struct azcm_borrow) {profile, &c.server, inner.message, 2};
    struct azcm_borrow a = {profile, &c.server, outer.message, 1};
    azcm_forward_owned(&a, (struct azcb_forward) {nested_outer, &c});
    absent(&c.server);
}
struct Rejected {
    const void *server;
    unsigned count;
};
static void rejected_forward(void *opaque) {
    struct Rejected *r = opaque;
    ++r->count;
    absent(r->server);
}
static void rejected(struct azcm_borrow *b) {
    struct Rejected r = {b->server, 0};
    azcm_forward_owned(b, (struct azcb_forward) {rejected_forward, &r});
    REQUIRE(r.count == 1, "unsupported view still forwards native work");
}
static void canonical_and_bounds(void) {
    int server;
    struct Owned o __attribute__((cleanup(owned_drop)));
    owned_init(&o, 0, 1, 11, 0);
    struct azcm_borrow b = {profile, &server, o.message, 2};
    o.message->kind = 0x9999;
    rejected(&b);
    o.message->kind = 0x2102;
    o.message->magic = 0;
    rejected(&b);
    o.message->magic = 0x872349ae;
    o.message->arguments.begin[1].type = 2;
    rejected(&b);
    o.message->arguments.begin[1].type = 1;
    resize(o.message, 1);
    rejected(&b);
    resize(o.message, 2);
    o.message->arguments.begin[0].value = 0;
    rejected(&b);
    o.message->arguments.begin[0].value = 10u << 16;
    rejected(&b);
    o.message->arguments.begin[0].value = 1u << 16;
    b.build = 0;
    rejected(&b);
    b.build = profile;
    uintptr_t saved[3];
    memcpy(saved, (char *) o.message + 0x28, sizeof saved);
    const uintptr_t bad[][3] = {{saved[0] + 1, saved[1], saved[2]}, {saved[1], saved[0], saved[2]}, {saved[0], saved[1] + 1, saved[2]}, {saved[0], saved[1], saved[0]}, {saved[0], saved[0] + 48u * 4097u, saved[0] + 48u * 4097u}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy((char *) o.message + 0x28, bad[i], sizeof bad[i]);
        rejected(&b);
    }
    memcpy((char *) o.message + 0x28, saved, sizeof saved);
}
struct Data {
    int server;
    uint32_t sequence;
};
static void data_forward(void *opaque) {
    struct Data *c = opaque;
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(&c->server, &e) && e.sequence == c->sequence && e.present == 0x8f && e.fields[1] == 17 && e.fields[2] == 4 && e.fields[3] == 0x1234 && e.fields[7] == 0xabcd, "copy raw wrapped sequence and bounded canonical data fields without inventing identity");
}
static void data_request_and_wrap(void) {
    struct Owned o __attribute__((cleanup(owned_drop)));
    owned_init(&o, 2, 9, 0, UINT32_MAX);
    o.message->kind = 0x3000;
    resize(o.message, 8);
    o.message->arguments.begin[1].value = 17;
    o.message->arguments.begin[2].value = 4;
    o.message->arguments.begin[3].value = 0x1234;
    o.message->arguments.begin[7].value = 0xabcd;
    uint32_t seq[] = {UINT32_MAX, 0};
    for (unsigned i = 0; i < 2; i++) {
        struct Data c = {0, seq[i]};
        o.message->sequence = seq[i];
        struct azcm_borrow b = {profile, &c.server, o.message, 77};
        azcm_forward_owned(&b, (struct azcb_forward) {data_forward, &c});
    }
}
struct Depth {
    int *server;
    struct Message *message;
    unsigned depth;
};
static void recurse(struct Depth *c);
static void depth_forward(void *opaque) {
    struct Depth *c = opaque;
    struct azcm_envelope e = {0};
    if (c->depth <= 32) REQUIRE(azcm_current(c->server, &e) && e.parameter == c->depth, "supported nested frame retains its own scalar parameter");
    else absent(c->server);
    if (c->depth < 50) {
        struct Depth next = {c->server, c->message, c->depth + 1};
        recurse(&next);
    }
    if (c->depth <= 32) REQUIRE(azcm_current(c->server, &e) && e.parameter == c->depth, "overflow scope never exposes or loses the outer frame");
    else absent(c->server);
}
static void recurse(struct Depth *c) {
    struct azcm_borrow b = {profile, c->server, c->message, c->depth};
    azcm_forward_owned(&b, (struct azcb_forward) {depth_forward, c});
}
static void bounded_nesting(void) {
    int server;
    struct Owned o __attribute__((cleanup(owned_drop)));
    owned_init(&o, 0, 1, 1, 1);
    struct Depth c = {&server, o.message, 1};
    recurse(&c);
    absent(&server);
}
struct Thread {
    int server;
    unsigned i;
};
static void thread_forward(void *opaque) {
    struct Thread *c = opaque;
    struct azcm_envelope e = {0};
    REQUIRE(azcm_current(&c->server, &e) && e.fields[1] == 31 + c->i, "separate actual caller threads cannot borrow another frame");
}
static void *thread_run(void *opaque) {
    struct Thread *c = opaque;
    struct Owned o __attribute__((cleanup(owned_drop)));
    owned_init(&o, c->i, c->i + 1, 31 + c->i, c->i);
    struct azcm_borrow b = {profile, &c->server, o.message, c->i};
    azcm_forward_owned(&b, (struct azcb_forward) {thread_forward, c});
    absent(&c->server);
    return NULL;
}
static void thread_isolation(void) {
    pthread_t threads[4];
    struct Thread contexts[4];
    for (unsigned i = 0; i < 4; i++) {
        contexts[i] = (struct Thread) {0, i};
        REQUIRE(!pthread_create(&threads[i], NULL, thread_run, &contexts[i]), "caller thread create");
    }
    for (unsigned i = 0; i < 4; i++) REQUIRE(!pthread_join(threads[i], NULL), "caller thread join");
}
int main(int argc, char **argv) {
    _Static_assert(sizeof(struct Argument) == 48 && sizeof(struct Message) == 0x40, "native message/argument prefixes");
    REQUIRE(argc == 2 && (argv[1][0] == '1' || argv[1][0] == '2'), "separate fixture profile process");
    profile = (unsigned) (argv[1][0] - '0');
    four_contexts();
    nested_and_exception();
    canonical_and_bounds();
    data_request_and_wrap();
    bounded_nesting();
    thread_isolation();
    printf("PASS cache-message build=%u: owned scalar copy, original cleanup, nesting/unwind restoration and independent threads\n", profile);
}
