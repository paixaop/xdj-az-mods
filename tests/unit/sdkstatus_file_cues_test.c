/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse the native memory fixture's boundary stubs and ownership accounting.
 * The FILE hooks below are the unchanged production definitions it includes. */
#define main memory_fixture_main
#include "sdkstatus_memory_test.c"
#undef main

extern uintptr_t cue_call(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
extern char cue_return;
__asm__(".text\n.p2align 2\n.global cue_call\n.type cue_call,%function\n"
        "cue_call:\nstp x29,x30,[sp,#-16]!\nmov x29,sp\nmov x9,x0\n"
        "mov x0,x1\nmov x1,x2\nmov x2,x3\nmov x3,x4\nmov x4,x5\nmov x5,x6\nmov x8,x2\nblr x9\n"
        ".global cue_return\ncue_return:\nldp x29,x30,[sp],#16\nret\n.size cue_call,.-cue_call\n");

static struct {
    Object query, reply, ticket, accessor, identity, storage, handle;
    Object atom[2], control[2], merged, vector, block, bytes, payload, decoded, decode_block, content, rows;
    uintptr_t list[1];
    FileCueProfile selected;
    unsigned get_calls, ticket_calls, append_calls, publish_calls;
    bool cached, corrupt, reject_append, foreign_decode, foreign_append, imported;
    unsigned hot;
} cf;

static uintptr_t cue_test_issue(uintptr_t ticket) {
    assert(ticket == (uintptr_t) cf.ticket.bytes);
    return (uintptr_t) cf.query.bytes;
}
static int cue_test_merge(uintptr_t accessor, uintptr_t first, uintptr_t second, unsigned kind, uintptr_t output) {
    assert(accessor == (uintptr_t) cf.accessor.bytes && !first && !second && kind == 1);
    put(output, (uintptr_t) cf.vector.bytes);
    return 0;
}
static int cue_test_get(uintptr_t accessor, uintptr_t identity, uintptr_t output, uintptr_t count) {
    ++cf.get_calls;
    assert(accessor == (uintptr_t) cf.accessor.bytes && identity == (uintptr_t) cf.identity.bytes);
    uintptr_t cached[2] = {0};
    input_cache_object = (uintptr_t) cf.atom[0].bytes;
    input_cache_control = (uintptr_t) cf.control[0].bytes;
    input_cache_fresh = !cf.cached;
    assert(!file_cache_get(101, (uintptr_t) cached, 202));
    uintptr_t vector = 0;
    assert(!cue_call((void *) file_merge, accessor, 0, 0, 1, (uintptr_t) &vector, 0));
    assert(vector == (uintptr_t) cf.vector.bytes);
    if (!cf.cached) {
        input_opens = 0;
        int error = 0;
        assert(!file_input_open((uintptr_t) cf.handle.bytes, 0, 1, &error));
        assert(!file_input_open((uintptr_t) cf.handle.bytes, 0, 1, &error));
    }
    put(output, (uintptr_t) cf.block.bytes);
    put(count, (uintptr_t) 1);
    return 0;
}
static uintptr_t cue_test_reply(uintptr_t server, uintptr_t packet, unsigned peer) {
    assert(server == 303 && packet == (uintptr_t) cf.reply.bytes && !peer);
    return 77;
}
static void cue_test_serve(uintptr_t server, uintptr_t request, unsigned peer, uintptr_t identity) {
    assert(server == 303 && object_word(request) == (uintptr_t) cf.query.bytes && !peer);
    uintptr_t output = 0, count = 0;
    assert(!cue_call((void *) file_get, (uintptr_t) cf.accessor.bytes, identity, (uintptr_t) &output, (uintptr_t) &count, 0, 0));
    assert(output == (uintptr_t) cf.block.bytes && count == 1);
    assert(file_reply(server, (uintptr_t) cf.reply.bytes, peer) == 77);
}
typedef struct CueDecoded {
    unsigned char bytes[0x80];
} CueDecoded;
static CueDecoded cue_test_decode(uintptr_t block, unsigned rate) {
    assert(block == (uintptr_t) cf.decode_block.bytes && rate == 48000);
    CueDecoded result = {{0}};
    memcpy(result.bytes, cf.decoded.bytes, sizeof result.bytes);
    return result;
}
static unsigned cue_test_append(uintptr_t content, uintptr_t row) {
    ++cf.append_calls;
    assert(content == (uintptr_t) cf.content.bytes && row == (uintptr_t) cf.rows.bytes);
    if (cf.reject_append) return 0;
    put(content + 0x28, row);
    put(content + 0x30, row + 0x80);
    return 1;
}
static void cue_test_publish(uintptr_t ticket, uintptr_t memory, uintptr_t hot, uintptr_t result) {
    ++cf.publish_calls;
    assert(ticket == (uintptr_t) cf.ticket.bytes && !memory && !hot && SCALAR(unsigned, result) == 1);
}
static uintptr_t cue_test_ticket(uintptr_t ticket) {
    ++cf.ticket_calls;
    cf.imported = file_import != NULL;
    if (cf.corrupt) cf.bytes.bytes[8] ^= 1;
    memcpy(cf.payload.bytes, cf.bytes.bytes + 4, 12);
    if (cf.foreign_decode) azn_file_decode((uintptr_t) cf.decode_block.bytes, 48000, (uintptr_t) cf.rows.bytes);
    else cue_call((void *) azn_file_decode_entry, (uintptr_t) cf.decode_block.bytes, 48000, (uintptr_t) cf.rows.bytes, 0, 0, 0);
    unsigned accepted;
    if (cf.foreign_append) accepted = file_append_0((uintptr_t) cf.content.bytes, (uintptr_t) cf.rows.bytes);
    else accepted = (unsigned) cue_call(cf.hot ? (void *) file_append_1 : (void *) file_append_0,
                                        (uintptr_t) cf.content.bytes, (uintptr_t) cf.rows.bytes, 0, 0, 0, 0);
    assert(accepted == !cf.reject_append);
    unsigned result = 1;
    cue_call((void *) publish_import, ticket, 0, 0, (uintptr_t) &result, 0, 0);
    if (cf.corrupt) cf.bytes.bytes[8] ^= 1;
    return 88;
}
static void cue_test_message(Object *message, unsigned kind, uint32_t seq) {
    memset(message, 0, sizeof *message);
    put((uintptr_t) message->bytes, (uint16_t) kind);
    put((uintptr_t) message->bytes + 4, UINT32_C(0x872349ae));
    put((uintptr_t) message->bytes + 8, seq);
}
static void cue_test_route(uint32_t seq) {
    cue_test_message(&cf.query, 0x2b04, seq);
    cue_test_message(&cf.reply, 0x4e02, seq);
    uintptr_t ticket = (uintptr_t) cf.ticket.bytes, query = (uintptr_t) cf.query.bytes;
    put(ticket + 0x1a, (uint16_t) 0x4e02);
    put(ticket + 0x1c, (uint16_t) 0x2b04);
    put(ticket + 0x20, seq);
    put(ticket + 0x28, (uintptr_t) cf.reply.bytes);
    put(ticket + 0x70, (uintptr_t) 1);
    assert(file_issue(ticket) == query);
    file_transport(AZN_FILE_SEND, query, 0);
    file_transport(AZN_FILE_WIRE, query, 0);
    file_transport(AZN_FILE_SERVER, query, 0);
    file_serve(303, (uintptr_t) &query, 0, (uintptr_t) cf.identity.bytes);
    assert(file_routes[seq % 32].phase == FileServed);
    file_transport(AZN_FILE_SEND, (uintptr_t) cf.reply.bytes, 0);
    file_transport(AZN_FILE_WIRE, (uintptr_t) cf.reply.bytes, 0);
    file_transport(AZN_FILE_CLIENT, (uintptr_t) cf.reply.bytes, 0);
    /* The same decoded packet may visit several listeners before its ticket. */
    file_transport(AZN_FILE_CLIENT, (uintptr_t) cf.reply.bytes, 0);
    assert(file_routes[seq % 32].phase == FileDelivered);
}
static void cue_test_drop_content(void) {
    for (unsigned i = 0; i < 16; ++i) {
        hold_file_content(&imported_files[i], 0);
        azn_pin_drop(imported_files[i].pin);
        imported_files[i] = (FileContent) {0};
    }
    while (atomic_load(&reap_needed)) azn_memory_maintenance();
}
static void cue_test_setup(int build) {
    memset(&cf, 0, sizeof cf);
    cf.selected = file_profiles[build - 1];
    cf.selected.getter_return = cf.selected.merge_return = cf.selected.decoder_return = (uintptr_t) &cue_return;
    cf.selected.append_return = cf.selected.hot_return = cf.selected.finish_return = (uintptr_t) &cue_return;
    file_profile = &cf.selected;
    file_bias = 0;
    void *functions[] = {(void *) cue_test_issue, (void *) cue_test_serve, (void *) cue_test_get,
                         (void *) cue_test_merge, (void *) cue_test_ticket, (void *) cue_test_decode, (void *) cue_test_append,
                         (void *) cue_test_reply, (void *) cue_test_publish, (void *) cue_test_append, (void *) input_original};
    memcpy(file_original, functions, sizeof functions);
    cf.list[0] = (uintptr_t) cf.merged.bytes;
    put((uintptr_t) cf.vector.bytes, (uintptr_t) cf.list);
    put((uintptr_t) cf.vector.bytes + 8, (uintptr_t) (cf.list + 1));
    put((uintptr_t) cf.merged.bytes + 0x10, (uint64_t) 16);
    for (unsigned i = 0; i < 2; ++i) {
        uintptr_t atom = (uintptr_t) cf.atom[i].bytes, control = (uintptr_t) cf.control[i].bytes;
        put(atom, file_profile->atoms[i]);
        put(atom + 0x18, (uint32_t) 16);
        put(atom + 0x1c, (uint32_t) 20);
        put(atom + 0x28, (uint64_t) 20);
        put(atom + 0x30, (uintptr_t) cf.bytes.bytes);
        put(atom + 0x38, (uint8_t) 1);
        put(control + 8, (int32_t) 1);
        put(control + 12, (int32_t) 1);
        put((uintptr_t) cf.merged.bytes + 0x30 + i * 16, atom);
        put((uintptr_t) cf.merged.bytes + 0x38 + i * 16, control);
    }
    put((uintptr_t) cf.bytes.bytes, (uint32_t) 16);
    put((uintptr_t) cf.block.bytes, (uintptr_t) cf.bytes.bytes);
    put((uintptr_t) cf.block.bytes + 8, (uint64_t) 16);
    put((uintptr_t) cf.decode_block.bytes, (uintptr_t) cf.payload.bytes);
    put((uintptr_t) cf.decode_block.bytes + 8, (uint64_t) 12);
    put((uintptr_t) cf.accessor.bytes + 0x18, (uintptr_t) cf.storage.bytes);
    put((uintptr_t) cf.handle.bytes, file_profile->input_table);
    setup_rows(&cf.content, &cf.decoded, "native FILE", 0, 48000, false);
}
int main(int argc, char **argv) {
    assert(!memory_fixture_main(argc, argv));
    cue_test_setup(atoi(argv[1]));
    /* A later atom refusing acquisition must release provisional owners
     * without publishing a token or changing the caller's ownership. */
    unsigned char previous_origins[sizeof file_origins];
    memcpy(previous_origins, file_origins, sizeof file_origins);
    FileRead partial = {.count = 1};
    for (unsigned i = 0; i < 2; ++i) {
        partial.owners[0][i].object = (uintptr_t) cf.atom[i].bytes;
        partial.owners[0][i].control = (uintptr_t) cf.control[i].bytes;
    }
    put((uintptr_t) cf.control[1].bytes + 8, (int32_t) 0);
    const uint64_t pending_tokens[1] = {123};
    remember_origins(&partial, pending_tokens);
    assert(*(int32_t *) (cf.control[0].bytes + 8) == 1);
    assert(*(int32_t *) (cf.control[1].bytes + 8) == 0);
    assert(!memcmp(previous_origins, file_origins, sizeof file_origins));
    put((uintptr_t) cf.control[1].bytes + 8, (int32_t) 1);
    const struct azn_file_cue_callbacks callbacks = {
        sizeof callbacks, 2, input_begin, input_complete, input_hold, input_opened, NULL};
    atomic_store(&file_callbacks, &callbacks);
    for (unsigned mode = 0; mode < 7; ++mode) {
        cf.cached = mode == 1;
        cf.corrupt = mode == 2;
        cf.reject_append = mode == 3;
        cf.foreign_decode = mode == 4;
        cf.foreign_append = mode == 5;
        cf.hot = mode == 6;
        put((uintptr_t) cf.decoded.bytes, (uint8_t) (cf.hot ? 3 : 0));
        put((uintptr_t) cf.content.bytes, cf.hot ? file_profile->hot_content : profile.content);
        cue_test_route(1000 + mode);
        assert(file_ticket((uintptr_t) cf.ticket.bytes) == 88 && cf.imported);
        uint64_t token = imported_memory_row((uintptr_t) cf.content.bytes, cf.hot ? 3 : 1);
        assert(token == ((mode == 0 || mode == 1 || mode == 6) ? 111u : 0u));
        assert(!file_import && !file_reading && !file_serving);
        cue_test_drop_content();
    }
    /* A different decoded packet with the same sequence invalidates delivery. */
    cf.hot = cf.cached = cf.corrupt = cf.reject_append = cf.foreign_decode = cf.foreign_append = false;
    cue_test_route(1007);
    Object impostor = cf.reply;
    file_transport(AZN_FILE_CLIENT, (uintptr_t) impostor.bytes, 0);
    assert(!file_routes[1007 % 32].phase);
    assert(file_ticket((uintptr_t) cf.ticket.bytes) == 88 && !cf.imported);
    assert(!imported_memory_row((uintptr_t) cf.content.bytes, 1));
    /* Duplicate issue and reordered transport cannot mint a route. */
    cue_test_route(1008);
    assert(file_issue((uintptr_t) cf.ticket.bytes) == (uintptr_t) cf.query.bytes);
    assert(!file_routes[1008 % 32].phase);
    cue_test_route(1009);
    file_transport(AZN_FILE_SEND, (uintptr_t) cf.query.bytes, 0);
    assert(!file_routes[1009 % 32].phase);
    cue_test_route(1010);
    atomic_flag_test_and_set(&file_guard_busy);
    file_transport(AZN_FILE_CLIENT, (uintptr_t) cf.reply.bytes, 0);
    atomic_flag_clear(&file_guard_busy);
    file_prune();
    assert(!file_routes[1010 % 32].phase);
    assert(cf.ticket_calls == 8 && cf.append_calls == 8 && cf.publish_calls == 8);
    for (unsigned i = 0; i < 16; ++i) {
        release_origins(&file_origins[i]);
        file_origins[i] = (FileOrigins) {0};
    }
    fclose(input_file);
    input_file = NULL;
    atomic_store(&file_callbacks, NULL);
    assert(!input_holds && refs == releases);
    for (unsigned i = 0; i < 2; ++i) assert(SCALAR(int32_t, (uintptr_t) cf.control[i].bytes + 8) == 1);
    puts("PASS FILE cues: native issue/serve/decode/append/publish, cached custody, hot bank, altered bytes, foreign callers, rejected append, ambiguous routing, balanced ownership");
    return 0;
}
