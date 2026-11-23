# Core stored track data

The SDK [track data guide](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/13-track-data.md)
defines the public service. Core stores immutable USB library snapshots and
bounded waveform windows in a shared worker. Native load observations are
handled by the [status service](CORE-SDK-DECK-STATUS.md). A native cue
observation and a saved cue component can represent different revisions.

## Source identity and publication

`track_service.c` owns tickets, the cache and the event journal.
`track_storage.c` owns the read-only database and analysis-file work. A
qualified physical USB port, mount identity, database identity, source
generation and content ID identify a record. Never reuse a ticket across owners
or a source reference across connection incarnations. The worker checks the
database and required analysis components before publication and during freshness
sweeps. Invalidated results stay resident while borrowed by an existing ticket.

The provider refuses escaped paths, special files, nonempty SQLite sidecars
that would invalidate a read-only snapshot, and files that change during a read.
A missing tag reports unavailable, and an explicit empty tag publishes empty.
A malformed component fails its read. Preserve independent component provenance
and clocks.

## Stored format rules

### Saved cues and loops

* Canonical short PCP2 entries can end at byte 40. A comment-length field is read only
  when all four bytes at offset `0x28` exist. A present malformed length still fails the
  component read. Entry header sizes must match PCP2's 16 bytes or PCPT's 28 bytes.
* A PCP2 memory entry exposes its raw one-byte color ID from offset `0x1c`, including
  zero, under `AZ_CUE_COLOR_ID`. This is distinct from `AZ_CUE_COLOR`, which indicates
  authoritative RGB in the optional hot-cue trailer. Memory trailer bytes are not treated
  as an RGB color. The public `reserved`/`color_id` union retains the cue-entry stride.
  No EP147 RGB palette is inferred from these IDs.
* PCPT loop status values 0 and 1 expose saved `auto_active = 0`; value 4 exposes saved
  `auto_active = 1`. Unknown values leave that property unavailable. Point entries do not
  acquire loop activation state. This is the saved Active Loop property, not current loop
  engagement or a native recall command.
* PCP2 has no equivalent activation field. A selected extended loop can receive that one
  saved property from an exact, unique legacy counterpart. Classification, type, original
  start/end milliseconds and, for hot cues, slot must agree. Duplicate counterparts,
  conflicting statuses, unknown statuses or mismatches leave the property unavailable.
  Memory lists retain DAT precedence, and distinct saved entries are not coalesced by
  their positions. A malformed discarded legacy annotation does not invalidate otherwise
  usable authoritative extended cues.

The field layouts and status enum are documented by the
[ANLZ format research](https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/anlz.html)
and its [primary Kaitai schema](https://github.com/Deep-Symmetry/crate-digger/blob/main/src/main/kaitai/rekordbox_anlz.ksy).

### Raw phrase variants

PSSI offsets `+12` and `+14` are separate 16-bit version and entry-stride fields. Versions
0 and 1 use the bounded existing raw kind/mood parser; the existing XOR variant remains
supported. An unknown version marks only phrases `AZ_AVAIL_UNSUPPORTED`, allowing other
valid requested components to publish. Invalid container/tag lengths remain failures, and
known versions retain their stride/count/payload checks. Normalized phrase semantics remain
unavailable. The split fields are described in
[pyrekordbox's primary format definitions](https://github.com/dylanljones/pyrekordbox/blob/master/pyrekordbox/anlz/structs.py).

### Waveform windows

The provider selects PWAV and PWV2 in DAT, PWV3/PWV4/PWV5 in EXT, and PWV6/PWV7
in 2EX. The layered encoding is derived from PWV7. Detail windows have a
150-column/s source axis. Preview windows use valid library duration divided
by column count. The file provider validates the tag's header, stride and
complete extent before reading a bounded column span. It rejects a duplicate
requested tag. Raw data does not establish display calibration.

See the [SDK test guide](CORE-SDK-AUTOMATION.md#stored-file-checks) for the
asset-independent and optional real-file checks.
