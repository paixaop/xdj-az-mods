/* SPDX-License-Identifier: GPL-3.0-only */
static void test_initial_loop_actions(Slot &slot, uintptr_t core, uintptr_t property, uintptr_t command, uintptr_t callback) {
    unsigned char saved_property[0x100], saved_command[0x100], saved_callback[0x100];
    memcpy(saved_property, reinterpret_cast<const void *>(property), sizeof saved_property);
    memcpy(saved_command, reinterpret_cast<const void *>(command), sizeof saved_command);
    memcpy(saved_callback, reinterpret_cast<const void *>(callback), sizeof saved_callback);
    const unsigned saved_rate = uint32_t(integer(core + 0x68));
    const unsigned saved_n = slot.motion.window_samples, saved_window_rate = slot.motion.window_rate;
    Buffer<0x80> play, play_table;
    const uintptr_t saved_play_table = state().profile.commands[1].table;
    state().profile.commands[1].table = play_table.addr();
    put(play.addr(), play_table.addr()); put(play.addr() + 0x20, core);
    put(callback + 0x18, play.addr());
    struct Case { uint64_t from, to, in, out, second; unsigned frames; };
    /* Actual initial native commands, including a source delta that differs
     * from loop length by one sample. Classification uses the command target,
     * never an inferred exact affine displacement or an enlarged window. */
    const Case cases[] = {{869488, 782742, 778806, 865551, 782524, 44},
                          {2644376, 2568788, 2564592, 2640179, 2568568, 45},
                          {984685, 919638, 908946, 973993, 919422, 44}};
    for (const auto &c : cases) {
        put(core + 0x68, uint32_t(44100));
        put(property + 0x60, uint32_t(44100));
        put(property + 0xb8, uint32_t(44100));
        put(property + 0x40, c.in); put(property + 0x98, c.out);
        put(command + 0x40, c.to); put(command + 0x48, c.second);
        put(callback + 0x50, c.in); put(callback + 0x58, c.out);
        put(callback + 0x80, c.out); put(callback + 0x88, c.in);
        slot.motion.window_samples = c.frames; slot.motion.window_rate = 44100;
        StatusHead from = status_head(core), to = from;
        from.sample = c.from; to.sample = c.to;
        StatusActionScope origin(command, 0);
        assert(status_action(slot, from, to) == AZN_JUMP);
        to.sample = c.second;
        assert(status_action(slot, from, to) == AZN_JUMP);
        to.sample = c.to + 1;
        assert(!status_action(slot, from, to)); // committed target still uses N=1
        to.sample = c.to;
        struct Corrupt { uintptr_t address; uint64_t value; };
        const Corrupt negatives[] = {{command + 0x20, 0}, {command + 0x18, 0},
            {command + 0x28, 0}, {command + 0x30, 0}, {command + 0x50, 0},
            {callback + 0x20, 0}, {callback + 0x28, 0}, {callback + 0x60, 0},
            {callback + 0x18, 0}, {play.addr() + 0x20, 0}, {play.addr() + 0x18, callback},
            {callback + 0x50, c.in + 1}, {callback + 0x80, c.out + 1},
            {command + 0x48, UINT64_MAX}};
        for (const auto &n : negatives) {
            const uint64_t saved = word(n.address);
            put(n.address, n.value);
            assert(!status_action(slot, from, to));
            put(n.address, saved);
        }
        put(command + 0x40, c.out + 1); to.sample = c.out + 1;
        assert(!status_action(slot, from, to));
        /* An altered recurring [IN,OUT] interval is not an initialization
         * witness, even when its changed endpoint is inside the loop. */
        put(command + 0x40, c.in + 1); put(command + 0x48, c.out); to.sample = c.in + 1;
        assert(!status_action(slot, from, to));
        put(command + 0x40, c.to); put(command + 0x48, c.second); to.sample = c.to;
        slot.motion.window_samples = 0;
        assert(!status_action(slot, from, to));
        slot.motion.window_samples = c.frames;
        slot.motion.window_rate = 0;
        assert(!status_action(slot, from, to));
        slot.motion.window_rate = 44100;
        put(command + 0x40, c.to); to.sample = c.to; to.rate = 48000;
        assert(!status_action(slot, from, to));
    }
    memcpy(reinterpret_cast<void *>(property), saved_property, sizeof saved_property);
    memcpy(reinterpret_cast<void *>(command), saved_command, sizeof saved_command);
    memcpy(reinterpret_cast<void *>(callback), saved_callback, sizeof saved_callback);
    put(core + 0x68, uint32_t(saved_rate));
    state().profile.commands[1].table = saved_play_table;
    slot.motion.window_samples = saved_n; slot.motion.window_rate = saved_window_rate;
    puts("PASS owned initial loop JUMP targets, actual source clocks and strict callback/source/endpoint negatives");
}
