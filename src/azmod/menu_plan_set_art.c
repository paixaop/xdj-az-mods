/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by menu_popup.c. Reuse the native face and font, with the same
 * per-state colors and dimensions as the other playlist buttons. */
static void mp_plan_clear(uint8_t *pixels, int stride, int width) {
    int sample = width - 25;
    for (int y = 18; y < 64; ++y) {
        uint8_t *row = pixels + y * stride;
        for (int x = 23; x < sample; ++x)
            memcpy(row + x * 4, row + (sample + ((x - sample) & 7)) * 4, 4);
    }
}

static void mp_plan_icon(uint8_t *pixels, int stride, uint8_t ink) {
    for (int line = 0; line < 3; ++line)
        for (int y = 29 + line * 8; y < 32 + line * 8; ++y) {
            uint8_t *row = pixels + y * stride;
            for (int x = 30; x < 52 - line * 4; ++x)
                memset(row + x * 4, ink, 3);
        }
}

static void mp_plan_glyph(uint8_t *pixels, int stride, int x, FT_GlyphSlot glyph, uint8_t ink) {
    int top = 47 - glyph->bitmap_top;
    for (unsigned y = 0; y < glyph->bitmap.rows; ++y) {
        const uint8_t *sample = glyph->bitmap.buffer + y * (unsigned) glyph->bitmap.pitch;
        for (unsigned x_offset = 0; x_offset < glyph->bitmap.width; ++x_offset) {
            unsigned alpha = sample[x_offset];
            uint8_t *pixel = pixels + (top + (int) y) * stride + (x + (int) x_offset) * 4;
            for (int channel = 0; channel < 3; ++channel)
                pixel[channel] = (uint8_t) ((pixel[channel] * (255 - alpha) + ink * alpha + 127) / 255);
        }
    }
}

static bool mp_plan_glyph_fits(FT_GlyphSlot glyph, int x, int width) {
    return glyph->bitmap_top <= 47 && 47 - glyph->bitmap_top + (int) glyph->bitmap.rows <= 80 &&
           x >= 0 && x + (int) glyph->bitmap.width <= width;
}

static void mp_plan_ink(uint8_t *pixels, int stride, int width, uint8_t ink) {
    static const uint32_t title[] = {'P', 'L', 'A', 'N', ' ', 'S', 'E', 'T'};
    struct mp_gl glyphs[sizeof title / sizeof *title];
    int low = 0, high = 0;
    int count = mp_lay(title, sizeof title / sizeof *title, glyphs, NULL);
    if (!mp_ink(glyphs, count, &low, &high))
        return;
    int inset = (width + 42 - low - high) / 2;
    for (int i = 0; i < count; ++i) {
        FT_GlyphSlot glyph = mp_glyph(glyphs[i].cp);
        if (!glyph)
            continue;
        int x = inset + glyphs[i].x + glyph->bitmap_left;
        if (mp_plan_glyph_fits(glyph, x, width))
            mp_plan_glyph(pixels, stride, x, glyph, ink);
    }
}

static uintptr_t mp_plan_face(const unsigned char *src, int length, int width, uint8_t ink) {
    uintptr_t image = mp_image_uncached(src, length);
    int stride = 0;
    uint8_t *pixels = mp_pixels_w(image, &stride, width);
    if (!pixels) {
        mp_release(image);
        return 0;
    }
    mp_plan_clear(pixels, stride, width);
    mp_plan_icon(pixels, stride, ink);
    mp_plan_ink(pixels, stride, width, ink);
#ifdef AZMOD_HAVE_DAYMODE
    dm_record_own_art((void **) &image);
#endif
    return image;
}

static int mp_build_plan_images(int width, uintptr_t out[3]) {
    static const unsigned char *const sources[2][3] = {
        {ML_NEWPL_W210_normal, ML_NEWPL_W210_down, ML_NEWPL_W210_disabled},
        {ML_NEWPL_normal, ML_NEWPL_down, ML_NEWPL_disabled}};
    static const int lengths[2][3] = {
        {sizeof ML_NEWPL_W210_normal, sizeof ML_NEWPL_W210_down, sizeof ML_NEWPL_W210_disabled},
        {sizeof ML_NEWPL_normal, sizeof ML_NEWPL_down, sizeof ML_NEWPL_disabled}};
    static const int ink[] = MP_FT_INK;
    if (!mp_ft_ready())
        return 0;
    int wide = width == 246;
    for (int i = 0; i < 3; ++i) {
        out[i] = mp_plan_face(sources[wide][i], lengths[wide][i], width, (uint8_t) ink[i]);
        if (!out[i]) {
            for (int j = 0; j < i; ++j)
                mp_release(out[j]);
            memset(out, 0, 3 * sizeof *out);
            return 0;
        }
    }
    return 1;
}
