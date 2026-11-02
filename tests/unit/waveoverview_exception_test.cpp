/* SPDX-License-Identifier: GPL-3.0-only */
#include <cstdio>
#include <cstring>
#include <cstdlib>
extern "C" {
int overview_scalar_tests(void);
void overview_exception_start(void (*)(long), int);
void overview_exception_observe(long);
void overview_exception_check(void);
void overview_lease_checks(void);
}
namespace {
struct OriginalError {
    unsigned marker;
};
void throwing_renderer(long self) {
    overview_exception_observe(self);
    throw OriginalError {0x147200u};
}
void run(int mode) {
    bool caught = false;
    try {
        overview_exception_start(throwing_renderer, mode);
    } catch (const OriginalError &error) {
        if (error.marker != 0x147200u) std::abort();
        caught = true;
    }
    if (!caught) std::abort();
    overview_exception_check();
}
} // namespace
int main(int argc, char **argv) {
    bool lifetime_only = argc == 2 && !std::strcmp(argv[1], "--lifetime-only");
    bool throw_only = argc == 2 && !std::strcmp(argv[1], "--throw-only");
    if (lifetime_only) run(2);
    else {
        if (!throw_only && overview_scalar_tests()) return 1;
        const int modes[] = {0, 1, 2, 3, 4};
        for (int mode : modes) run(mode);
        if (!throw_only) overview_lease_checks();
    }
    std::puts("overview exceptions: original propagation, conditional restoration and real retirement passed");
    return 0;
}
