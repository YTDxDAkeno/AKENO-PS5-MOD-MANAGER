# Mod preparation and the compatibility engine

Everything below runs on the PS5 inside Akeno. No PC, Python or desktop tool is involved, and
nothing a mod contains is ever executed. Code: `src/mods/ModPreparation.cpp`,
`src/mods/ArchiveLayout.cpp`, `src/unreal/`, `src/compatibility/CompatibilityEngine.cpp`.

## Pipeline

```
download (HTTPS, size + SHA-256)                                    DownloadManager
  -> secure extraction into staging (no links, no traversal)        SecureExtractor
  -> read-only listing of the installed game's physical folder      games::probeGameTree
  -> archive layout analysis and proposed mapping                   mods::analyzeLayout
  -> content checks on the mapped paths (code, fakelib, sce_sys...) mods::analyzeMod
  -> Unreal container analysis of the extracted files               unreal::analyzeUnreal
  -> the game's own Unreal containers (headers, package ids)        unreal::probeGameUnreal
  -> compatibility assessment and activation decision               compatibility::assess
  -> dry-run plan shown to the user (check screen)                  mods::planInstall
  -> install only when activation is allowed                        install::storeMod
  -> verify every copy, publish the overlay atomically              install::applyOverlay
  -> diagnostic export on demand (Quick / Deep)                     diagnostics::exportReport
```

The check (`runModCheck`) and the installer (`storeMod`) call the same `prepareMod`, so the plan
a user reviews is the plan that is executed. A check report (schema 2) stores the layout, the
Unreal facts, a snapshot of the game, and the assessment. It is re-made when the game's
version changes.

## Four separate answers

| Answer | Values | Means |
|---|---|---|
| Mapping confidence | `none`, `ambiguous`, `candidate`, `likely`, `established` | where the files go |
| Game loading support | `verified`, `expected`, `unverified`, `unsupported` | whether this game reads files from there |
| Platform compatibility | `verified`, `likely`, `unknown`, `needs-conversion`, `incompatible` | whether the content works on the PS5 build of this version |
| Activation | allowed / allowed after confirmation / blocked | whether Akeno may install it into the overlay |

A good path is not a working mod. Copying files, publishing an overlay, and ShadowMountPlus
mounting it are never counted as evidence that the game loaded or accepted a mod. The
installation log keeps the same distinction (stages 5 to 7 are always "not-observed").

## Archive layout rules (`mods::analyzeLayout`)

Applied in this order; the first that applies decides.

