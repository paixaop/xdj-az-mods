/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static void fixture_vtable(uintptr_t address, uintptr_t value) {
    assert(fixture_vtable_count < 8);
    fixture_vtables[fixture_vtable_count].address = address;
    fixture_vtables[fixture_vtable_count++].value = value;
}
static void fixture_signatures(unsigned version) {
    fixture_vtable_count = 0;
    const uintptr_t tempo[] = {0x1f5c8c8, 0x20c8c40}, disposal[] = {0x1fcbe20, 0x21394f0};
    const uint32_t tempo_signature[] = {0xa9b87bfd, 0x910003fd, 0xa90153f3, 0xaa0003f3};
    memcpy((void *) tempo[version], tempo_signature, sizeof tempo_signature);
    fixture_vtable(BPMCTRL_VT + 16, disposal[version]);
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++) {
        const struct ds_model_profile *profile = &ds_models[model];
        const uint32_t signature[] = {profile->first, 0x910003fd, 0xa90153f3, 0xaa0003f3};
        memcpy((void *) profile->writer[version], signature, sizeof signature);
        fixture_vtable(profile->control[version] + 16, profile->dispose[version]);
    }
    const struct ds_pad_profile *pad = &ds_pad_profiles[version];
    const uint32_t entries[] = {0xa9af7bfd, 0xa9b47bfd, 0xf9400c01, 0xa9b87bfd, 0xa9b37bfd, 0xa9b87bfd, 0xa9bc7bfd, 0xa9b47bfd};
    for (unsigned i = 0; i < 8; i++) memcpy((void *) pad->entry[i], &entries[i], sizeof entries[i]);
    const uintptr_t selectors[] = {0x2039d58, 0x21a7650};
    const uint32_t signature[] = {0x72001c7f, 0x54000160, 0xa9be7bfd, 0x910003fd};
    memcpy((void *) selectors[version], signature, sizeof signature);
    fixture_vtable(pad->ctrl + 16, pad->dispose);
}
static void fixture_hooks_reset(void) {
    fixture_hook_calls = fixture_hook_failure = 0;
    fixture_vtable_wrong = 0;
    atomic_store(&ds_tempo_ready, 0);
    atomic_store(&ds_pad_ready, 0);
    for (unsigned m = 0; m < DS_MODEL_COUNT; m++) atomic_store(&ds_model_ready[m], 0);
}
static void fixture_install_all(void) {
    ds_tempo_install();
    ds_models_install();
    ds_pad_install();
}
static void fixture_install_rejections(unsigned version) {
    fixture_hooks_reset();
    fixture_vtable_wrong = 1;
    fixture_install_all();
    assert(!fixture_hook_calls && !atomic_load(&ds_tempo_ready) && !atomic_load(&ds_pad_ready));
    fixture_hooks_reset();
    uintptr_t high = g_ep_img_hi;
    g_ep_img_hi = g_ep_img_lo;
    fixture_install_all();
    assert(!fixture_hook_calls);
    g_ep_img_hi = high;
    const uintptr_t tempo[] = {0x1f5c8c8, 0x20c8c40};
    *(uint32_t *) tempo[version] ^= 1;
    ds_tempo_install();
    assert(!fixture_hook_calls && !atomic_load(&ds_tempo_ready));
    *(uint32_t *) tempo[version] ^= 1;
    const struct ds_pad_profile *pad = &ds_pad_profiles[version];
    *(uint32_t *) pad->entry[7] ^= 1;
    ds_pad_install();
    assert(!fixture_hook_calls && !atomic_load(&ds_pad_ready));
    *(uint32_t *) pad->entry[7] ^= 1;
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++)
        *(uint32_t *) ds_models[model].writer[version] ^= 1;
    ds_models_install();
    assert(!fixture_hook_calls);
    for (unsigned model = 0; model < DS_MODEL_COUNT; model++) {
        assert(!atomic_load(&ds_model_ready[model]));
        *(uint32_t *) ds_models[model].writer[version] ^= 1;
    }
}
static void fixture_install_failures(void) {
    for (unsigned failure = 1; failure <= 2; failure++) {
        fixture_hooks_reset();
        fixture_hook_failure = failure;
        ds_tempo_install();
        assert(fixture_hook_calls == failure && !atomic_load(&ds_tempo_ready));
    }
    for (unsigned failure = 1; failure <= 10; failure++) {
        fixture_hooks_reset();
        fixture_hook_failure = failure;
        ds_pad_install();
        assert(fixture_hook_calls == failure && !atomic_load(&ds_pad_ready));
    }
    for (unsigned failure = 1; failure <= 2; failure++) {
        fixture_hooks_reset();
        fixture_hook_failure = failure;
        ds_models_install();
        assert(!atomic_load(&ds_model_ready[0]));
        assert(atomic_load(&ds_model_ready[1]) && atomic_load(&ds_model_ready[2]));
    }
}
static int gui_hook_installation(void) {
    const uintptr_t low = 0xa00000, high = 0x3200000;
    void *mapped = mmap((void *) low, high - low, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped == (void *) low);
    g_ep_img_lo = low;
    g_ep_img_hi = high;
    fixture_hook_enabled = 1;
    g_ep147_build = 0;
    fixture_install_all();
    assert(!fixture_hook_calls);
    for (unsigned version = 0; version < 2; version++) {
        g_ep147_build = version ? EPB_V200 : EPB_V130;
        fixture_signatures(version);
        fixture_install_rejections(version);
        fixture_install_failures();
        fixture_hooks_reset();
        fixture_install_all();
        assert(fixture_hook_calls == 18);
        assert(atomic_load(&ds_tempo_ready) && atomic_load(&ds_pad_ready));
        for (unsigned m = 0; m < DS_MODEL_COUNT; m++) assert(atomic_load(&ds_model_ready[m]));
    }
    assert(!munmap(mapped, high - low));
    puts("PASS both firmware profiles validate GUI writer signatures and disposal targets, reject unsupported images and publish readiness only after every required hook succeeds");
    return 0;
}
