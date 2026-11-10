/* SPDX-License-Identifier: GPL-3.0-only */
/* Native adapters execute in the ARM emulator. The mock originals model the
 * real two-reply GRID ticket and the PHRASES constructor's hidden x8 result. */
namespace {
struct MapFixture {
    Object atom, bytes, block, output_bytes, output_block, grid, phrase, data, control, cache_result, query, reply;
    uintptr_t cache_object = 0, cache_control = 0, next_query = 0, phrase_object = 0;
    unsigned tag = AZN_FILE_PQTZ;
    int held = 0;
    bool mutate = false;
} mf;
void map_test_hold(const uint64_t *tokens, size_t n, int acquire) {
    for (size_t i = 0; i < n; ++i) if (tokens[i]) mf.held += acquire ? 1 : -1;
    assert(mf.held >= 0);
}
void map_test_complete(uint64_t read, const azn_file_tag *tags, size_t n, uint64_t tokens[3]) {
    assert(read == 9000 && n == 1 && tags && tags->name == mf.tag);
    assert(tags->first == (mf.tag == AZN_FILE_PQTZ ? 14u : 0u));
    std::memset(tokens, 0, 3 * sizeof *tokens);
    tokens[0] = mf.tag == AZN_FILE_PQTZ ? 101 : mf.tag == AZN_FILE_PQT2 ? 102 : 103;
}
int map_test_atom(uintptr_t, uintptr_t, uintptr_t output) { put(output, mf.atom.address()); return 0; }
void map_test_cache() {
    uintptr_t object = input_cache_object, control = input_cache_control;
    input_cache_object = mf.cache_object;
    input_cache_control = mf.cache_control;
    assert(!file_cache_get(101, mf.cache_result.address(), 202));
    input_cache_object = object;
    input_cache_control = control;
}
int map_test_get_grid(uintptr_t accessor, uintptr_t, uintptr_t output) {
    map_test_cache();
    uintptr_t atom = 0;
    assert(!file_map_pqtz(accessor, 0, reinterpret_cast<uintptr_t>(&atom)) && atom == mf.atom.address());
    put(output, mf.block.address());
    return 0;
}
int map_test_get_extra(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t output, uintptr_t count) {
    map_test_cache();
    put(output, mf.block.address());
    put(count, uint64_t(1));
    return 0;
}
template<unsigned Kind>
void map_test_server(uintptr_t server, uintptr_t, unsigned peer, uintptr_t identity) {
    uintptr_t output = 0;
    uint64_t count = 0;
    if (Kind) assert(!file_map_get_specified(101, identity, 0, 0, reinterpret_cast<uintptr_t>(&output), reinterpret_cast<uintptr_t>(&count)));
    else assert(!file_map_get_grid(101, identity, reinterpret_cast<uintptr_t>(&output)));
    file_map_reply(server, mf.reply.address(), peer);
}
int map_test_augment(uintptr_t, uintptr_t, unsigned, unsigned, uintptr_t, unsigned, uintptr_t output, uintptr_t size) {
    put(output, mf.output_bytes.address());
    put(output + 8, uint64_t(24));
    put(size, uint32_t(24));
    return 0;
}
struct MapTestResult { uintptr_t object, reserved[2]; };
template<unsigned Kind>
MapTestResult map_test_construct(uintptr_t, uintptr_t, uintptr_t status) {
    put(status, uint32_t(0));
    return {Kind ? (mf.phrase_object ? mf.phrase_object : mf.phrase.address()) : mf.grid.address(), {0, 0}};
}
uintptr_t map_test_ticket_grid(uintptr_t ticket) {
    if (message_kind(object_word(ticket + 0x28)) == 0x4602) {
        put(ticket + 0x80, object_word(mf.block.address()));
        put(ticket + 0x88, object_word(mf.block.address() + 8));
        put(ticket + 0x1a, uint16_t(0x4f02));
        put(ticket + 0x1c, uint16_t(0x2c04));
        put(ticket + 0x20, scalar<uint32_t>(mf.next_query + 8));
        return mf.next_query;
    }
    if (mf.mutate) mf.bytes.bytes[20] ^= 1;
    uint32_t size = 0, status = 1;
    assert(!file_map_augment(ticket, object_word(ticket + 0x80), 0, unsigned(object_word(ticket + 0x88)),
                            mf.block.address(), unsigned(object_word(mf.block.address() + 8)),
                            mf.output_block.address(), reinterpret_cast<uintptr_t>(&size)));
    MapTestResult output {};
    azn_file_map_content(ticket, mf.output_block.address(), reinterpret_cast<uintptr_t>(&status), reinterpret_cast<uintptr_t>(&output), 0);
    assert(!status && output.object == mf.grid.address());
    return 0;
}
uintptr_t map_test_ticket_phrase(uintptr_t ticket) {
    MapTestResult output {};
    uint32_t status = 1;
    if (mf.mutate) mf.bytes.bytes[20] ^= 1;
    azn_file_map_content(ticket, mf.block.address(), reinterpret_cast<uintptr_t>(&status), reinterpret_cast<uintptr_t>(&output), 1);
    assert(!status && output.object == (mf.phrase_object ? mf.phrase_object : mf.phrase.address()));
    return 0;
}
void map_test_message(Object &object, unsigned kind, unsigned seq) {
    object = {};
    put(object.address(), uint16_t(kind));
    put(object.address() + 4, uint32_t(0x872349ae));
    put(object.address() + 8, uint32_t(seq));
}
void map_test_route(Object &query, Object &ticket, unsigned seq, unsigned tag) {
    mf.tag = tag;
    unsigned kind = tag == AZN_FILE_PQTZ ? 0x2204 : 0x2c04;
    map_test_message(query, kind, seq);
    map_test_message(mf.reply, map_response(kind), seq);
    file_map_transport(AZN_FILE_SEND, query.address(), 0);
    file_map_transport(AZN_FILE_WIRE, query.address(), 0);
    file_map_transport(AZN_FILE_SERVER, query.address(), 0);
    uintptr_t request = query.address();
    if (tag == AZN_FILE_PQTZ) file_map_serve<0>(303, reinterpret_cast<uintptr_t>(&request), 0, 404);
    else file_map_serve<1>(303, reinterpret_cast<uintptr_t>(&request), 0, 404);
    file_map_transport(AZN_FILE_SEND, mf.reply.address(), 0);
    file_map_transport(AZN_FILE_WIRE, mf.reply.address(), 0);
    file_map_transport(AZN_FILE_CLIENT, mf.reply.address(), 0);
    put(ticket.address() + 0x28, mf.reply.address());
    put(ticket.address() + 0x20, uint32_t(seq));
    put(ticket.address() + 0x1a, uint16_t(map_response(kind)));
    put(ticket.address() + 0x1c, uint16_t(kind));
}
void map_test_encoded(unsigned tag) {
    mf.bytes = {};
    put(mf.bytes.address(), uint32_t(60));
    put(mf.bytes.address() + 4, __builtin_bswap32(tag));
    put(mf.bytes.address() + 8, __builtin_bswap32(uint32_t(24)));
    put(mf.bytes.address() + 12, __builtin_bswap32(uint32_t(60)));
    put(mf.block.address(), mf.bytes.address());
    put(mf.block.address() + 8, uint64_t(64));
}
void map_adapter_cases(int build, const azn_file_cue_callbacks *common) {
    const auto prior = file_callbacks.load();
    file_callbacks.store(common);
    const azn_file_map_callbacks cb = {sizeof cb, 1, nullptr, map_test_complete, map_test_hold, nullptr, common_api};
    file_map_callbacks.store(&cb);
    file_map_profile = &file_map_profiles[build - 1];
    file_map_original[0] = reinterpret_cast<void *>(map_test_server<0>);
    file_map_original[1] = reinterpret_cast<void *>(map_test_server<1>);
    file_map_original[2] = reinterpret_cast<void *>(map_test_get_grid);
    file_map_original[3] = reinterpret_cast<void *>(map_test_get_extra);
    file_map_original[4] = reinterpret_cast<void *>(map_test_atom);
    file_map_original[5] = reinterpret_cast<void *>(map_test_ticket_grid);
    file_map_original[6] = reinterpret_cast<void *>(map_test_ticket_phrase);
    file_map_original[7] = reinterpret_cast<void *>(map_test_augment);
    file_map_original[8] = reinterpret_cast<void *>(map_test_construct<0>);
    file_map_original[9] = reinterpret_cast<void *>(map_test_construct<1>);
    put(mf.grid.address(), file_map_profile->content[0]);
    put(mf.phrase.address(), file_map_profile->content[1]);
    put(mf.control.address() + 8, int32_t(1));
    put(mf.control.address() + 12, int32_t(1));
    mf.cache_object = mf.data.address();
    mf.cache_control = mf.control.address();
    ++common_holds;
    file_input_remember(common_api(), 2222, 1, mf.cache_object, mf.cache_control);
    Object grid_ticket, phrase_ticket, first_query, second_query, phrase_query, base_bytes;
    put(mf.atom.address() + 0x18, uint32_t(24));
    put(mf.atom.address() + 0x1c, uint32_t(64));
    put(mf.atom.address() + 0x28, uint64_t(64));
    put(mf.atom.address() + 0x30, mf.bytes.address());
    put(mf.block.address(), base_bytes.address());
    put(mf.block.address() + 8, uint64_t(64));
    map_test_message(second_query, 0x2c04, 101);
    mf.next_query = second_query.address();
    map_test_route(first_query, grid_ticket, 100, AZN_FILE_PQTZ);
    assert(file_map_ticket<0>(grid_ticket.address()) == second_query.address());
    assert(map_pending[0].token == 101);
    // The real second query must retain the first query's exact ticket and
    // MemoryBlock. A reused ticket address alone cannot authorize a join.
    MapImport wrong;
    wrong.seq = 101;
    wrong.ticket = phrase_ticket.address();
    map_pending_take(wrong, first_query.address());
    assert(!wrong.tokens[0]);
    // Recreate the witnessed first response using a new monotonic sequence.
    map_test_message(second_query, 0x2c04, 103);
    map_test_route(first_query, grid_ticket, 102, AZN_FILE_PQTZ);
    assert(file_map_ticket<0>(grid_ticket.address()) == second_query.address());
    for (auto &o : file_input_origins) file_input_release(o);
    ++common_holds;
    file_input_remember(common_api(), 2223, 2, mf.cache_object, mf.cache_control);
    map_test_encoded(AZN_FILE_PQT2);
    map_test_route(second_query, grid_ticket, 103, AZN_FILE_PQT2);
    assert(!file_map_ticket<0>(grid_ticket.address()));
    map_test_encoded(AZN_FILE_PSSI);
    map_test_route(phrase_query, phrase_ticket, 104, AZN_FILE_PSSI);
    assert(!file_map_ticket<1>(phrase_ticket.address()));
    aznm_packet packet {};
    packet.owner = 1;
    packet.grid.content = azn_owned_pin(mf.grid.address());
    packet.phrases.content = azn_owned_pin(mf.phrase.address());
    uint64_t tokens[3] {};
    assert(file_maps_origins(&packet, tokens) && tokens[0] == 101 && tokens[1] == 102 && tokens[2] == 103);
    file_maps_hold(tokens, 3, 0);
    // Equal content in another independently owned object is not membership.
    Object foreign_grid;
    put(foreign_grid.address(), file_map_profile->content[0]);
    azn_pin_drop(packet.grid.content);
    packet.grid.content = azn_owned_pin(foreign_grid.address());
    assert(!file_maps_origins(&packet, tokens));
    azn_pin_drop(packet.grid.content);
    azn_pin_drop(packet.phrases.content);
    // The constructor must consume the exact encoded reply observed at the
    // source. Mutation after delivery cannot attach that source to a new Content.
    Object tainted_phrase;
    put(tainted_phrase.address(), file_map_profile->content[1]);
    mf.phrase_object = tainted_phrase.address();
    map_test_encoded(AZN_FILE_PSSI);
    map_test_route(phrase_query, phrase_ticket, 105, AZN_FILE_PSSI);
    mf.mutate = true;
    assert(!file_map_ticket<1>(phrase_ticket.address()));
    mf.mutate = false;
    packet.grid.content = azn_owned_pin(mf.grid.address());
    packet.phrases.content = azn_owned_pin(tainted_phrase.address());
    assert(!file_maps_origins(&packet, tokens));
    azn_pin_drop(packet.grid.content);
    azn_pin_drop(packet.phrases.content);
    // Remote traffic and duplicate/old sequences cannot enroll an origin.
    map_test_message(first_query, 0x2204, 106);
    file_map_transport(AZN_FILE_SEND, first_query.address(), 1);
    assert(map_routes[106 % 32].seq != 106);
    file_map_transport(AZN_FILE_SEND, first_query.address(), 0);
    file_map_transport(AZN_FILE_SEND, first_query.address(), 0);
    assert(!map_routes[106 % 32].phase);
    for (auto &r : map_routes) map_retire(r);
    for (auto &p : map_pending) { file_maps_hold(&p.token, 1, 0); p = {}; }
    for (auto &c : map_contents) { file_maps_hold(c.tokens, 3, 0); azn_pin_drop(c.pin); c = {}; }
    for (auto &o : file_input_origins) file_input_release(o);
    assert(!mf.held && !common_holds && scalar<int32_t>(mf.control.address() + 8) == 1 && scalar<int32_t>(mf.control.address() + 12) == 1);
    file_map_callbacks.store(nullptr);
    file_callbacks.store(prior);
    while (reap_needed.load()) azn_memory_maintenance();
    puts("PASS FILE maps: original cached inputs, two-reply GRID/PQT2 continuation, encoded PSSI constructor, exact Content membership, changed wire, stale sequence and remote rejection, balanced custody");
}
} // namespace
