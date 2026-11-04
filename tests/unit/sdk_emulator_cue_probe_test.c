#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
static uint64_t fake_now = UINT64_C(1000000000);
static uint64_t test_now(void) { return ++fake_now; }
#define CUE_TEST_NOW test_now
#ifndef CUE_PROBE_SOURCE
#define CUE_PROBE_SOURCE "../../../sdk/examples/cue-command-probe/probe.c"
#endif
#include CUE_PROBE_SOURCE
#include <assert.h>
static char directory[256];
static az_command_request submitted;
static unsigned submits, polls, releases, cue_reads, release_busy, cap_reads, cancels;
static uint64_t caps_delay, actual_submit_clock;
static int immediate, hold_poll, cancelled;
static az_command_completion_fn completion;
static void *completion_user;
static az_command_result terminal_receipt;
static unsigned acknowledgements;
static void (*posted)(void *);
static void *posted_user;
static void post_job(const char *id, void (*fn)(void *), void *user) {
    assert(!strcmp(id, ID) && !posted);
    posted = fn;
    posted_user = user;
}
static int acknowledge(const char *id, uint64_t request) {
    assert(!strcmp(id, ID) && request == submitted.request_id);
    acknowledgements++;
    return AZ_OK;
}
static void native_delivery(void) {
    if (!cue.pending) return;
    az_command_completion event = {.size = sizeof event, .version = AZ_DATA_VERSION, .settled = 1};
    if (!cue.terminal) {
        if (hold_poll && !cancelled) return;
        event.kind = AZ_COMMAND_COMPLETION_TERMINAL;
        event.result = (az_command_result) {sizeof(az_command_result), AZ_DATA_VERSION, cancelled ? AZ_COMMAND_CANCELLED : immediate ? AZ_COMMAND_REJECTED
                                                                                                                                     : AZ_COMMAND_APPLIED,
                                            cancelled ? AZ_CANCELLED : immediate ? AZ_STALE
                                                                                 : AZ_OK,
                                            submitted.request_id, cancelled || immediate ? 0 : 1, test_now()};
        terminal_receipt = event.result;
        completion(&event, completion_user);
    }
    if (release_busy) {
        release_busy--;
        return;
    }
    event.kind = AZ_COMMAND_COMPLETION_RELEASED;
    event.result = terminal_receipt;
    completion(&event, completion_user);
}
static void test_step(void) {
    native_delivery();
    cue_worker_step();
    if (posted) {
        void (*fn)(void *) = posted;
        posted = NULL;
        fn(posted_user);
    }
    cue_step();
    cue_flush();
}
#define cue_step test_step
static void log_message(const char *id, const char *fmt, ...) {
    (void) id;
    (void) fmt;
}
static const char *data_dir(const char *id) {
    assert(!strcmp(id, ID));
    return directory;
}
static int caps(az_data_capabilities *c) {
    c->version = AZ_DATA_VERSION;
    cap_reads++;
    fake_now += caps_delay;
    caps_delay = 0;
    c->commands = (UINT64_C(1) << 27) - 2;
    return AZ_OK;
}
static int snapshot(int d, az_deck_snapshot *s) {
    s->version = AZ_DATA_VERSION;
    s->deck = (uint32_t) d;
    s->valid = AZ_LIVE_NATIVE_LOAD;
    s->native_load_generation = 7;
    s->native_load_unit_generation = 11;
    s->native_load_observed_ns = 10;
    return AZ_OK;
}
static int cues(int d, az_native_cue_snapshot *s) {
    cue_reads++;
    s->version = AZ_DATA_VERSION;
    s->deck = (uint32_t) d;
    s->native_load_generation = 7;
    s->revision = 9;
    s->count = 8;
    for (unsigned i = 0; i < 8; i++) {
        s->entries[i].classification = AZ_CUE_HOT;
        s->entries[i].entry_id = i + 1;
        s->entries[i].slot = i;
        s->entries[i].valid = i < 6 ? AZ_CUE_START : 0;
        s->entries[i].start_s = i;
    }
    return AZ_OK;
}
static int submit(const char *id, const az_command_request *q, az_command_completion_fn fn, void *user, az_command_result *r) {
    assert(!strcmp(id, ID));
    completion = fn;
    completion_user = user;
    submits++;
    actual_submit_clock = fake_now;
    submitted = *q;
    r->version = AZ_DATA_VERSION;
    r->request_id = q->request_id;
    r->outcome = immediate ? AZ_COMMAND_REJECTED : AZ_COMMAND_ACCEPTED;
    r->reason = immediate ? AZ_STALE : 0;
    return AZ_OK;
}
static int poll(const char *id, uint64_t request, az_command_result *r) {
    (void) id;
    (void) request;
    (void) r;
    polls++;
    assert(!"command status polling is forbidden");
    return AZ_FAILED;
}
static int cancel_command(const char *id, uint64_t request, az_command_result *r) {
    assert(!strcmp(id, ID) && request == submitted.request_id);
    cancels++;
    cancelled = 1;
    r->version = AZ_DATA_VERSION;
    r->request_id = request;
    r->outcome = AZ_COMMAND_CANCELLED;
    r->reason = AZ_CANCELLED;
    r->observed_ns = test_now();
    return AZ_OK;
}
static int release(const char *id, uint64_t request) {
    (void) id;
    (void) request;
    releases++;
    assert(!"legacy release is forbidden");
    return AZ_FAILED;
}
static void reset(void) {
    static az_core_v1 c;
    static az_library_v1 l;
    memset(&cue, 0, sizeof cue);
    memset(&l, 0, sizeof l);
    c.data_dir = data_dir;
    c.log = log_message;
    c.post_ui = post_job;
    l.data_capabilities = caps;
    l.deck_snapshot = snapshot;
    l.native_cues = cues;
    l.command_submit_async = submit;
    l.command_acknowledge = acknowledge;
    l.size = sizeof l;
    l.abi = AZ_LIBRARY_ABI;
    l.command_poll = poll;
    l.command_cancel = cancel_command;
    l.command_release = release;
    cue.core = &c;
    cue.lib = &l;
    submits = polls = releases = cancels = acknowledgements = 0;
    atomic_init(&cue.io_failed, 0);
    atomic_init(&cue.ui_ready, 0);
    atomic_init(&cue_job.busy, 0);
    atomic_init(&cue_logs.written, 0);
    atomic_init(&cue_logs.consumed, 0);
    posted = NULL;
    immediate = hold_poll = cancelled = 0;
    release_busy = 0;
    caps_delay = 0;
}
static void lifecycle(unsigned op, unsigned flags, unsigned classification, unsigned entry, uint64_t revision) {
    reset();
    struct cue_input v = {.pid = (uint64_t) getpid(), .id = 19, .deck = 2, .op = op, .load = 7, .revision = revision, .timeout = UINT64_C(10000000000), .flags = flags, .classification = classification, .entry = entry, .rgb = 0x123456};
    strcpy(v.action, "submit");
    strcpy(v.comment, "unit UTF8 \342\234\223");
    assert(cue_handle(&v) == AZ_OK && cue.pending);
    assert(submitted.operation == op && submitted.flags == flags && submitted.classification == classification && submitted.entry_id == entry && submitted.expected_revision == revision && submitted.rgb == 0x123456 && !strcmp(submitted.comment, v.comment));
    cue_step();
    assert(!cue.pending && submits == 1 && polls == 0 && releases == 0 && acknowledgements == 1);
    assert(cue_handle(&v) == AZ_STALE && submits == 1);
}
static void worker_lease(void) {
    uint64_t saved_clock = fake_now;
    reset();
    uint64_t start = fake_now;
    assert(cue_worker_begin(start));
    struct cue_input v = {.pid = (uint64_t) getpid()};
    strcpy(v.action, "state");
    uint64_t original_idle = cue.worker_idle_deadline_ns;
    fake_now = start + CUE_IDLE_NS - 100;
    assert(cue_worker_continue(fake_now));
    v.flags = 1;
    assert(cue_handle(&v) == AZ_INVALID);
    assert(compare_timestamps(cue.worker_idle_deadline_ns, original_idle, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    v.flags = 0;
    assert(cue_handle(&v) == AZ_OK);
    assert(compare_timestamps(cue.worker_idle_deadline_ns, original_idle, 1000000000, 1000000000, 1) == AZ_TIME_AFTER);
    assert(cue_worker_continue(start + CUE_IDLE_NS + 1));
    fake_now = start + CUE_LIFETIME_NS - 100;
    assert(cue_handle(&v) == AZ_OK);
    assert(!cue_worker_continue(start + CUE_LIFETIME_NS));
    reset();
    assert(!cue_worker_begin(0) && !cue_worker_begin(UINT64_MAX));
    assert(cue_worker_begin(fake_now));
    v = (struct cue_input) {.pid = (uint64_t) getpid(), .id = 1, .op = AZ_CMD_MAIN_SET, .load = 7, .timeout = UINT64_C(10000000000), .flags = 2};
    strcpy(v.action, "submit");
    assert(cue_handle(&v) == AZ_OK && cue.pending);
    uint64_t deadline = cue.active.deadline;
    cue.worker_idle_deadline_ns = fake_now + 1;
    fake_now += 2;
    assert(cue_worker_continue(fake_now));
    uint64_t drain = cue.worker_drain_deadline_ns;
    cue_note_activity(fake_now + 1);
    assert(compare_timestamps(cue.worker_drain_deadline_ns, drain, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(cue.active.deadline, deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    release_busy = 1;
    cue_step();
    assert(cue.pending && cue.terminal);
    assert(cue_worker_continue(fake_now));
    cue_step();
    assert(!cue.pending && !cue_worker_continue(fake_now));
    reset();
    assert(cue_worker_begin(fake_now));
    cue.pending = 1;
    cue.io_failed = 1;
    assert(cue_worker_continue(fake_now));
    drain = cue.worker_drain_deadline_ns;
    assert(!cue_worker_continue(drain) && cue.pending); // Bounded failure remains explicit.
    reset();
    assert(cue_worker_begin(fake_now));
    cue.sequence = 8192;
    assert(!cue_worker_continue(fake_now));
    fake_now = saved_clock;
    reset();
}
int main(void) {
    snprintf(directory, sizeof directory, "/tmp/sdk-cue-probe-test-%ld", (long) getpid());
    assert(mkdir(directory, 0700) == 0);
    char p[512];
    snprintf(p, sizeof p, "%s/results.%ld.log", directory, (long) getpid());
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    close(fd);
    worker_lease();
    reset();
    cap_reads = 0;
    cue_worker_step();
    assert(!cap_reads && !submits && !polls);
    assert(mod.tick == cue_tick);
    mod.tick();
    cue_worker_step();
    assert(cap_reads == 1 && !submits && !polls);
    for (unsigned i = 1; i <= 8; i++) lifecycle(AZ_CMD_HOT_TRIGGER, AZ_COMMAND_NATIVE_LOAD, AZ_CUE_HOT, i, 0);
    lifecycle(AZ_CMD_MAIN_SET, AZ_COMMAND_NATIVE_LOAD, AZ_CUE_MAIN, 0, 0);
    lifecycle(AZ_CMD_MAIN_RECALL, AZ_COMMAND_NATIVE_LOAD, AZ_CUE_MAIN, 0, 0);
    lifecycle(AZ_CMD_CUE_COLOR, AZ_COMMAND_NATIVE_LOAD | AZ_COMMAND_NATIVE_CUE_IDS | AZ_COMMAND_PERSIST, AZ_CUE_MEMORY, 4, 55);
    struct cue_input v;
    char input[1024];
    int n = snprintf(input, sizeof input, "v=1 pid=%ld action=submit id=1 deck=0 op=25 load=7 revision=9 timeout_ns=10000000000 flags=7 classification=2 entry_id=3 position=0 end=0 beats=0 rgb=0 comment_hex=68656c6c6f20e29c93\n", (long) getpid());
    assert(cue_parse(input, (size_t) n, &v) && !strcmp(v.comment, "hello \342\234\223") && v.revision == 9 && v.flags == 7);
    assert(!cue_parse(input, (size_t) n - 1, &v));
    input[n - 2] = 'g';
    assert(!cue_parse(input, (size_t) n, &v));
    char comment[128];
    assert(!cue_comment("00", comment) && !cue_comment("a", comment) && !cue_comment("zz", comment));
    double d;
    assert(!cue_number("nan", &d) && !cue_number("inf", &d) && !cue_number("0x1p0", &d));
    reset();
    immediate = 1;
    memset(&v, 0, sizeof v);
    v.pid = (uint64_t) getpid();
    v.id = 1;
    v.op = AZ_CMD_MAIN_SET;
    v.load = 7;
    v.timeout = 10000000000;
    v.flags = 2;
    strcpy(v.action, "submit");
    assert(cue_handle(&v) == AZ_OK && !cue.terminal);
    cue_step();
    assert(acknowledgements == 1 && !polls && !releases && !cue.pending);
    reset();
    memset(&v, 0, sizeof v);
    v.pid = (uint64_t) getpid();
    strcpy(v.action, "state");
    unsigned prior = cue_reads;
    fd = open(p, O_RDONLY);
    assert(fd >= 0);
    off_t fence = lseek(fd, 0, SEEK_END);
    uint64_t before_clock = fake_now;
    assert(cue_handle(&v) == AZ_OK && cue_reads == prior && !submits && compare_timestamps(fake_now, before_clock, 1000000000, 1000000000, 1) == AZ_TIME_AFTER);
    char state_row[2048], stamp[64];
    cue_flush();
    ssize_t got = pread(fd, state_row, sizeof state_row - 1, fence);
    assert(got > 0);
    state_row[got] = 0;
    snprintf(stamp, sizeof stamp, "query_ns=%llu ", (unsigned long long) fake_now);
    assert(strstr(state_row, stamp) && strstr(state_row, "cue_count=0\n") && !strstr(state_row, "SDK_CUE_ENTRY"));
    close(fd);
    strcpy(v.action, "snapshot");
    assert(cue_handle(&v) == AZ_OK && cue_reads == prior + 1 && !submits);
    reset();
    memset(&v, 0, sizeof v);
    v.pid = (uint64_t) getpid();
    v.id = 91;
    v.op = AZ_CMD_MAIN_SET;
    v.load = 7;
    v.timeout = UINT64_C(10000000000);
    v.flags = 2;
    strcpy(v.action, "submit");
    uint64_t transport_start = fake_now;
    caps_delay = UINT64_C(7000000000);
    assert(cue_handle(&v) == AZ_OK);
    assert(compare_timestamps(actual_submit_clock, transport_start + UINT64_C(7000000000), 1000000000, 1000000000, 1) >= AZ_TIME_EQUAL);
    assert(compare_timestamps(submitted.deadline_ns, actual_submit_clock + UINT64_C(10000000000), 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    assert(compare_timestamps(cue.active.submitted_ns, actual_submit_clock, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL && compare_timestamps(cue.active.deadline - cue.active.submitted_ns, UINT64_C(10000000000), 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    uint64_t fixed_deadline = cue.active.deadline;
    release_busy = 2;
    fake_now += UINT64_C(3000000000);
    cue_step();
    assert(cue.pending && cue.terminal && acknowledgements == 1 && compare_timestamps(cue.active.deadline, fixed_deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    fake_now += UINT64_C(3000000000);
    cue_step();
    assert(cue.pending && cue.terminal && acknowledgements == 1 && compare_timestamps(cue.active.deadline, fixed_deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    fake_now += UINT64_C(3000000000);
    cue_step();
    assert(!cue.pending && acknowledgements == 1 && compare_timestamps(cue.active.deadline, fixed_deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    reset();
    hold_poll = 1;
    memset(&v, 0, sizeof v);
    v.pid = (uint64_t) getpid();
    v.id = 92;
    v.op = AZ_CMD_MAIN_SET;
    v.load = 7;
    v.timeout = UINT64_C(10000000000);
    v.flags = 2;
    strcpy(v.action, "submit");
    assert(cue_handle(&v) == AZ_OK && cue.pending && !cue.terminal);
    char request_path[512];
    snprintf(request_path, sizeof request_path, "%s/request", directory);
    fd = open(request_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    n = snprintf(input, sizeof input, "v=1 pid=%ld action=cancel id=92 deck=0 op=0 load=0 revision=0 timeout_ns=0 flags=0 classification=0 entry_id=0 position=0 end=0 beats=0 rgb=0 comment_hex=-\n", (long) getpid());
    assert(write(fd, input, (size_t) n) == n);
    close(fd);
    atomic_store(&cue.ui_ready, 1);
    cue_step();
    assert(cancels == 1 && cue.pending && cue.active.id == 92 && compare_timestamps(submitted.deadline_ns, cue.active.deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    cue_step();
    assert(!cue.pending && acknowledgements == 1 && !releases && access(request_path, F_OK) != 0);
    reset();
    memset(&v, 0, sizeof v);
    v.pid = (uint64_t) getpid();
    v.id = 93;
    v.op = AZ_CMD_MAIN_SET;
    v.load = 7;
    v.timeout = UINT64_C(30000000001);
    v.flags = 2;
    strcpy(v.action, "submit");
    assert(cue_handle(&v) == AZ_INVALID && !submits);
    v.timeout = UINT64_C(30000000000);
    assert(cue_handle(&v) == AZ_OK && submits == 1);
    cue_step();
    assert(!cue.pending);
    unlink(p);
    rmdir(directory);
    puts("PASS cue probe parser, allHotIDs1..8, genericmetadata, pending cancellation and terminal release");
    return 0;
}
