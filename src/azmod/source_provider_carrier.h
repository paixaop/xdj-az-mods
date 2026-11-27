/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_CARRIER_H
#define AZMOD_SOURCE_PROVIDER_CARRIER_H

#include <stdint.h>

typedef int (*azsp_carrier_patch_fn)(uintptr_t address, void *entry, void **original);

/* Install during EP147 startup, before the firmware constructs its servers. */
int azsp_carrier_install(int build, azsp_carrier_patch_fn patch);

/* Call from the firmware message thread. The core creates the OneLibrary catalog
 * below root before requesting a mount.
 * root is the private carrier directory, with absolute backing paths stable for
 * the life of the firmware process. Mount completion is asynchronous. */
int azsp_carrier_mount(const char *root);
uint64_t azsp_carrier_media(void);
const char *azsp_carrier_root(void);

/* Both sides of this interface use a live native owner. On its own library
 * worker only, the core may resolve its SQLite connection using mlserver_chain. */
uint16_t azsp_carrier_library_media(const void *server);
int azsp_carrier_select(void *source_list_model);

/* On the firmware message thread, queue one read on the owning library worker.
 * AZ_OK means the native receiver took ownership of the message. */
int azsp_carrier_wake(void);

/* On the owning library worker, after servicing core work. A true result
 * consumes and releases the wake, so the caller skips native dispatch. */
int azsp_carrier_consume_wake(void *server, void *message, unsigned sender);

#endif
