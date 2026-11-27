/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by bus_interpose.c after the mixer handlers. */

static void mixer_route_buttons(uint8_t *b, int crc_in, int menu_open) {
    if (crc_in && sdk_source_browser_filter(b))
        frame_crc_stamp(b, 96);
    mixer_menu_buttons(b, crc_in, menu_open);
}

static void mixer_apply_performance(uint8_t *b, ssize_t r, uint64_t cfg, int crc_in) {
    mixer_drum_tone(b, r);
    mixer_stem_eq(b, r);
    mixer_fx_disarm(b, r);
    int delta;
    int hold = mixer_browse_knob(b, &delta);
    /* Playlist Editing takes effect immediately, while its hooks install only at startup. */
    atomic_store_explicit(&g_reorder_enabled, bus_cfg_reorder(cfg), memory_order_relaxed);
    if (atomic_load_explicit(&g_reorder_enabled, memory_order_relaxed) && !hold)
        mixer_reorder_gesture(b, delta);
    if (knob_anchored)
        b[34] = knob_anchor;
    /* EP147 drops frames whose bytes no longer match their CRC. */
    if (crc_in)
        frame_crc_stamp(b, 96);
}

static void mixer_deliver(uint8_t *b, ssize_t r, uint64_t cfg, int crc_in) {
    bus_capture_mixer(b, r);
    mixer_xpad_drum(b, r, cfg);
    int menu_open = mixer_watchdog();
    if (r >= 34)
        mixer_route_buttons(b, crc_in, menu_open);
    if (crc_in && sdk_filter_run(AZ_BUS_MIXER_IN, b, (int) r))
        frame_crc_stamp(b, 96);
}

/* The mixer/panel bus frame: 96 bytes + CRC-16/X25 at 96/97. */
static void bus_mixer_frame(uint8_t *b, ssize_t r, uint64_t cfg) {
    /* Was this frame's CRC valid as it arrived? Every re-stamp is gated on it. Computed
     * before azmod touches a byte. */
    int crc_in = r >= 98 && frame_crc_ok(b, 96);
    bus_capture_mixer_in(b, r);
    /* A mixer frame with a bad CRC is not input. With a PC attached the panel delivers
     * 128-byte all-0xFF reads every few seconds, all CRC-invalid; EP147 drops them. Parsed as
     * buttons, byte 33 = ff is BACK + TAG TRACK + LOAD 1-4 all pressed and byte 34 = ff a
     * knob jump. Worse, a swallow would re-stamp a valid CRC onto the frame and EP147 would
     * act on every button at once. So a frame that fails the CRC is passed through untouched
     * and never read. */
    if (r >= 98 && !crc_in)
        return;
    if (crc_in) /* community mods see only checked input (a short frame carries no CRC) */
        sdk_filter_raw(AZ_BUS_MIXER_IN, b, (int) r);
    if (r >= 35)
        mixer_apply_performance(b, r, cfg, crc_in);
    if (r >= 12)
        mixer_deliver(b, r, cfg, crc_in);
}
