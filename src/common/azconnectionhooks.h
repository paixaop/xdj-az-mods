/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCONNECTIONHOOKS_H
#define AZCONNECTIONHOOKS_H
#include "azconnectionepoch.h"
struct sqlite3;
typedef int (*az_native_sql_open)(const char *, struct sqlite3 **, int, const char *);
typedef int (*az_native_sql_close)(struct sqlite3 *, int);
typedef int (*az_sql_file_control)(struct sqlite3 *, const char *, int, void *);
struct az_sql_range {
    uintptr_t begin, end;
    unsigned flags;
};
/* Opaque immutable proof of the exact loaded module, prepared before redirects.
 * The retained module reference and proof live through process teardown.
 * They own code lifetime, never a database connection or owner. */
struct az_sql_profile;
#define AZ_SQL_PRIVATE __attribute__((visibility("hidden")))
AZ_SQL_PRIVATE uintptr_t az_sql_profile_base(const struct az_sql_profile *);
AZ_SQL_PRIVATE unsigned az_sql_profile_range_count(const struct az_sql_profile *);
AZ_SQL_PRIVATE const struct az_sql_range *az_sql_profile_range(const struct az_sql_profile *, unsigned i);
AZ_SQL_PRIVATE bool az_sql_profile_contains(const struct az_sql_profile *, uintptr_t address, size_t size, unsigned flags);
AZ_SQL_PRIVATE az_sql_file_control az_sql_profile_file_control(const struct az_sql_profile *);
AZ_SQL_PRIVATE uintptr_t az_sql_profile_open_entry(const struct az_sql_profile *);
AZ_SQL_PRIVATE uintptr_t az_sql_profile_close_entry(const struct az_sql_profile *);
static inline uintptr_t az_sql_profile_io_rva(const struct az_sql_profile *p, unsigned i) {
    (void) p;
    return i == 0 ? 0xb3d98 : i == 1 ? 0xb3e30
                          : i == 2   ? 0xb3ec8
                                     : 0;
}
static inline uintptr_t az_sql_profile_vfs_open_rva(const struct az_sql_profile *p) {
    (void) p;
    return 0x53968;
}
static inline uintptr_t az_sql_profile_read_rva(const struct az_sql_profile *p) {
    (void) p;
    return 0x1513c;
}
static inline uintptr_t az_sql_profile_size_rva(const struct az_sql_profile *p) {
    (void) p;
    return 0x1bdf4;
}
static inline uintptr_t az_sql_profile_control_rva(const struct az_sql_profile *p) {
    (void) p;
    return 0x536b0;
}
/* Startup-only loader/byte validation. No injected production profile, caller
 * trust flag or path override. Unsupported preparation remains unavailable.
 * This calls no database API. */
AZ_SQL_PRIVATE const struct az_sql_profile *az_connection_prepare_profile(void);
struct az_connection_installer {
    /* Publish the callable original BEFORE making its replacement reachable.
     * Return exactly 1 on acceptance. Never retain or mutate the slot later.
     * Failure leaves births unavailable; an installed close keeps retiring. */
    int (*call)(uintptr_t entry, void *replacement, void **original, void *context);
    void *context;
};
/* Opt-in startup dependency. The immutable binding borrows the existing state,
 * which must outlive every hooked call, including DSO/atexit teardown. Close
 * installs first; both originals are required before births are admitted.
 * No disable/reset/unbind operation can stop observing an installed close. */
AZ_SQL_PRIVATE int az_connection_install(struct az_connection_state *, struct az_connection_installer);
AZ_SQL_PRIVATE bool az_connection_ready(void);
#if defined(AZSDK_CONNECTION_HOOKS_TESTING)
/* Synthetic forwarding only: no module proof is minted, one binding/process. */
int az_connection_test_install(struct az_connection_state *, struct az_connection_installer, uintptr_t synthetic_base);
#endif
#undef AZ_SQL_PRIVATE
#endif
