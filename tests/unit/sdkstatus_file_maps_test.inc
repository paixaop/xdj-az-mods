/* SPDX-License-Identifier: GPL-3.0-only */
/* Native adapters execute in the ARM emulator. The mock originals model the
 * real two-reply GRID ticket and the PHRASES constructor's hidden x8 result. */

struct MapFixture {
    Object atom, bytes, block, output_bytes, output_block, grid, phrase, data, control, cache_result, query, reply;
    uintptr_t cache_object, cache_control, next_query, phrase_object;
    unsigned tag;
    int held;
    bool mutate;
} mf = {.tag = AZN_FILE_PQTZ};
void map_test_hold(const uint64_t *tokens, size_t n, int acquire) {
    for (size_t i = 0; i < n; ++i)
        if (tokens[i]) mf.held += acquire ? 1 : -1;
    assert(mf.held >= 0);
}
void map_test_complete(uint64_t read, const struct azn_file_tag *tags, size_t n, uint64_t tokens[3]) {
    assert(read == 9000 && n == 1 && tags && tags->name == mf.tag);
    assert(tags->first == (mf.tag == AZN_FILE_PQTZ ? 14u : 0u));
    memset(tokens, 0, 3 * sizeof *tokens);
    tokens[0] = mf.tag == AZN_FILE_PQTZ ? 101 : mf.tag == AZN_FILE_PQT2 ? 102
                                                                        : 103;
}
int map_test_atom(uintptr_t unused_0 __attribute__((unused)), uintptr_t unused_1 __attribute__((unused)), uintptr_t output) {
    put(output, ((uintptr_t) mf.atom.bytes));
    return 0;
}
void map_test_cache() {
    uintptr_t object = input_cache_object, control = input_cache_control;
    input_cache_object = mf.cache_object;
    input_cache_control = mf.cache_control;
    assert(!file_cache_get(101, ((uintptr_t) mf.cache_result.bytes), 202));
    input_cache_object = object;
    input_cache_control = control;
}
int map_test_get_grid(uintptr_t accessor, uintptr_t unused_1 __attribute__((unused)), uintptr_t output) {
    map_test_cache();
    uintptr_t atom = 0;
    assert(!file_map_pqtz(accessor, 0, ((uintptr_t) (&atom))) && atom == ((uintptr_t) mf.atom.bytes));
    put(output, ((uintptr_t) mf.block.bytes));
    return 0;
}
int map_test_get_extra(uintptr_t unused_0 __attribute__((unused)), uintptr_t unused_1 __attribute__((unused)), uintptr_t unused_2 __attribute__((unused)), uintptr_t unused_3 __attribute__((unused)), uintptr_t output, uintptr_t count) {
    map_test_cache();
    put(output, ((uintptr_t) mf.block.bytes));
    put(count, (uint64_t) (1));
    return 0;
}
void map_test_server_0(uintptr_t server, uintptr_t unused_1 __attribute__((unused)), unsigned peer, uintptr_t identity) {
    uintptr_t output = 0;
    uint64_t count = 0;
    if (0) assert(!file_map_get_specified(101, identity, 0, 0, ((uintptr_t) (&output)), ((uintptr_t) (&count))));
    else assert(!file_map_get_grid(101, identity, ((uintptr_t) (&output))));
    file_map_reply(server, ((uintptr_t) mf.reply.bytes), peer);
}
void map_test_server_1(uintptr_t server, uintptr_t unused_1 __attribute__((unused)), unsigned peer, uintptr_t identity) {
    uintptr_t output = 0;
    uint64_t count = 0;
    if (1) assert(!file_map_get_specified(101, identity, 0, 0, ((uintptr_t) (&output)), ((uintptr_t) (&count))));
    else assert(!file_map_get_grid(101, identity, ((uintptr_t) (&output))));
    file_map_reply(server, ((uintptr_t) mf.reply.bytes), peer);
}
int map_test_augment(uintptr_t unused_0 __attribute__((unused)), uintptr_t unused_1 __attribute__((unused)), unsigned unused_2 __attribute__((unused)), unsigned unused_3 __attribute__((unused)), uintptr_t unused_4 __attribute__((unused)), unsigned unused_5 __attribute__((unused)), uintptr_t output, uintptr_t size) {
    put(output, ((uintptr_t) mf.output_bytes.bytes));
    put(output + 8, (uint64_t) (24));
    put(size, (uint32_t) (24));
    return 0;
}
typedef struct MapTestResult MapTestResult;
struct MapTestResult {
    uintptr_t object, reserved[2];
};
MapTestResult map_test_construct_0(uintptr_t unused_0 __attribute__((unused)), uintptr_t unused_1 __attribute__((unused)), uintptr_t status) {
    put(status, (uint32_t) (0));
    return (MapTestResult) {0 ? (mf.phrase_object ? mf.phrase_object : ((uintptr_t) mf.phrase.bytes)) : ((uintptr_t) mf.grid.bytes), {0, 0}};
}
MapTestResult map_test_construct_1(uintptr_t unused_0 __attribute__((unused)), uintptr_t unused_1 __attribute__((unused)), uintptr_t status) {
    put(status, (uint32_t) (0));
    return (MapTestResult) {1 ? (mf.phrase_object ? mf.phrase_object : ((uintptr_t) mf.phrase.bytes)) : ((uintptr_t) mf.grid.bytes), {0, 0}};
}
uintptr_t map_test_ticket_grid(uintptr_t ticket) {
    if (message_kind(object_word(ticket + 0x28)) == 0x4602) {
        put(ticket + 0x80, object_word(((uintptr_t) mf.block.bytes)));
        put(ticket + 0x88, object_word(((uintptr_t) mf.block.bytes) + 8));
        put(ticket + 0x1a, (uint16_t) (0x4f02));
        put(ticket + 0x1c, (uint16_t) (0x2c04));
        put(ticket + 0x20, SCALAR(uint32_t, mf.next_query + 8));
        return mf.next_query;
    }
    if (mf.mutate) mf.bytes.bytes[20] ^= 1;
    uint32_t size = 0, status = 1;
    assert(!file_map_augment(ticket, object_word(ticket + 0x80), 0, (unsigned) (object_word(ticket + 0x88)),
                             ((uintptr_t) mf.block.bytes), (unsigned) (object_word(((uintptr_t) mf.block.bytes) + 8)),
                             ((uintptr_t) mf.output_block.bytes), ((uintptr_t) (&size))));
    MapTestResult output = {0};
    azn_file_map_content(ticket, ((uintptr_t) mf.output_block.bytes), ((uintptr_t) (&status)), ((uintptr_t) (&output)), 0);
    assert(!status && output.object == ((uintptr_t) mf.grid.bytes));
    return 0;
}
uintptr_t map_test_ticket_phrase(uintptr_t ticket) {
    MapTestResult output = {0};
    uint32_t status = 1;
    if (mf.mutate) mf.bytes.bytes[20] ^= 1;
    azn_file_map_content(ticket, ((uintptr_t) mf.block.bytes), ((uintptr_t) (&status)), ((uintptr_t) (&output)), 1);
    assert(!status && output.object == (mf.phrase_object ? mf.phrase_object : ((uintptr_t) mf.phrase.bytes)));
    return 0;
}
void map_test_message(Object *object, unsigned kind, unsigned seq) {
    (*object) = (Object) {0};
    put(((uintptr_t) object->bytes), (uint16_t) (kind));
    put(((uintptr_t) object->bytes) + 4, (uint32_t) (0x872349ae));
    put(((uintptr_t) object->bytes) + 8, (uint32_t) (seq));
}
void map_test_route(Object *query, Object *ticket, unsigned seq, unsigned tag) {
    mf.tag = tag;
    unsigned kind = tag == AZN_FILE_PQTZ ? 0x2204 : 0x2c04;
    map_test_message(query, kind, seq);
    map_test_message(&(mf.reply), map_response(kind), seq);
    file_map_transport(AZN_FILE_SEND, ((uintptr_t) query->bytes), 0);
    file_map_transport(AZN_FILE_WIRE, ((uintptr_t) query->bytes), 0);
    file_map_transport(AZN_FILE_SERVER, ((uintptr_t) query->bytes), 0);
    uintptr_t request = ((uintptr_t) query->bytes);
    if (tag == AZN_FILE_PQTZ) file_map_serve_0(303, ((uintptr_t) (&request)), 0, 404);
    else file_map_serve_1(303, ((uintptr_t) (&request)), 0, 404);
    file_map_transport(AZN_FILE_SEND, ((uintptr_t) mf.reply.bytes), 0);
    file_map_transport(AZN_FILE_WIRE, ((uintptr_t) mf.reply.bytes), 0);
    file_map_transport(AZN_FILE_CLIENT, ((uintptr_t) mf.reply.bytes), 0);
    put(((uintptr_t) ticket->bytes) + 0x28, ((uintptr_t) mf.reply.bytes));
    put(((uintptr_t) ticket->bytes) + 0x20, (uint32_t) (seq));
    put(((uintptr_t) ticket->bytes) + 0x1a, (uint16_t) (map_response(kind)));
    put(((uintptr_t) ticket->bytes) + 0x1c, (uint16_t) (kind));
}
void map_test_encoded(unsigned tag) {
    mf.bytes = (Object) {0};
    put(((uintptr_t) mf.bytes.bytes), (uint32_t) (60));
    put(((uintptr_t) mf.bytes.bytes) + 4, __builtin_bswap32(tag));
    put(((uintptr_t) mf.bytes.bytes) + 8, __builtin_bswap32((uint32_t) (24)));
    put(((uintptr_t) mf.bytes.bytes) + 12, __builtin_bswap32((uint32_t) (60)));
    put(((uintptr_t) mf.block.bytes), ((uintptr_t) mf.bytes.bytes));
    put(((uintptr_t) mf.block.bytes) + 8, (uint64_t) (64));
}
void map_adapter_cases(int build, const struct azn_file_cue_callbacks *common) {
    __auto_type prior = atomic_load_explicit(&(file_callbacks), memory_order_seq_cst);
    atomic_store_explicit(&(file_callbacks), common, memory_order_seq_cst);
    const struct azn_file_map_callbacks cb = {sizeof cb, 1, NULL, map_test_complete, map_test_hold, NULL, common_api};
    atomic_store_explicit(&(file_map_callbacks), &cb, memory_order_seq_cst);
    file_map_profile = &file_map_profiles[build - 1];
    file_map_original[0] = ((void *) (map_test_server_0));
    file_map_original[1] = ((void *) (map_test_server_1));
    file_map_original[2] = ((void *) (map_test_get_grid));
    file_map_original[3] = ((void *) (map_test_get_extra));
    file_map_original[4] = ((void *) (map_test_atom));
    file_map_original[5] = ((void *) (map_test_ticket_grid));
    file_map_original[6] = ((void *) (map_test_ticket_phrase));
    file_map_original[7] = ((void *) (map_test_augment));
    file_map_original[8] = ((void *) (map_test_construct_0));
    file_map_original[9] = ((void *) (map_test_construct_1));
    put(((uintptr_t) mf.grid.bytes), file_map_profile->content[0]);
    put(((uintptr_t) mf.phrase.bytes), file_map_profile->content[1]);
    put(((uintptr_t) mf.control.bytes) + 8, (int32_t) (1));
    put(((uintptr_t) mf.control.bytes) + 12, (int32_t) (1));
    mf.cache_object = ((uintptr_t) mf.data.bytes);
    mf.cache_control = ((uintptr_t) mf.control.bytes);
    ++common_holds;
    file_input_remember(common_api(), 2222, 1, mf.cache_object, mf.cache_control);
    Object grid_ticket = {0}, phrase_ticket = {0}, first_query = {0}, second_query = {0}, phrase_query = {0}, base_bytes = {0};
    put(((uintptr_t) mf.atom.bytes) + 0x18, (uint32_t) (24));
    put(((uintptr_t) mf.atom.bytes) + 0x1c, (uint32_t) (64));
    put(((uintptr_t) mf.atom.bytes) + 0x28, (uint64_t) (64));
    put(((uintptr_t) mf.atom.bytes) + 0x30, ((uintptr_t) mf.bytes.bytes));
    put(((uintptr_t) mf.block.bytes), ((uintptr_t) base_bytes.bytes));
    put(((uintptr_t) mf.block.bytes) + 8, (uint64_t) (64));
    map_test_message(&(second_query), 0x2c04, 101);
    mf.next_query = ((uintptr_t) second_query.bytes);
    map_test_route(&(first_query), &(grid_ticket), 100, AZN_FILE_PQTZ);
    assert(file_map_ticket_0(((uintptr_t) grid_ticket.bytes)) == ((uintptr_t) second_query.bytes));
    assert(map_pending[0].token == 101);
    // The real second query must retain the first query's exact ticket and
    // MemoryBlock. A reused ticket address alone cannot authorize a join.
    MapImport wrong __attribute__((cleanup(map_import_drop))) = {0};
    wrong.seq = 101;
    wrong.ticket = ((uintptr_t) phrase_ticket.bytes);
    map_pending_take(&(wrong), ((uintptr_t) first_query.bytes));
    assert(!wrong.tokens[0]);
    // Recreate the witnessed first response using a new monotonic sequence.
    map_test_message(&(second_query), 0x2c04, 103);
    map_test_route(&(first_query), &(grid_ticket), 102, AZN_FILE_PQTZ);
    assert(file_map_ticket_0(((uintptr_t) grid_ticket.bytes)) == ((uintptr_t) second_query.bytes));
    for (unsigned o_index = 0; o_index < sizeof(file_input_origins) / sizeof(file_input_origins)[0]; ++o_index) {
        FileInputOrigin *o = &(file_input_origins[o_index]);
        file_input_release(o);
    }
    ++common_holds;
    file_input_remember(common_api(), 2223, 2, mf.cache_object, mf.cache_control);
    map_test_encoded(AZN_FILE_PQT2);
    map_test_route(&(second_query), &(grid_ticket), 103, AZN_FILE_PQT2);
    assert(!file_map_ticket_0(((uintptr_t) grid_ticket.bytes)));
    map_test_encoded(AZN_FILE_PSSI);
    map_test_route(&(phrase_query), &(phrase_ticket), 104, AZN_FILE_PSSI);
    assert(!file_map_ticket_1(((uintptr_t) phrase_ticket.bytes)));
    struct aznm_packet packet = {0};
    packet.owner = 1;
    packet.grid.content = azn_owned_pin(((uintptr_t) mf.grid.bytes));
    packet.phrases.content = azn_owned_pin(((uintptr_t) mf.phrase.bytes));
    uint64_t tokens[3] = {0};
    assert(file_maps_origins(&packet, tokens) && tokens[0] == 101 && tokens[1] == 102 && tokens[2] == 103);
    file_maps_hold(tokens, 3, 0);
    // Equal content in another independently owned object is not membership.
    Object foreign_grid = {0};
    put(((uintptr_t) foreign_grid.bytes), file_map_profile->content[0]);
    azn_pin_drop(packet.grid.content);
    packet.grid.content = azn_owned_pin(((uintptr_t) foreign_grid.bytes));
    assert(!file_maps_origins(&packet, tokens));
    azn_pin_drop(packet.grid.content);
    azn_pin_drop(packet.phrases.content);
    // The constructor must consume the exact encoded reply observed at the
    // source. Mutation after delivery cannot attach that source to a new Content.
    Object tainted_phrase = {0};
    put(((uintptr_t) tainted_phrase.bytes), file_map_profile->content[1]);
    mf.phrase_object = ((uintptr_t) tainted_phrase.bytes);
    map_test_encoded(AZN_FILE_PSSI);
    map_test_route(&(phrase_query), &(phrase_ticket), 105, AZN_FILE_PSSI);
    mf.mutate = true;
    assert(!file_map_ticket_1(((uintptr_t) phrase_ticket.bytes)));
    mf.mutate = false;
    packet.grid.content = azn_owned_pin(((uintptr_t) mf.grid.bytes));
    packet.phrases.content = azn_owned_pin(((uintptr_t) tainted_phrase.bytes));
    assert(!file_maps_origins(&packet, tokens));
    azn_pin_drop(packet.grid.content);
    azn_pin_drop(packet.phrases.content);
    // Remote traffic and duplicate/old sequences cannot enroll an origin.
    map_test_message(&(first_query), 0x2204, 106);
    file_map_transport(AZN_FILE_SEND, ((uintptr_t) first_query.bytes), 1);
    assert(map_routes[106 % 32].seq != 106);
    file_map_transport(AZN_FILE_SEND, ((uintptr_t) first_query.bytes), 0);
    file_map_transport(AZN_FILE_SEND, ((uintptr_t) first_query.bytes), 0);
    assert(!map_routes[106 % 32].phase);
    for (unsigned r_index = 0; r_index < sizeof(map_routes) / sizeof(map_routes)[0]; ++r_index) {
        MapRoute *r = &(map_routes[r_index]);
        map_retire(r);
    }
    for (unsigned p_index = 0; p_index < sizeof(map_pending) / sizeof(map_pending)[0]; ++p_index) {
        MapPending *p = &(map_pending[p_index]);
        file_maps_hold(&p->token, 1, 0);
        (*p) = (MapPending) {0};
    }
    for (unsigned c_index = 0; c_index < sizeof(map_contents) / sizeof(map_contents)[0]; ++c_index) {
        MapContentOrigin *c = &(map_contents[c_index]);
        file_maps_hold(c->tokens, 3, 0);
        azn_pin_drop(c->pin);
        (*c) = (MapContentOrigin) {0};
    }
    for (unsigned o_index = 0; o_index < sizeof(file_input_origins) / sizeof(file_input_origins)[0]; ++o_index) {
        FileInputOrigin *o = &(file_input_origins[o_index]);
        file_input_release(o);
    }
    assert(!mf.held && !common_holds && SCALAR(int32_t, ((uintptr_t) mf.control.bytes) + 8) == 1 && SCALAR(int32_t, ((uintptr_t) mf.control.bytes) + 12) == 1);
    atomic_store_explicit(&(file_map_callbacks), NULL, memory_order_seq_cst);
    atomic_store_explicit(&(file_callbacks), prior, memory_order_seq_cst);
    while (atomic_load_explicit(&(reap_needed), memory_order_seq_cst)) azn_memory_maintenance();
    puts("PASS FILE maps: original cached inputs, two-reply GRID/PQT2 continuation, encoded PSSI constructor, exact Content membership, changed wire, stale sequence and remote rejection, balanced custody");
}
