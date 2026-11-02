/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#define dbg(...) ((void)0)
#include "../../src/azmod/inline_hooks.c"
int slip_getter_test_patch(uintptr_t address,void *replacement,void **original,void *context) {
    (void)context;return azm_install_island_hook_at(address,replacement,original,"slip-getter-test");
}