1. **manifest** — a curated Akeno catalogue manifest names `archiveRoot` and `targetPrefix`.
   Confidence `established`. The game listing only adds observations (for example a spelling
   that differs from the game's).
2. **unreal** — the archive contains `<Project>/Content/...` (or starts at `Content/`). Every
   folder above the project folder is packaging (`SomeMod/Files/Windows/`). The project folder
   is matched case-insensitively to the game's own (`Dawnwalker` -> `dawnwalker`): `likely`.
   A project named differently from the game's only Content/Paks folder: `candidate`.
3. **game-root** — after removing at most four leading folders that do not exist in the game,
   the archive's folders match the game's top-level folders: `likely` when every file's folder
   exists, else `candidate`. A leading folder that is itself a game folder is never removed.
4. **unreal-flat** — only `.pak/.utoc/.ucas/.sig` files in one folder. On PC, Unreal Engine
   also mounts packages from `<Project>/Content/Paks/~mods`; the target is that folder in the
   game's spelling, as a `candidate` only.

Otherwise there is no mapping. Several project folders or alternative variants
(`Option A/`, `Option B/`) are `ambiguous`; Akeno does not choose. Only leading folders shared
by every file are removed; nothing below the mapped root is flattened. Existing folders take the
game's spelling (PS5 game files are lower-case and the filesystem is case-sensitive); new
folders and file names keep the archive's spelling. Two files that would land on the same path
(also after case folding) invalidate the mapping. Readmes and preview images outside the mapped
content are not installed. The original archive path of every file is kept as `storePath` next
to its `installPath`.

## Unreal Engine analysis (`src/unreal/`)

| Format | Read | Verified |
|---|---|---|
| `.pak` footer, versions 1-11 (UE 4.0 - 5.x) | version, encrypted index, key GUID, compression methods | index SHA-1, secondary index SHA-1 (v10+) |
| `.pak` index (unencrypted) | mount point, entry count, file names (v1-9 entries, v10+ full directory index) | via the hashes above |
| `.utoc` IoStore TOC, versions 1-8 | container id and flags (compressed, encrypted, signed, indexed), chunk ids and types, offsets, compression blocks, methods, directory index, chunk hashes | consistency of every table |
| `.ucas` (uncompressed, unencrypted) | chunk data | every chunk hash recomputed (BLAKE3-160 for v8; BLAKE3 or SHA-1 for older), `.ucas` length against the block table |
| container-header chunk | signature, version, container id, package ids | container id equals the `.utoc`'s |
| Zen package header (UE 5.0-5.2 and 5.3+) | package name, flags (cooked, unversioned), imported package names | chunk id equals CityHash64 of the lower-case UTF-16 name |
| legacy `.uasset` summary | tag, file versions, unversioned | - |

A `.pak/.utoc/.ucas` set is linked only by its file names. Akeno also checks what the format
allows; extensions alone never count. Missing companions (a `.utoc` without `.ucas`, or an
IoStore pair without the `.pak` that mounts it), damaged headers, a truncated `.ucas` or a hash
mismatch make the set damaged (category D). Encrypted or compressed containers (Oodle is not
available on the console) and unknown versions are reported as not inspected and make the
compatibility `UNKNOWN`. Nothing is decrypted, decompressed or converted.

Loader markers: `ue4ss`, `UE4SS-settings.ini`, `LogicMods` (UE4SS BPModLoader), Lua scripts in
mod script folders, Windows executables and libraries, Reloaded-II (`ModConfig.json`),
`dsts-loader`, Fluffy Mod Manager (`modinfo.ini`).

The game side (`probeGameUnreal`) finds the single `<project>/content/paks` folder and reads only
the 144-byte `.utoc` header plus its chunk-id table, and the `.pak` footer. That gives the game's
IoStore and pak versions, whether its containers are signed or encrypted, its compression
methods, and the ids of every package it contains (so a mod's package and its imports can be
looked up).

## Categories and outcomes (`compatibility::assess`)

| Category | Meaning | Examples |
|---|---|---|
| A | portable data: format, path and loading established for this game | curated catalogue entries; PC files covered by a hardware-verified game rule |
| B | potentially portable: needs game-specific evidence, validation or a conversion | PC-cooked Unreal packages without a game rule; loose `.uasset` for an IoStore game |
| C | needs a PC runtime | Windows DLL/EXE, UE4SS, LogicMods, Reloaded-II, dsts-loader, script extenders |
| D | unsupported or dangerous | PS5 code, `fakelib`, `sce_sys`/`sce_module`, executable replacement, damaged or incomplete containers, over-long paths |

Outcomes: `VERIFIED_PS5` (a record for this exact mod, title and game version: the curated
catalogue or a game adapter), `LIKELY_COMPATIBLE`, `EXPERIMENTAL`, `NEEDS_CONVERSION`,
`REQUIRES_UNSUPPORTED_LOADER`, `INCOMPATIBLE`, `UNKNOWN`.

Definite incompatibilities found on the console: the mod's IoStore or pak version is newer than
the game's; the game uses no IoStore containers but the mod is one; the game has no Unreal
content folder at all; `/Game/` packages the mod imports are missing from the installed game
(built for another game version or build); the catalogue marks the title or region as not
supported. The game's containers being signed is recorded as evidence against and as a risk.

### Activation contract

Activation is allowed only when every one of these holds; otherwise each failing rule is named
on the check screen and in the log:

* not category C or D, and no blocking finding;
* a mapping that is not `none`, `ambiguous` or `candidate`;
* for a PC source (Nexus Mods, GameBanana): game loading `verified` or `expected` **and**
  platform compatibility `verified` or `likely` — in this build that requires a game adapter
  with hardware evidence for the installed title and version, and none ships;
* for the curated catalogue: the catalogue rules allow it, and the outcome is `VERIFIED_PS5`,
  `LIKELY_COMPATIBLE` or `EXPERIMENTAL` (EXPERIMENTAL asks for confirmation).

There is no override for these rules. Mods installed by 0.2.0-alpha from a PC source have no
recorded decision; they can be turned off or removed, never re-activated.

## Extension points

### Game adapters (`IGameAdapter`)

Per-title knowledge: which folder this game loads which file types from
(`LoadingConvention`), for which game versions this was observed on hardware, with what
evidence, and whether PC-cooked packages of that kind were shown to work. A matching rule turns
a candidate path into an established one and game loading into `verified` for those versions
only. `modVerified()` records a specific mod version as verified (the only way to
`VERIFIED_PS5` outside the catalogue). `Registry::builtin()` is empty: no title has such
evidence yet.

### Conversion providers (`IConversionProvider`)

A conversion turns PC content into what one PS5 game loads. A provider may be registered only
with a real implementation, its input requirements, validation of its output and tests. It is
offered on the check screen when `canConvert` says it applies. Offering is not converting: the
outcome stays `NEEDS_CONVERSION` until a converted result has been analysed like any other mod.
No provider ships, and no generic conversion of PC-cooked Unreal assets is attempted.

## What is verified where

| Claim | Host tests | Needs a PS5 |
|---|---|---|
| Parsers read the formats above, reject damaged and unknown input | yes (synthetic containers, cross-checked against the real Better Carry Weight files and reference BLAKE3/CityHash implementations) | - |
| Wrapper detection and mapping rules | yes | - |
| Activation gating, legacy PC state, rollback, journal, logging | yes (permission-fault tests need an unprivileged runner) | - |
| Read-only game listing and container probe never write | yes (hashes before/after) | behaviour of PS5 filesystems (nullfs, exFAT, PFS) |
| A PS5 game mounts containers from `~mods` | - | yes, per title and version |
| A PC-cooked package works on the PS5 build | - | yes, per title, version and mod |
| ShadowMountPlus mounted the overlay; the game opened the files | - | yes (SMP debug.log, game behaviour) |

## Roadmap toward game-specific PC-to-PS5 support

Nothing in this list is implemented yet; each step needs evidence before code claims it.

1. **Per-title loading evidence.** For one Unreal title, test on hardware (by the user, outside
   Akeno's automatic paths) whether the PS5 build mounts an extra, unmodified container from
   `<project>/content/paks/~mods` — for example a container that only adds a harmless new
   package. Record firmware, SMP version, game version, the SMP mount line and the observed
   effect. Only then add an `IGameAdapter` rule for exactly that title and version.
2. **Content compatibility per title.** Compare a PC-cooked package with the PS5 build's
   package of the same id: container versions, package summary versions, import tables and
   export class names (all readable from uncompressed, unencrypted data). Mismatches are hard
   evidence against; matches are evidence for, never proof.
3. **Experiment mode with explicit evidence capture.** An opt-in flow for one mod at a time:
   publish, ask the user to start the game, collect SMP's debug.log and a short questionnaire,
   and store the result as a hardware record. Never for overlays with loaders or code.
4. **Conversion providers, one game at a time.** Candidates: repackaging loose cooked assets
   into an IoStore container for a game whose loading is established (needs an IoStore writer
   and the game's container settings); config-file translation where the PS5 location is
   known. Each provider ships with validation and tests, or not at all. Oodle-compressed or
   encrypted game data stays out of scope.
5. **Effective backport root.** Install to the root ShadowMountPlus actually selects (see
   `docs/shadowmount.md`), refusing when a higher-priority foreign backport exists.
