/* SPDX-License-Identifier: GPL-3.0-only */
/* Reference generator: link against the retained musl runtime, never the mod. */
#include <stdint.h>
#include <stdio.h>
#include <math.h>
int main(void) {
    uint32_t seed = 17;
    const float boundaries[] = {0, -0.0f, 0x1p-149f, -0x1p-149f, 0x1p-126f, -0x1p-126f,
        0.2554128f, -0.2554128f, 0.5493061f, -0.5493061f, 10, -10, 88, -104};
    for (unsigned i = 0; i < 4096; i++) {
        seed = seed * 1664525u + 1013904223u;
        union { float f; uint32_t u; } x;
        x.f = i < sizeof boundaries / sizeof *boundaries ? boundaries[i]
            : (float)((int32_t)(seed >> 8) - 8388608) * 0x1p-16f;
        union { float f; uint32_t u; } e = {expf(x.f)}, m = {expm1f(x.f)}, t = {tanhf(x.f)};
        printf("{0x%08x,0x%08x,0x%08x,0x%08x},\n", x.u, e.u, m.u, t.u);
    }
}
