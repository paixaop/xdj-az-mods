# Firmware support

One `azmod.so` and one `azstem.so` run on every XDJ-AZ firmware AZ-MODS supports: each
library identifies the running `EP147` (the deck's main program) by md5 and selects that
build's address table, and on any other build stays inert. Community mods are tied to
firmware through tags rather than tables; for them see
[Hooks and firmware tags](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/05-hooks-and-firmware.md)
in the SDK. The design principles behind this are in [ARCHITECTURE.md](../ARCHITECTURE.md)
(section 2), the start-up sequence in section 5.

| File | What it holds |
|---|---|
| `src/common/ep147build.h` | the known builds (`EPB_KNOWN`) and `ep147_identify()`, shared by both libraries |
| `src/azmod/ep147addr.h`, `src/azstem/ep147addr.h` | the per-build address tables and `ep147_addr_init()` (generated) |
| `src/azmod/azmod.c` | `azmod_init()` (identification), `ep_off()`, `azmod_fw_tag()`, `pngpatch_apply()` |
| `src/azstem/azstem.c` | `azstem_init()` (identification) |
| `src/azmod/folder_preview.c` | the worked example of a feature that exists on one build only |
| `src/azmod/ep147png.h`, `tools/ep147-pngtable.py` | each build's PNG image table, which bounds every art write |

"Must" marks what the code enforces or depends on; "should" marks the conventions the
existing code follows.

## Builds, not version numbers

The mods hook `EP147` at fixed addresses, so what matters is the exact `EP147` binary, not
the firmware version a DJ sees. Two firmwares that ship the same `EP147` are one build; a
firmware that changes `EP147` in any way is a new build.

| Build | Firmware | `EP147` size (bytes) | `EP147` md5 |
|---|---|---|---|
| `1.30` | 1.30, and the AZ-MODS firmware images built from it (which leave `EP147` unchanged) | 57874328 | `3518275e8b4d5d63636d8d7b4468d7ba` |
| `2.00` | 2.00 and 2.01 (the same `EP147`) | 62461736 | `8daa1de55d7328f458f6d855cdc3cdde` |

The build name is what the logs and the art patch file names use.

## Identification

Both constructors do the same, right after the process filter (which returns unless
`/proc/self/comm` is `EP147`):

```c
int b  = ep147_identify("/proc/self/exe", md5, &sz);
int ok = ep147_addr_init(b);
```

`ep147_identify()`:

1. Opens `/proc/self/exe` and reads its size. A size that matches no `EPB_KNOWN` entry is
   `EPB_UNKNOWN` at once, without hashing.
2. Maps the file and computes its md5 with the MD5 in the same header (no library
   dependency), once per library, before `EP147`'s `main`.
3. Returns the entry's id (`EPB_V130`, `EPB_V200`) only when size and md5 both match.
   Anything else is `EPB_UNKNOWN`.

The md5 here identifies which known build is running, so the right address table is used. It
is not a security check: replacing `EP147` already takes root on the deck, and everything the
deck loads from a USB is checked with SHA-256 and a signature ([UPDATES-AND-SIGNING.md](UPDATES-AND-SIGNING.md)).

The md5 is the number `md5sum /home/root/pdj/EP147` prints on the deck, so a build can be
identified over SSH without running the mods.

Each library logs the result:

| Library | Log | When |
|---|---|---|
| azstem | `/tmp/azstem-build.log` (rotated to `.1` past 16 KB) | every start |
| azmod | `/tmp/azmod-build.log` | only with `/home/root/settings/azmod-logs.on` ([TESTING.md](TESTING.md)) |

```
[1767225600] EP147 2.00 md5=8daa1de55d7328f458f6d855cdc3cdde size=62461736 -> stems ON
[1767225600] EP147 UNKNOWN md5= size=60000000 -> INERT (unknown EP147 build or incomplete address table)
```

The outcome is `stems ON` (azstem), `mods ON` (azmod) or `INERT (...)`. An unknown size is
never hashed, so its md5 field is empty.

### The community firmware tag is separate

The Community Mods host derives a second identity, the firmware tag `ep147-<16 hex digits>`
(`cm_derive_fwtag()` in `src/azmod/community.c`): an FNV-1a 64-bit hash over `EP147`'s
executable `PT_LOAD` segments. It is computed for any build, so mod authors can name a build
without a table in the core. The core's own addresses never depend on it; they depend on
the md5 table above.

## The address tables

Code never writes an `EP147` address as a number. It names each by its value in build 1.30,
with an `EP_` prefix:

```c
#define DJPUCTOR_DEFAULT EP_14a2560
```

Each library's generated `ep147addr.h` holds:

| Item | Content |
|---|---|
| `enum { EPI_<1.30 hex>, ..., EPI_N }` | one index per name the library uses |
| `ep147_addr_v130[EPI_N]`, `ep147_addr_v200[EPI_N]` | one `static const uint32_t` array per known build, in enum order |
| `ep147_addr[]` | the running build's table, all zeros until initialized |
| `ep147_addr_init(build)` | copies the array for `build` into `ep147_addr[]` |
| `#define EP_<1.30 hex> ((uintptr_t)ep147_addr[EPI_<1.30 hex>])` | one per name |

So `EP_14a2560` is `0x14a2560` on 1.30, the same function's address on 2.00, and 0 before
identification or on an unknown build.

`ep147_addr_init()` returns 1 only when the build is known **and every entry of its array is
non-zero**. One missing address makes the whole library inert on that build: a library that
ran with some of its addresses would call through a zero, or through another build's value.

Addresses that cannot be constant initializers (the table is filled at run time) are
assigned right after a successful `ep147_addr_init()`: in `ep147_late_init()` in azmod, and
inline in `azstem_init()` in azstem.

### Basis comments

The 1.30 array is the list of names. Every entry in the 2.00 array carries a comment stating
how its value was established:

| Comment | Meaning |
|---|---|
| `code w=N` | the function's instructions at the 1.30 address were found exactly once in the new build over a window of N instruction words, with every PC-relative field (branch targets, `ADR`/`ADRP` pages, literal loads, the `:lo12:` offsets that consume an `ADRP`) masked out. A function that only moved matches; one that was recompiled does not. |
| `xref a/b agree` | of the b instructions in 1.30 that point at the address, a were relocated into the new build, and all point at the same new address |
| `ptr N agree` | N data words in 1.30 hold the address (vtable slots, tables); relocated, they all hold the same new value |
| `vtable <mangled class> +0xNN` | resolved through the class's RTTI name to its type info and vtable address point in the new build |
| `string '...'` | a string literal found exactly once in the new build's `.rodata` |
| `hand: ...` | resolved by hand; the text states the evidence |

Several bases on one line, separated by `|`, all agree. Globals and `.bss` data that have
none of these anchors are never guessed: such a name is resolved by hand.

### How the tables are produced

The headers start with "Generated per firmware build. Do not edit by hand."
Run `python3 tools/ep147-addrgen.py` to generate them from the reviewed
`tools/ep147-addresses.json` catalog and the exact stock ELFs in the sibling
`anchor/firmware/` directory. Pass `--firmware-dir` to use another directory.
`python3 tools/ep147-addrgen.py --check` validates without rewriting files.
The local tool validates explicit mappings; it does not discover new addresses.
It guarantees:

- One entry per `EP_` name in the library's code (comments and string literals ignored), and
  the separately reviewed cache-entry catalog; stale ordinary references are removed.
- A name without a value for every known build fails generation; the zero check in
  `ep147_addr_init()` is the second line of defense.
- A check mode fails when a committed header differs from a fresh generation.
- An `EP147` address composed with `movz`/`movk` in inline assembly is refused: assembly
  receives addresses from C, through `EP_` names, or the table would not see them.
- Both stock image identities must match. Cache entries additionally require their
  reviewed executable ranges, prologues and complete function bytes. A matching
  address does not establish its ABI, ownership or safe installation.

Run `python3 tests/unit/ep147_addrgen_test.py` for the two-firmware generation and
rejection regressions. These require the stock ELFs and are separate from asset-free tests.

To add or change an address, open an issue or pull request with the value for **every**
supported build and the evidence for each, in the form of the basis comments
([ARCHITECTURE.md](../ARCHITECTURE.md), section 9).

## `ep_off()`: layouts that moved

A new build can also change what is not an address: a field offset, a virtual function's
slot index, a record size, a list column id. azmod handles those with one helper in
`src/azmod/azmod.c`:

```c
static int ep_off(int v130, int v200) {
    return g_ep147_build == EPB_V200 ? v200 : v130;
}
```

Current uses, each next to a comment naming the instruction that reads the value:

| File | Value | 1.30 | 2.00 |
|---|---|---|---|
| `stem_marks.c` | browse reply record size | `0x70` | `0x78` (2.00 appended a member) |
| `stem_marks.c` | track list TITLE column id | 4 | 5 (2.00 inserted a column) |
| `extras.c` | vtable slot of `setText` in `TrackInfo{2,4}DeckWidget` / `SimpleWaveform{2,4}DeckWidget` | 14 / 13 | 15 / 14 (2.00 inserted a virtual before it) |
| `extras.c` | offset of the track info widget's `juce::Component` subobject | `0x170` | `0x1d8` |
| `playlist_editor.c` | a library server field | 448 | 456 (the class grew 8 bytes) |
| `crash_guards.c` | two fields of a login controller | `0x80`, `0x98` | `0x90`, `0xa8` |
| `folder_preview.c` | 2.00-only addresses ([below](#features-that-exist-on-only-some-builds)) | 0 | the address |

`ep_off()` returns the 1.30 value for any build that is not 2.00. That is correct only
because an unknown build never gets this far; `azmod_fw_tag()` beside it is the same. A
`_Static_assert` that `EPB_KNOWN` has exactly two entries, next to them, fails the build
until both have a case for a new build.

azstem has no per-build offsets: every offset it reads is one constant both builds share.

## On an unknown build the mods stay out

There is no fallback to another build's table. A wrong address is a crash in someone's set;
a deck that behaves stock is not.

| Library | When `ep147_addr_init()` returns 0 |
|---|---|
| azmod | `azmod_init()` returns with `g_azm_inert` still set, before every hook, thread and art patch. The overlay thread never starts, so there is no Mod Menu row, no LED change and no Community Mods (the host is started by the overlay thread). The functions azmod interposes (the libc calls in `bus_interpose.c`, and the ALSA and SQLite calls elsewhere) are still bound, as a preloaded library's exports always are; while `g_azm_inert` is set each forwards the call unchanged. |
| azstem | `azstem_init()` returns before installing a hook or starting a thread |

The deck looks and behaves as stock; the only traces are the `INERT (...)` build log lines.

A set that crashes `EP147` anyway (a wrong table entry, a broken build) is caught by the
loader's crash-loop guard, which does not depend on identification:
[USB-STICK.md](USB-STICK.md#the-crash-loop-guard). Community mods have their own start
quarantine inside the core ([COMMUNITY-MODS-INTERNALS.md](COMMUNITY-MODS-INTERNALS.md#quarantine-and-crash-blame)).

## Features that exist on only some builds

The table rule (one zero makes the library inert) is right for an address the library needs
on every build, and wrong for a feature that exists on one: the address has no value on the
other builds, and leaving the library inert there would take every other mod with it. Such a
feature keeps its addresses **out of the table** and writes them with `ep_off()`, with 0 for
every build where it does not apply. Its installer treats 0 as "not on this build" and
returns before touching anything.

`src/azmod/folder_preview.c` is the example. It fills the Browse preview pane in a case where
stock 2.00 leaves it blank; 1.30 does not have the bug:

```c
static const struct { int v200; const char *what; } FP_EVENTS[] = {
    {0x3161800, "tap"},          /* CurrentListRowSelectEvent */
    ...
};
static uintptr_t fp_vt(int i) { return (uintptr_t) ep_off(0, FP_EVENTS[i].v200); }
#define FP_CREATOR_VT ((uintptr_t) ep_off(0, 0x317f748)) /* gui::disp_data::BrowseDispDataCreator */

static void install_folderpreview(void) {
    if (g_ep147_build != EPB_V200 || !fp_vt(0) || access(FP_OFF_FLAG, F_OK) == 0)
        return; /* 2.00 only */
    /* every slot must hold the base handler before any is touched: all or nothing */
    ...
```

The pattern a new build-specific feature should follow:

| Rule | In `folder_preview.c` |
|---|---|
| 0 means absent | every address is `ep_off(0, ...)`, and the installer checks it |
| Verify memory before patching | each vtable slot must hold the expected base handler (read with `img_q()`, which only reads `EP147`'s own image); nothing is installed unless every check passes |
| Check identity at use | the hook acts only on objects whose vtable is one of the expected ones, and otherwise lets `EP147` behave as stock |
| Kill switch | `/home/root/settings/azmod-folderpreview.off` |

These addresses are outside the generated tables and the generator's checks, so a reviewer
verifies each by hand. They should stay few, each next to a comment naming what it is; the
table is the place for anything the library needs on every build.

Two per-build data sets follow the same all-or-nothing idea:

| Data | Rule |
|---|---|
| Art patches | a release's `azmod-pngpatch/` holds blobs `N.bin` with `N.addr` (1.30) and `N.addr.<build>` (for example `N.addr.2.00`) for every other build. `pngpatch_apply()` skips the whole set unless every blob has an address file for the running build, and then refuses any write that does not start exactly at one of that build's PNG images inside `.rodata`, with a PNG that fits it. The images are listed per build in `src/azmod/ep147png.h`, generated from the pristine binaries by `tools/ep147-pngtable.py`. |
| USB asset mods | a folder `MODS/<name>/` with a `mod.txt` naming `target=<tag>` and `N.bin` / `N.addr` pairs, applied only when the tag equals `azmod_fw_tag()` (`v135` on 1.30, `v200` on 2.00) and held to the same image-table check. An asset address is meaningful only for the build it came from, so a new build needs a new tag, and existing asset mods then stop applying. The loader runs only with `/home/root/settings/azmod-usbmods.on`, through `AZ_PROBE_ON()`, so it is compiled out of a release build (`AZMOD_PROBES` unset). |

## Identity checks at use

[ARCHITECTURE.md](../ARCHITECTURE.md), section 2, states the rule: `EP147` rebuilds widgets
and other objects on its own schedule (a track load rebuilds the pad tiles), so a pointer
captured earlier may point at freed memory or at a different object (the SDK's
[Guidelines](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/GUIDELINES.md#1-unknown-means-do-nothing)
give the reasons). How the code handles it (`src/azmod/ep_mem.h`):

| Reader | May read |
|---|---|
| `own_q()` / `own_qv()` | an object alive for the whole call: a hook's argument (or an object reached from it through `EP147`'s own fields) on the thread that owns it, during `EP147`'s call, or an object whose lifetime azmod tracks. A plain aligned 8-byte load; it refuses addresses below `0x10000`, above the 48-bit user range or not 8-byte aligned, and cannot tell a live object from a dead one, so the caller must already know. |
| `img_q()` | `EP147`'s own image (code, vtables, static data), mapped for the life of the process |
| `own_str()` | a NUL-terminated string owned by such a live object |
| `crash_peek8()` | after a crash only, inside the fatal-signal handler (the crash reporter's frame walk) |

A cached `juce::Component` pointer is held in a **wref** slot (`wref_track()`, `wref_set()`,
`wref_get()`): the hook on `juce::Component`'s destructor clears every slot holding the
component before its memory is freed, and without that hook nothing may be cached. A vtable
check then confirms what the object is. In `src/azmod/menu_popup.c`:

```c
static int mp_valid(uintptr_t p, uintptr_t want) {
    uintptr_t vt = 0;
    return p && !(p & 7) && own_q(p, &vt) && vt == want;
}
```

The rules that follow:

- **No raw cached pointers.** A cached pointer is cleared by its object's destructor (a
  wref slot, or a destructor hook as for `DjPlayerUnit` below), checked against the
  expected vtable at use, and zeroed when a check fails.
- **The expected vtables are table entries.** `MP_CTRL_VT`, `IMAGESCOMP_VT` and the like
  resolve through `EP_` names, so a check is per build automatically.
- **On a new build, a failing check is the cheap symptom.** A wrong vtable entry makes the
  feature do nothing instead of crashing. A feature that does nothing on a new build is a
  table or layout error to find.

## The deck comes from the constructor

Code that needs to know which deck an object belongs to takes the deck number from the
constructor that receives it as an argument, never from the order in which objects arrive.
Objects are created and rendered in orders that change with the screen, the load sequence
and the build, and other objects share the same code paths (six audio units run the deck
render function: four `DjPlayerUnit` decks and two `AudioRecorderUnit`s). The reasons are in
the SDK's [Guidelines](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/GUIDELINES.md#1-unknown-means-do-nothing).

The worked example is `DjPlayerUnit(PlayerId)`, hooked in `src/azstem/deck_track_map.c`:

- The constructor receives a `PlayerId` that carries the deck number twice (a byte at `+0`
  and a 32-bit word at `+8`). The number is accepted only when both copies agree and it is
  below the number of decks.
- It runs at `EP147` start, before the first audio block, so the four decks own their slots
  in `g_deck_self[]` before a unit that cannot identify itself could claim one.
- The slot is published only after the original constructor returns, and only the pointer
  is stored.
- The destructor hook clears the slot before the original destructor runs, so a later
  reader finds unknown instead of freed memory.

Other identities come the same way: the track from the `gui::LoadState` constructor, which
receives the deck (`deck_track_map.c`), and the per-deck widgets from their own
constructors ([ARCHITECTURE.md](../ARCHITECTURE.md), section 2). A lookup that cannot answer
returns unknown and the caller does nothing. Per-deck state is an array indexed by deck,
never a single global.

For firmware support: a constructor's address is a table entry like any other, but its
**argument layout** is not. On a new build, check that each hooked constructor still
receives the deck where the hook reads it.

## Adding firmware X.YZ

In dependency order. "X.YZ" stands for the new build name.

1. **Get the new `EP147` identity.** On a deck running stock X.YZ:
   ```sh
   md5sum /home/root/pdj/EP147
   wc -c < /home/root/pdj/EP147
   ```
   If the md5 equals an existing entry (as 2.01 equals 2.00), the build is already
   supported: stop here.
2. **Record the control.** Run the current mods on X.YZ with `azmod-logs.on` set (azmod's
   build log needs it) and confirm that both build logs say `UNKNOWN ... INERT` and the deck
   behaves stock. This proves the identification step can
   say no on this firmware.
3. **Add the build.** In `src/common/ep147build.h`, add `EPB_V<XYZ>` to the enum and a row
   `{ md5, size, EPB_V<XYZ>, "X.YZ" }` to `EPB_KNOWN`.
4. **Resolve every address.** For every name in both `ep147addr.h` headers, establish the
   X.YZ value and its basis. Add an `ep147_addr_v<xyz>` array to each header and extend the
   build selection in each `ep147_addr_init()`. No entry may be zero; a name that cannot be
   resolved keeps the library inert on X.YZ until it is.
5. **Extend `ep_off()`.** Step 3 trips the `_Static_assert` beside `ep_off()`; it stays
   tripped until this step and the next are done and its count is raised. Give `ep_off()` a
   value for X.YZ and update every caller (`grep -n "ep_off(" src/azmod/*.c`). Changing its
   signature makes every unconverted call fail to compile; the two-value form returns the
   1.30 value for any build that is not 2.00.
6. **Review every build comparison** (`grep -n "g_ep147_build\|EPB_V" src/azmod/*.c`):
   - `azmod_fw_tag()`: give X.YZ its own asset mod tag (it returns `v135` for anything that
     is not 2.00);
   - `install_folderpreview()`: decide whether X.YZ has the blank preview bug; if it does,
     give the feature X.YZ addresses, otherwise leave them 0;
   - the art patch suffix in `pngpatch_apply()` is automatic (`N.addr.X.YZ`).
7. **Check what is not in any table.** Field offsets, vtable slot numbers, record sizes and
   argument registers written as plain constants, in both libraries. Read each hooked
   function on X.YZ and confirm the hook's signature and every offset it reads. A value that
   moved becomes an `ep_off()` use in azmod; azstem has no per-build mechanism, so a
   moved value there needs one added.
8. **Check every hook installs.** The installer declines a function whose first
   instructions it cannot relocate ([ARCHITECTURE.md](../ARCHITECTURE.md), section 5). A
   function recompiled for X.YZ may start differently; a declined hook is a feature that is
   off, so look for it explicitly.
9. **Release data.** Art patches need an `N.addr.X.YZ` for every blob, or the whole set is
   skipped on X.YZ (native art is kept). Add X.YZ to `tools/ep147-pngtable.py` and
   regenerate `src/azmod/ep147png.h`: a build with no image table refuses every art write.
10. **Community Mods.** Nothing changes in the core: the firmware tag is computed for any
    build. Mods declared `"any"` load on X.YZ; mods with hooks need new tags and addresses
    from their authors.
11. **Deck scripts.** They test for capabilities rather than versions (the SSH hook uses the
    firmware's `dropbear` when it exists and the USB's build when not). Confirm the
    loader's assumptions on X.YZ: the `EP147.service` unit name, the `/home/root/settings`
    layout, and the `HuiProcessor` thread the restart check and the crash-loop guard look
    for.
12. **Test.** On X.YZ: both build logs say `X.YZ ... ON`, `NRestarts` stays 0, and every mod
    works ([TESTING.md](TESTING.md), "On a real deck"). Then run the same build on every older
    supported build: adding a table must not change their behavior.
