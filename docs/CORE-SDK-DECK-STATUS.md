# Passive deck status

The observation table publishes INITIAL, CHANGE and RESYNC images on the UI
thread. Each topic exposes its own field validity, producer, revision and
scope. A native load can supply status before a saved edition or library
reference has been qualified. Use the
[SDK subscription guide](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/17-deck-status.md)
for the client contract.

## Field qualification

| Topic | Lifetime and source |
| --- | --- |
| LOAD and METADATA | A matched native load receipt establishes the epoch. A native source/connection and file join qualify saved data for that epoch. A native duration has its source sample clock. A saved duration fallback has library precision. |
| POSITION and LOOP | Current transport receipts provide source sample coordinates and clock. Working bounds, retained endpoints and engagement have separate validity. Slip can retain both audible and background coordinates. |
| CUES, GRID, PHRASES and OVERVIEW | Native and stored sets carry separate revisions. File reads validate source, file edition and extent before publication. Keep nested arrays tied to their status image or snapshot. |
| SETTINGS, SYNC and KEY | Valid receipts distinguish Slip, tempo and Sync settings, following and lock, and original and effective key. |
| PHRASE_TRANSITION | Qualified motion and discrete transport actions share an ordered load-scoped stream. The reducer follows the current phrase revision and publishes the applicable START, ITEM, GAP and END endpoints. |

Require READY before using a value. EMPTY confirms absence on the qualified
scope. Failing to observe a fresh value can withdraw validity. A temporarily
unavailable read does not establish load retirement.

## Implementation and checks

The [native capture guide](CORE-SDK-STATUS-NATIVE.md) details writer
qualification. [Native sources](CORE-SDK-NATIVE-SOURCES.md) explains
connection and file ownership. Use the core reducer and file worker in
`src/azmod/sdk_status_engine.c` and `src/azmod/sdk_status_store.c` when
examining projection, retention or delivery behavior.

The `/home/root/settings/azmod.deck-status.off` startup flag disables passive
status. The snapshot notification adapters have separate `azmod.native-live.off`
and `azmod.native-loop.off` flags under the same settings directory.

Run `bash tests/sdk-status.sh` and `bash tests/sdk-status-native.sh` for reducer
and producer regressions. Use [SDK integration tests](CORE-SDK-AUTOMATION.md)
to qualify each firmware, source and operation. Pin the process and capture
fresh receipts on the intended logical deck.
