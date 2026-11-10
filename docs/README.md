# Documentation

How AZ-MODS works, for building on it, contributing to it, or writing tools that
interoperate with it.

| Start here | For |
|---|---|
| [BUILDING.md](../BUILDING.md) | building the source and checking that it reproduces a release |
| [ARCHITECTURE.md](../ARCHITECTURE.md) | how the two libraries are organized and work inside the deck |
| [deck/README.md](../deck/README.md) | the deck-side scripts, how they reach a deck, and their tests |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | bug reports and pull requests |
| [AZ-MODS SDK](https://github.com/Kyle-Hosman/az-mods-sdk) | writing a community mod, and the [Guidelines](https://github.com/Kyle-Hosman/az-mods-sdk/blob/main/docs/GUIDELINES.md) every mod and this code follow |

## File formats on the USB stick

| Document | Specifies |
|---|---|
| [USB-STICK.md](USB-STICK.md) | the folders AZ-MODS uses, every file under `MODS/`, how the deck chooses a USB and loads the mods at boot, the restart policy, the crash-loop guard, and every settings file the release builds read |
| [STEMS-FORMAT.md](STEMS-FORMAT.md) | `STEMS/`: how a folder is matched to a track, the stem files and their encoding, `source.json`, writing a folder safely |
| [DRUM-ROLLS-FORMAT.md](DRUM-ROLLS-FORMAT.md) | `DRUM ROLLS/`: banks, sample naming and order, accepted audio, writing a bank safely |
| [MOD-MENU-KEYS.md](MOD-MENU-KEYS.md) | the stable key of every official Mod Menu setting, as stored in `azmod.conf` and read by community mods |

## Systems

| Document | Explains |
|---|---|
| [UPDATES-AND-SIGNING.md](UPDATES-AND-SIGNING.md) | how a mods release travels from the release channel to a deck: the assets, the signed manifest, verification on the deck and in the desktop app, version rules, running a separate channel |
| [COMMUNITY-MODS-INTERNALS.md](COMMUNITY-MODS-INTERNALS.md) | the deck-side machinery behind Community Mods: the fetcher, its cache and catalog, the install pipeline, the core's checks, quarantine and crash blame |
| [LIBRARY-EDITING.md](LIBRARY-EDITING.md) | how Playlist Editing changes a rekordbox library on the USB and keeps `exportLibrary.db` and `export.pdb` consistent; the desktop app's backup and repair |
| [STEMS-DESIGN.md](STEMS-DESIGN.md) | how stems integrates controls, audio and native displays; design lessons, current limitations and service recommendations for the public SDK |
| [REMOTE-STEMS.md](REMOTE-STEMS.md) | making stems on a computer from the deck: discovery, pairing, the HTTP protocol, the security model |

## Working on the source

| Document | Covers |
|---|---|
| [FIRMWARE-SUPPORT.md](FIRMWARE-SUPPORT.md) | how one build runs on several deck firmwares, what happens on an unknown one, and how to add the next |
| [TESTING.md](TESTING.md) | every level of testing, from the host tests to the developer emulator and a real deck |
| [LOGS.md](LOGS.md) | every log and status file on the deck, what is always written and what the diagnostics flag adds, limits, and collecting logs for a bug report |

Each document names the source files it describes. Where a document and the code disagree,
the code is what runs.
