/* SPDX-License-Identifier: GPL-3.0-only */
#include "source_provider_carrier.c"

#include <assert.h>
#include <stdio.h>

static struct carrier_vector workers;
static const char library_name[] = "Library";
static const uint8_t factory_config = 7;
static unsigned factory_calls;
static uint8_t factory_storage[2];
static int stock_library;

/* The bridge leaves its C result argument in x4 when it forwards to the
 * original. Use that argument to stub the factory without a firmware image. */
static void stock_factory(const struct carrier_vector *input, const void *name,
                          const uint8_t *storage, const uint8_t *config, void **out) {
    assert(input == &workers && name == library_name && config == &factory_config);
    assert(factory_calls < sizeof factory_storage);
    factory_storage[factory_calls++] = *storage;
    *out = *storage == CARRIER_STORAGE ? NULL : &stock_library;
}

static void assert_factory_calls(uint8_t storage, unsigned expected) {
    factory_calls = 0;
    void *result = NULL;
    assert(carrier_factory_hook(&workers, library_name, &storage, &factory_config, &result) == &result);
    assert(result == &stock_library);
    assert(factory_calls == expected && factory_storage[0] == storage);
    if (expected == 2)
        assert(factory_storage[1] == CARRIER_STORAGE);
}

static void factory_ownership(void) {
    library_factory = (void *) stock_factory;
    assert_factory_calls(2, 1);
    atomic_store(&carrier_enabled, 1);
    assert_factory_calls(1, 1);
    assert_factory_calls(2, 2);
    assert(!carrier_library.object && !carrier_library.control);
    carrier_library.object = &stock_library;
    assert_factory_calls(2, 1);
    carrier_library = (struct carrier_owner) {0};
    atomic_store(&carrier_enabled, 0);
}

struct aggregate_fixture {
    struct carrier_vector input, streaming, servers;
    struct carrier_owner added;
    unsigned calls;
};

static void check_servers(struct aggregate_fixture *fixture, const struct carrier_vector *servers) {
    const struct carrier_vector *original = &fixture->servers;
    ptrdiff_t count = original->end - original->begin;
    ++fixture->calls;
    if (!fixture->added.object) {
        assert(servers == original);
        return;
    }
    assert(servers != original && servers->end - servers->begin == count + 1);
    assert(servers->capacity == servers->end);
    for (ptrdiff_t i = 0; i < count; ++i) {
        assert(servers->begin[i].object == original->begin[i].object);
        assert(servers->begin[i].control == original->begin[i].control);
    }
    assert(servers->begin[count].object == fixture->added.object);
    assert(servers->begin[count].control == fixture->added.control);
    assert(!original->end->object && !original->end->control);
}

static void stock_library_aggregate(void *self, const struct carrier_vector *input, const struct carrier_vector *servers) {
    struct aggregate_fixture *fixture = self;
    assert(input == &fixture->input);
    check_servers(fixture, servers);
}

static void stock_repository_aggregate(void *self, const struct carrier_vector *input,
                                       const struct carrier_vector *servers, const struct carrier_vector *streaming) {
    struct aggregate_fixture *fixture = self;
    assert(input == &fixture->input && streaming == &fixture->streaming);
    check_servers(fixture, servers);
}

static void aggregate_ownership(void) {
    int objects[17], controls[17], carrier_object, carrier_control;
    struct carrier_owner owners[18] = {0};
    for (size_t i = 0; i < sizeof objects / sizeof objects[0]; ++i)
        owners[i] = (struct carrier_owner) {&objects[i], &controls[i]};
    owners[5] = (struct carrier_owner) {0};
    struct carrier_owner extra = {&carrier_object, &carrier_control};
    struct aggregate_fixture fixture = {.servers = {owners, owners + 17, owners + 18}};
    library_aggregate = stock_library_aggregate;
    repository_aggregate = stock_repository_aggregate;

    carrier_library = (struct carrier_owner) {0};
    carrier_library_aggregate(&fixture, &fixture.input, &fixture.servers);
    fixture.added = carrier_library = extra;
    carrier_library_aggregate(&fixture, &fixture.input, &fixture.servers);
    --fixture.servers.end;
    fixture.added = (struct carrier_owner) {0};
    carrier_library_aggregate(&fixture, &fixture.input, &fixture.servers);

    fixture.servers.end = owners + 5;
    carrier_repository_aggregate(&fixture, &fixture.input, &fixture.servers, &fixture.streaming);
    fixture.added = carrier_repository = extra;
    carrier_repository_aggregate(&fixture, &fixture.input, &fixture.servers, &fixture.streaming);
    --fixture.servers.end;
    fixture.added = (struct carrier_owner) {0};
    carrier_repository_aggregate(&fixture, &fixture.input, &fixture.servers, &fixture.streaming);
    assert(fixture.calls == 6);
    carrier_library = (struct carrier_owner) {0};
    carrier_repository = (struct carrier_owner) {0};
}

static unsigned forwarded_mounts;
static const char root_path[] = "/carrier";
static const char *filesystem = "vfat";

static unsigned stock_storage_worker(void *controller, unsigned link, unsigned storage, const void *path,
                                     const void *fs, unsigned flags, unsigned protection) {
    assert(controller && path && fs == &filesystem);
    assert(link == 1 && (storage == 2 || storage == CARRIER_STORAGE) && flags == 3 && protection == 4);
    ++forwarded_mounts;
    return 85;
}

static void mount_ownership(void) {
    unsigned char controller[0x518 + sizeof(void *)] = {0};
    int other_controller;
    const char *path = root_path;
    const char *other_path = "/carrier/other";
    storage_worker = stock_storage_worker;
    assert(carrier_storage_worker(controller, 1, CARRIER_STORAGE, &path, &filesystem, 3, 4) == 85);

    atomic_store(&carrier_enabled, 1);
    assert(azsp_carrier_mount(root_path) == AZ_PENDING);
    atomic_store(&storage_controller, controller);
    assert(carrier_storage_worker(controller, 1, 2, &path, &filesystem, 3, 4) == 85);
    assert(carrier_storage_worker(controller, 1, CARRIER_STORAGE, &other_path, &filesystem, 3, 4) == 85);
    assert(carrier_storage_worker(&other_controller, 1, CARRIER_STORAGE, &path, &filesystem, 3, 4) == 85);
    /* The matching request is intercepted. Without a native progress object
     * it fails locally and never reaches the stock USB worker. */
    assert(carrier_storage_worker(controller, 1, CARRIER_STORAGE, &path, &filesystem, 3, 4) == 0);
    assert(forwarded_mounts == 4);
}

int main(void) {
    factory_ownership();
    aggregate_ownership();
    mount_ownership();
    puts("PASS source carrier factory, server vectors, and mount ownership");
    return 0;
}
