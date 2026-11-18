/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    assert(library);
    int (*run)(void) = (int (*)(void)) dlsym(library, "main");
    assert(run && !dlsym(library, "fixture_native_try"));
    assert(run() == 0);
    assert(!dlclose(library));
}
