/* SPDX-License-Identifier: GPL-3.0-only */
static void test_unicode_paths(void) {
    const uint16_t valid[] = {'/', 'a', 0xd83c, 0xdfb5, '.', 'm', 'p', '3'};
    const uint16_t high[] = {0xd800}, low[] = {0xdc00}, wrong[] = {0xd800, 'x'}, nul[] = {'a', 0};
    assert(fo_unicode(valid, sizeof valid / sizeof *valid));
    assert(!fo_unicode(high, 1) && !fo_unicode(low, 1));
    assert(!fo_unicode(wrong, 2) && !fo_unicode(nul, 2));
    assert(!fo_unicode(NULL, 1) && !fo_unicode(valid, 0) && !fo_unicode(valid, 1024));
    const uint16_t mixed[] = {'/', 0xa9, 0x20ac, 0xd83c, 0xdfb5};
    assert(sw_name(mixed, 5, "/\xc2\xa9\xe2\x82\xac\xf0\x9f\x8e\xb5"));
    assert(!sw_name(mixed, 5, "/\xc2\xa9\xe2\x82\xac"));
    assert(!sw_name(high, 1, "x") && !sw_name(low, 1, "x"));
    assert(!sw_name(wrong, 2, "x") && !sw_name(nul, 2, "a"));
    assert(!sw_name(NULL, 1, "x") && !sw_name(valid, 1024, "x"));
    assert(sw_name(NULL, 0, "") && !sw_name(NULL, 0, NULL));
    puts("PASS Unicode file locators preserve surrogate pairs and reject incomplete, isolated, embedded NUL and oversized paths");
}

static void test_observation_retry(void) {
    struct ss_binding current = {.cut = 13, .serial = 4, .topics = SS_ALL};
    struct ss_monitor monitor = {0};
    struct ss_observation observation = {.before = current, .current = &current, .monitor = &monitor, .gap = SS_ALL, .proof_cause = DS_PROOF_FILESYSTEM, .proof_before = 17};
    assert(ss_observation_current(&observation));
    current.serial++;
    assert(!ss_observation_current(&observation));
    assert(monitor.pending_db_gap == SS_ALL && monitor.proof_pending_cause == DS_PROOF_FILESYSTEM && monitor.proof_pending_before == 17);
    observation.before = current;
    current.writing = 1;
    assert(!ss_observation_current(&observation));
    current.writing = 0;
    current.cut++;
    assert(!ss_observation_current(&observation));
    observation.before = current;
    current.retired = 1;
    assert(!ss_observation_current(&observation));
    current.retired = 0;
    current.topics = 0;
    assert(!ss_observation_current(&observation));
    current.topics = SS_ALL;
    assert(ss_observation_current(&observation));
    observation.confirmed[0] = 21;
    observation.rank[0] = 5;
    ss_accept_file_proofs(&observation);
    assert(!monitor.pending_db_gap && !monitor.proof_pending_cause);
    assert(monitor.confirmed[0] == 21 && monitor.confirmed_rank[0] == 5);
    puts("PASS observation retry retains uncertainty and prior proof through binding, writer and retirement races until a current cut is accepted");
}
