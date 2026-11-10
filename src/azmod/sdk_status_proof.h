/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_PROOF_H
#define SDK_STATUS_PROOF_H
#include <stdatomic.h>
/* Private, optional diagnostics. Context is copied synchronously by the reducer;
 * it never changes admission, publication, or source ownership. */
enum { DS_PROOF_GAP = 1, DS_PROOF_BASELINE, DS_PROOF_RETIRE, DS_PROOF_RESYNC };
enum {
    DS_PROOF_DATABASE = 1, DS_PROOF_FILESYSTEM = 2, DS_PROOF_FILE_CHANGE = 4,
    DS_PROOF_READ = 8, DS_PROOF_SOURCE = 16, DS_PROOF_BOUNDARY = 32
};
struct ds_proof_context {
    unsigned kind, cause;
    uint64_t binding_cut, binding_serial, connection, db_before, db_after;
    uint64_t device, inode, mount_id, file_device[2], file_inode[2], file_signature[2];
    uint64_t topics, providers[4], revisions[AZ_STATUS_TOPIC_COUNT];
};
static _Thread_local struct ds_proof_context ds_proof_context;
static _Atomic unsigned ds_proof_enabled __attribute__((unused));
#endif
