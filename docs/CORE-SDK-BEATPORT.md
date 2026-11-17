# Beatport login service

Core 1.2.8 supplies `az_core_v1.beatport_login_service`. Beatport Remember
0.3.0 uses this public service and contains no firmware hooks or native object
pointers. The service supports EP147 2.00 only; unsupported firmware or a failed
native-hook installation returns `NULL`.

## Thread ownership and lifetime

Community `tick` and `post_ui` callbacks run on the overlay thread. They cannot
call EP147's JUCE text editors. Beatport Remember 0.2.1 did that from `tick`;
the observed startup crash traversed its autofill call and
`TextEditor::setText` into the native rendering stack.

`sdk_beatport.c` installs native hooks before EP147 starts. The constructor
records the controller and registers its widget with the core's component
destruction tracking. Complete/deleting destructor variants, including the
independently compiled secondary-base variants, clear the controller.
Only the message-thread timer callback reads or fills the editors or submits
LOG IN. A screen generation is checked after callbacks, so destruction and
reconstruction at the same address cannot continue an old autofill operation.

The controller may be constructed while its login screen is hidden. Autofill,
capture and submission require a visible widget and visible parent chain.
Opening or hiding the screen advances its generation; opening starts a fresh
delay. This prevents a hidden startup fill from consuming the attempt before
the native screen clears its editors and becomes visible.

The EP147 2.00 adapter uses these verified entry points:

| Address | Role |
| --- | --- |
| `0x1b8e3a8` | Login controller constructor |
| `0x1b8dfa8` | Widget-name dispatch, including LOG IN |
| `0x1b8e988`, `0x1b8ef30` | Complete and deleting controller destructors |
| `0x1b8f110`, `0x1b8ed48` | Destructors through the secondary base at `+0x8` |
| `0x1b8f2e8`, `0x1b8eb60` | Destructors through the secondary base at `+0x30` |
| `0x263f4e0` | Editor text getter; nontrivial String result through AAPCS64 `x8` |
| `0x2647670` | Editor text setter |

The dispatch argument is a widget-name String, not a Button pointer. Capture
checks the LOG IN name and button state before copying a complete pair. Mask,
clear, and LOG OUT actions never save credentials. Automatic submission uses
the firmware's own LOG IN name and requires both editors to still match the
pair filled by this service.

The first configuring mod owns the service. Configuration, status and Forget
are restricted to that live mod's overlay callbacks (or its initialization).
Disabling or suspending the mod stops native work. Preferences and Forget
invalidate pending automatic work. The short native dispatch is ordered against
accepted configuration/Forget requests; an overlapping request returns `-1` and
can be retried. An editor call already in progress may finish; its return checks
the control generation before continuing autofill or submitting anything.
No native callback runs while holding the storage-state mutex.

## Persistence

A dedicated worker reads and writes
`/home/root/settings/azmod-community/beatport-remember.dat`. It preserves the
legacy `user=<hex>` / `pass=<hex>` format. The API exposes a saved username and
storage status, never a password.

Both fields must be nonempty and at most 127 bytes. Parsing rejects partial,
duplicate, malformed, embedded-NUL, and overlong fields instead of truncating
them. Saving writes a private temporary sibling, completes short/interrupted
writes, syncs the file, renames it, and syncs the directory. Failures before
rename preserve the previous file. A directory-sync failure after rename is
reported as uncertain durability. File permissions are `0600`.

Only completed operations change committed status. Pending load/save/delete
and storage errors are visible to the mod. A capture cannot overwrite an
accepted Forget request. A failed save can be retried by pressing LOG IN again;
a failed deletion can be retried from the mod page. No credential value is
written to logs.

At startup the adapter also removes the known stale, unattached, mode-0600
`0x340`-byte cipher shared-memory segment left by EP147 across process restarts.
Attached segments and other sizes/modes are left alone.

## Tests

`tests/unit/beatport_credentials_run.sh` tests legacy round trips, malformed
files, bounds, permissions, partial writes and commit failures with plain and
ASan/UBSan builds. `tests/unit/sdk_beatport_run.sh` exercises native-thread
ownership, lifecycle and cancellation, hidden startup and reopening, capture selection, and worker
persistence with synthetic credentials. The SDK and community-mod repositories
also test the public service contract and the mod's page callbacks.

These tests establish local behavior. A physical-deck acceptance check must
separately verify login capture, restart persistence, native autofill, and
successful Beatport authentication with the owner's account.
