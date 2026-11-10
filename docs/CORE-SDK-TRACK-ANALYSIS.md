# Track Analysis service

`sdk_analysis_service.c` implements the public worker-only source lease and ANLZ
publication entries appended to `az_library_v1`. The public contract is documented in
the SDK's `docs/18-track-analysis.md`. The core and SDK public headers must remain identical.

The service shares existing physical USB qualification, read-only library resolution
and ANLZ parsing. Each lease owns copied source identities, paths and freshness signatures;
no native objects or mod-private firmware calls cross the API. `sdk_analysis_write.h`
provides a bounded, format-only insert/empty-tag replacement used by the publisher.

The community mod owns the analysis algorithm and its portable worker. The core owns
source qualification, validation and persistence. Existing saved maps are never replaced.
Directory descriptors and no-follow component walks constrain creation and rename to
the selected USB. Malformed/unknown tags fail closed; unrelated bytes survive verbatim.

Run `tests/sdk-analysis.sh` in plain and ASan/UBSan modes. Physical validation remains
required. Missing row locators use a deterministic source-local path and a conditional native
transaction spool update after the files are saved. Playback gates stay in force.
Streaming download qualification, active native map publication and immediate phrase display refresh are not implemented by this service.
