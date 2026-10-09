# PPSA28000: Better Carry Weight x10 and the installation layout

Evidence: the Nexus Mods archive `dac30f1aa5840955.zip` (55 481 bytes, SHA-256
`e2336589…4805d32`), the native diagnostic export `diagnostic-PPSA28000-20261008-165652-…json`
(Akeno 0.2.0-alpha, revision `655094c1`, firmware 12.20, ShadowMountPlus 1.7beta4), SMP's
`debug.log` of the same session, and an empty `akeno.log`. No game was launched for this
investigation, and the mod was not activated again. Fixture: `tests/fixtures/mods/better-carry-weight-x10/`.

## A. What was wrong

### Confirmed code defects

| # | Defect | Evidence | Fixed by |
|---|---|---|---|
| 1 | **Archives from Nexus Mods and GameBanana were installed with their packaging folder.** `buildDownloadRequest` set `archiveRoot`/`targetPrefix` only for the Akeno catalogue; for every other provider both stayed empty, and `analyzeMod` mapped each archive path verbatim to the game root. | state.json mapping `Better Carry Weight x10/00000000_…_P.pak` -> `Better Carry Weight x10/00000000_…_P.pak`; the overlay inventory shows the folder at `backports/PPSA28000/` | `mods::analyzeLayout`, used by the check and the installer |
| 2 | **"Adds files only" was treated as safe enough to activate PC files.** The PC check only refused replacements; files that land where the game never looks pass it, and so would files that the game loads but cannot use. | the mod was activated with EXPERIMENTAL as its only label | activation contract (`docs/compatibility-engine.md`): PC files need recorded evidence for the title and version |
| 3 | **Compatibility was decided by file names.** `.pak/.utoc/.ucas` were "asset" files; nothing checked that they belong together, that they are well-formed, or which packages they replace. | analysis report: three files, kind `asset`, no findings | `src/unreal/` parsers and analyzer |
| 4 | **INFO log lines were buffered in user space.** `RotatingFileSink` flushed only WARN and ERROR, so the install's INFO lines stayed in an `std::ofstream` buffer. | `akeno.log` exported as 0 bytes right after an install and a diagnostic export of the same process; the uploaded `akeno.log` is empty | write-through sink (one `write(2)` per record, `fsync` on flush and for warnings) and a test that reads the log without flushing |
| 5 | **The diagnostic export spent its whole budget hashing game containers** and then discarded its own overlay-selection prediction. | `bytesHashed` 27 708 048 860, `timeLimitReached` true, `dawnwalker-ps5.ucas` (38 GB) `changed-interrupted-or-unreadable`, `selection` "Export interrupted; final evidence not rechecked"; `dawnwalker-ps5.utoc` never reached | Quick/Deep modes, selection first, game folder in its own time slice, API recheck unless cancelled |
| 6 | **Budget stops were reported as filesystem failures**, and errno was dropped. | a time-limit stop produced "Directory enumeration incomplete" / "Hash incomplete; file changed, read failed or export interrupted" | findings now say "the export's time limit was reached" and carry `EACCES (…)`-style errno text |
| 7 | **ctime counted as a content change.** `same()` compared `st_ctim`, so a chmod made a directory or file "changed during inventory". SMP chmods every entry below `backports/` (`[BKP] permissions fixed: root=/data/homebrew/backports entries=3` in this session's debug.log). | code + SMP log | content identity is now dev, inode, size and mtime; ctime-only changes are listed as metadata changes |
| 8 | **Title discovery reused a stale errno.** `errno` was cleared once before the `readdir` loop, so any errno set in the loop body could be reported as an enumeration failure. | code | errno is cleared before every `readdir` |

`directoryStream` itself was not a descriptor-ownership bug (it duplicated the descriptor and
`closedir` closed the duplicate); it now uses `F_DUPFD_CLOEXEC` and keeps errno on failure.
`Cannot stat directory` / `Cannot stat entry` could not be attributed to a code defect: on a
live descriptor, `fstat` fails only for reasons of the filesystem or the sandbox. Those findings
now keep the errno, so the next export from the console says which one.

### Hypotheses (not established)

* **The PS5 build may not mount containers from `~mods` at all.** Console builds of Unreal
  Engine can mount from a fixed list instead of scanning `Paks` recursively. Unknown until
  tested on hardware.
* **Unionfs shadow folders.** SMP mounts folder-game backports read-write (`backport overlay
  mounted (rw)` in debug.log). FreeBSD's unionfs creates a "shadow" directory in the upper layer
  when a lower-only directory is looked up on a writable union, so a mounted run can add empty
  folders (and anything the game writes) to Akeno's backport folder. This would also explain
  "Directory changed during inventory" when an export runs while a game is mounted. The export
  now marks overlay entries Akeno did not record and flags empty ones; a report taken right
  after a mounted run would confirm or refute this.
* **Case of file names.** Every observed game path is lower-case. Whether the PS5 build opens a
  mixed-case `00000000_BetterCarryWeightx10_P.pak` is unknown; Akeno maps existing folders to
  the game's spelling but keeps new names as they are.

## D. Case study: Better Carry Weight x10

### Original archive

```
Better Carry Weight x10/                                        (folder entry)
Better Carry Weight x10/00000000_BetterCarryWeightx10_P.pak       347 bytes
Better Carry Weight x10/00000000_BetterCarryWeightx10_P.ucas  171 112 bytes
Better Carry Weight x10/00000000_BetterCarryWeightx10_P.utoc      368 bytes
```

No executables, DLLs, scripts or loader files.

### Detected wrapper and package grouping

* `Better Carry Weight x10/` holds only package files and matches no folder of the game: a
  packaging folder (rule `unreal-flat`).
* The three files share the stem `00000000_BetterCarryWeightx10_P` (`_P`: patch priority) and
  form one IoStore package set. Verified on the real files:
  * `.pak`: version 11, mount point `../../../`, **0 files**, index and both secondary indexes
    match their SHA-1 hashes. It is the companion that makes the engine mount the `.utoc`.
  * `.utoc`: IoStore version 8 (`ReplaceIoChunkHashWithIoHash`), container id
    `e4a7420854dad984`, flags `indexed` only (not compressed, not encrypted, not signed),
    64 KiB blocks, one partition, two chunks: `ExportBundleData` `7285fde174bd91f8`
    (169 752 bytes) and `ContainerHeader` `e4a7420854dad984` (1 360 bytes). Directory index:
    `../../../BP_PlayerCharacter.uasset`.
  * `.ucas`: exactly the 171 112 bytes the block table needs; both chunk hashes recomputed with
    BLAKE3-160 match the `.utoc`. The `.utoc` and `.ucas` belong together.
  * Container header: signature `IoCn`, version 4, the same container id, one package id.
  * Package: `/Game/_Dawnwalker/Player/BP_PlayerCharacter`, UE 5.3+ Zen header, flags
    `Cooked | UnversionedProperties | FilterEditorOnly`, no versioning information. CityHash64
    of its lower-case UTF-16 name is `7285fde174bd91f8`: the chunk id is that package's id.
    It imports 161 packages (all `/Game/...`: player abilities, camera modes, gameplay effects
    such as `GE_Encumbered`, audio events, UI widgets).

### Proposed target

```
/data/homebrew/backports/PPSA28000/
    dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.pak
    dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.ucas
    dawnwalker/content/paks/~mods/00000000_BetterCarryWeightx10_P.utoc
```

Mapping confidence: **candidate**. Evidence: the game has one Unreal package folder,
`dawnwalker/content/paks` (`dawnwalker-ps5.pak` 10 244 371 082 bytes,
`dawnwalker-ps5.ucas` 38 212 660 416 bytes, `dawnwalker-ps5.utoc`); the author's PC
instructions name `Dawnwalker/Content/Paks/~mods/`. Against: nothing in the archive states the
path, and nothing shows the PS5 build loads anything from `~mods`.

### What it means, and what it does not

* `BP_PlayerCharacter` is the player's Blueprint class. The mod replaces the **whole package**
  (169 752 bytes of exports), not a single value. A "carry weight x10" mod presumably changes
  default values inside it; the bytes of the change are not separable without parsing the
  exports, which needs the game's class layouts (the package is unversioned).
* The reference shows which package the mod overrides. It does not show that the PS5 build has
  that package at the same id (Quick diagnostics now read `dawnwalker-ps5.utoc`'s chunk ids
  to answer exactly that), that its class layout matches the PC build the mod was cooked
  against, or that the PS5 build would mount the container at all.
* Platform: cooked Blueprint data is largely platform-neutral, but unversioned property data
  only loads correctly in a build whose classes match exactly. A PC build of a different game
  version, or a PS5 build with platform-specific differences, can fail to load or crash when
  the player character is created.

### Compatibility limitations

| Question | Answer |
|---|---|
| Path mapping | candidate (`dawnwalker/content/paks/~mods`) |
| Game loading support | unverified |
| Platform compatibility | unknown (PC-cooked, unversioned Blueprint) |
| Engine/container versions | comparable on the console (Quick diagnostics or the check read the game's `.utoc` header) |
| Imports present in the PS5 game | comparable on the console from the game's chunk ids |
| Outcome / category | `UNKNOWN`, category B |
| Activation | **blocked** |

### Should activation remain blocked?

Yes. The three things activation would need — that the PS5 build mounts `~mods`, that it has
the same package at the same id with matching imports, and that the PC-cooked Blueprint loads on
it — are each unverified. The new check measures the second on the console; the first and the
third need a deliberate hardware test, outside Akeno's automatic paths. The installed copy from
0.2.0-alpha (wrapper folder at the overlay root) is inert in the observed layout and can be
turned off in Installed Mods, which removes Akeno's overlay; Akeno will not activate it again.

### Update 2026-10-09: testing it on the console

Follow-up research (`docs/investigations/pc-unreal-mods-on-ps5.md`) found no public evidence either
way for PS5 builds, and confirmed that the SMP overlay makes added files visible to the game. Akeno
now offers Better Carry Weight x10 as a **test install**: every console-side check passes (data-only
IoStore set, same IoStore version as the game, all imports present, game containers unsigned,
files only added). Procedure:

1. Back up the PPSA28000 saved data.
2. Downloads → the mod → check → CROSS ("Test install") → confirm. Files go to
   `dawnwalker/content/paks/~mods/00000000_bettercarryweightx10_p.{pak,utoc,ucas}` (lower case,
   like every PS5 game file). The old 0.2.0-alpha copy of the same download is replaced.
3. Start the game; check the carry weight.
4. Installed Mods → OPTIONS → *It works* / *No effect in the game* / *The game crashed or did not
   start*. A crash turns the mod off.
5. With *no effect*: open the check again and press SQUARE to test it directly in
   `dawnwalker/content/paks/` (this replaces the first test install).

## ShadowMountPlus integration notes

* Scan-root precedence: the diagnostic model (owning root first, then configured roots, then
  `/data/homebrew`) is unchanged; the installer still publishes to the fixed
  `/data/homebrew/backports`. In this session `scan_paths` was empty (release defaults) and the
  game lives in `/data/homebrew`, so the fixed root is also the predicted one. Installing into
  another root without owning it would require recording ownership per root; it stays on the
  roadmap rather than risking another application's backports.
* Folder games: unionfs, read-write when the underlying mount is (see above). The original
  game folder is the lower layer and is never written; writes go to the backport.
* The 256-redirect limit belongs to installed-PKG NSFS redirects and is not applied to folder
  games.
* Overlay publication is a same-filesystem rename while the game is not mounted; SMP applies
  the overlay at the next launch. Mount state can only be observed in SMP's debug.log.
