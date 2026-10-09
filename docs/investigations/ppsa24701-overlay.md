# PPSA24701 launch shutdown investigation

This is an offline source/log investigation, not a hardware reproduction or a
kernel crash diagnosis. Baseline: Akeno
`097a3e05e5951cfe2c8c71858813b32a36ce79d4`, branch
`claude/bold-johnson-xsimni`. The user reports firmware 12.20 and ShadowMountPlus
1.7beta4. Changes and tests in this PR do not establish that the shutdown is fixed.

## Evidence and its limits

User observations:

* Vanilla launches normally.
* An Akeno-installed overlay with 514 files / approximately 282 MB, including
  PC Reloaded-II/dsts-loader content, caused the entire console to shut down or
  reboot at launch. No failing-run panic trace, original archive, overlay manifest
  or exact Akeno binary has been supplied.
* A manually created `backports/PPSA24701/test.txt` test launched normally. The
  user identifies the supplied log as **most likely** that successful test.
  Redirection/consumption of `test.txt` was never confirmed.

The supplied `debug.log` is 10,217,378 bytes / 92,507 lines, SHA-256
`0f0f3b07bb17927fbf2dc8263b03d97c651b9096aa34a732ef9c890896490053`.
The raw log is not committed. Relevant observations (file timestamps have no
unambiguous date/timezone; do not convert them to a chronology across sessions):

| Lines | Observation | What it supports |
| --- | --- | --- |
| 92335–92336 | 15:27:30: folder source nullfs-mounted to `/system_ex/app/PPSA24701`; launch mount ready | Successful base-game mount preparation; folder route for this run |
| 92337–92339 | Generic `/mnt` and `/data` sandbox mounts; sandbox ready | Sandbox setup, not proof of a specific overlay or injected library |
| 92341–92344 | Game started, PID 386; MDBG monitor and kstuff tracking | A launch occurred. “tracking crash-candidate state” is monitor setup, not a crash report |
| 92346 | 15:27:45: kstuff was already disabled at the pause deadline | Runtime context worth preserving; no causal link to the reported shutdown |
| 92351–92361 | 15:31:02–03: game stop, process exit, sandbox removal and runtime release | Cleanup was observed; does not establish the exit reason |

There are 21 PPSA24701 lines, no `panic`/`fatal` matches, no `unionfs` match,
and **no PPSA24701 backport-overlay mount/failure message**. One backport
notification refers to another title. Absence of a line is not proof that an
operation never occurred: logging, capture completeness and existing mounts are
unknown. This log establishes neither the original crash nor consumption of the
test file. Even an explicit successful unionfs mount would only prove mounting;
file-open/read evidence would still be needed to establish game consumption.

An independently inspected PC reference,
[`RyoTune/DSTS.ModLoader@80dbb70`](https://github.com/RyoTune/DSTS.ModLoader/tree/80dbb70f9c1082b528389f8eebe40c007383f7f8),
clarifies what the reported `dsts-loader` structure can mean:
[`ModConfig.json`](https://github.com/RyoTune/DSTS.ModLoader/blob/80dbb70f9c1082b528389f8eebe40c007383f7f8/DSTS.ModLoader/ModConfig.json)
declares `DSTS.ModLoader.dll`, the supported executable
`digimon story time stranger.exe`, and dependencies on
`MVGL.FileLoader.Reloaded`, Reloaded-II signature scanning and shared hooks.
[`Mod.cs`](https://github.com/RyoTune/DSTS.ModLoader/blob/80dbb70f9c1082b528389f8eebe40c007383f7f8/DSTS.ModLoader/Mod.cs)
registers `dsts-loader` via `IMvglApi.AddProbingPath`; its project contains
function-signature patterns for `PackFileResource_ReadFile`/`GetFileSize`.
Thus this directory is a PC loader probing convention, not evidence that the
PS5 game natively reads the same loose-file layout. Copying that folder into a
unionfs overlay does not reproduce the loader's runtime behavior. **The user's
actual archive has not been matched to this reference revision.** This supports
blocking the unsupported dependency, not attributing a kernel crash to a file.

## Lifecycle trace and exact upstream comparison

Upstream tag [`1.7beta4`](https://github.com/drakmor/ShadowMountPlus/tree/d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075)
resolves to `d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075`. Akeno's older research
cites `4cde42a2293756132735d8e4280eb71f089865ee`, a later revision. The inspected
`sm_filesystem.c`, `sm_scan.c` and `sm_pkg_backport.c` have no diff between those
revisions; API service behavior does differ (including game version metadata).

1. **Download/check.** Provider metadata is not PS5 conversion. Nexus/GameBanana
   expose experimental downloads. `runModCheck` inspects and extracts into Akeno
   staging, analyzes every file and caches the report, then removes staging.
   `SecureExtractor` enforces archive entry, path, link, size and hash constraints.
   `analyzeMod` maps `archiveRoot` and `targetPrefix`; it recognizes executables,
   system directories and some PC loader names, but does not parse Digimon
   asset/container formats. `evaluateManifest` compares catalogue claims with
   title/version; “verified” is a catalogue claim, not a result of this task.
2. **Install.** `AppController::installDownload` builds an `InstallRequest` and
   captures a game-list `TitleTarget`. `storeMod` extracts/analyzes again, copies
   the mod into `mods/<TITLE>/<download>/files`, and records per-file mappings and
   SHA-256 in `state.json`. PC sources were checked for exact-path replacements
   only at installation. No original game files are written.
3. **Generate/validate.** `applyOverlay` builds `staging/apply-*/next` from copies
   in enabled-mod order; later exact-path replacements win. It checks hashes,
   full source path length, regular entries and depth. The baseline did not
   rerun content/loader checks on legacy stored mods. `result.files/bytes` count
   writes, including overwritten files; those totals are not a unique final-tree
   inventory. The offline tool inventories the actual tree instead.
4. **Publish/activate.** Akeno does not call a mount route. It checks cached
   `mounted`, protects foreign backports with directory device/inode ownership,
   requires a same-filesystem rename, moves the previous tree aside, saves the
   new identity and renames `next` to `/data/homebrew/backports/<TITLE>`.
   “overlay applied” means publication on disk, not that SMP mounted it or the
   game read it. The integration supports a fixed default scan root, not full
   upstream backport discovery/precedence.
5. **SMP at launch.** [`resolve_backport_path_for_title`](https://github.com/drakmor/ShadowMountPlus/blob/d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075/src/sm_scan.c#L768)
   tries the owning scan root, then other configured roots. An earlier matching
   directory can shadow Akeno's default destination. A missing candidate returns
   successfully from launch preparation without applying an overlay.
   [`mount_backport_overlay`](https://github.com/drakmor/ShadowMountPlus/blob/d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075/src/sm_filesystem.c#L862)
   mounts unionfs above the folder/image runtime path, `copymode=transparent`,
   inheriting read-only state. It logs an actual new mount, but can also accept an
   existing matching unionfs layer. Akeno does not inspect the resulting mount.
6. **PKG alternative.** [`create_child_redirects_locked`](https://github.com/drakmor/ShadowMountPlus/blob/d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075/src/sm_pkg_backport.c#L321)
   counts one NSFS redirect per file, or one per missing directory subtree.
   Existing directories recurse; depth >=64 is refused. The 256 redirect cap is
   **not a folder/image unionfs file cap**. Upstream returns errors on exceeding
   the limit; that alone is not evidence of a kernel panic.
7. **Vanilla/rollback/recovery.** `setVanilla` disables selections and applies an
   empty tree, removing only Akeno's owned backport. It neither unmounts the game
   nor removes a higher-priority foreign overlay. Recovery normally cleans the
   journal's staging paths; it is not automatic restoration of the old overlay.
   Original game content is untouched, but a missing Akeno directory alone does
   not establish “vanilla” when other backports or already-mounted layers exist.

## Concrete defects and remaining integration gaps

| Finding at baseline | Evidence / consequence | This PR |
| --- | --- | --- |
| Legacy stored mods bypass new loader checks | `applyOverlay` hashes stored files but never calls `analyzeMod`; `b6a94c8` only tightened new installations | Reanalyze stored file names/headers before copying; reject explicit dsts-loader directory markers; test legacy matching-hash loader state |
| PC replacement checks fail open on unreadable/missing roots or non-directory parents | `existsNoFollow` treats every `lstat` failure as absence; a nonempty gameFolder is trusted | Require a real absolute game root, distinguish ENOENT below it from errors, reject links/type conflicts; regression tests |
| PC provenance/checks disappear after storing | State lacked pcSource; game updates can add a formerly absent path | Persist pcSource; infer it for legacy Nexus/GameBanana state; repeat replacement check at activation; test game-update case |
| Case collisions across mods do not implement deterministic last-wins | Baseline folds the `written` key but removes the later spelling; on case-sensitive storage both spellings can survive, including differently cased parents | Refuse ambiguous file/directory spellings before the swap; exact-path order remains supported |
| Journal writes ignored before live rename | `markOverlayTouched` and `recordStep` errors discarded | Abort before swapping when journal persistence fails; permission fault test |
| Failed swap rollback leaves replacement identity or loses old data | State is updated before next rename; baseline restores old directory without old state, and deletes staging even if restore fails | Restore old identity too; retain journal/staging if restoration fails; state-write fault test. Failure of the second rename/restore itself is not directly fault-injected |
| Failed Vanilla can leave disabled selections with unchanged live overlay | `setVanilla` saved flags before mounted/root checks | Precheck mounted/recovery state and restore selections on failure; tests |
| PKG count overestimates redirects | Baseline counts all files **and** directories | Keep conservative activation refusal, correct its message; offline tool models subtree redirects against explicit complete baseline |
| Stop/mount guard is insufficient | UI uses cached game list; SMP [`game_to_json`](https://github.com/drakmor/ShadowMountPlus/blob/d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075/src/sm_api_service.c#L1075) always reports mounted=false for installed PKGs | Documented unresolved. No mount/launch lock or reliable PKG running-state proof added; do not treat false as proof a title is stopped |
| Effective backport selection is unverified | Akeno never resolves all configured candidates against upstream precedence | Native read-only export now predicts selection using beta4 scan-root precedence and local filesystem evidence; unknown on missing/ambiguous evidence. Installer remains fixed-root; no mount or consumption proof |

These are code defects/gaps, **not established causes of the reported shutdown**.
The host regression with 514 benign files checks that the folder path does not
inherit the PKG limit; it is not a console capacity or stability test.

## Hypotheses requiring more evidence

* The original overlay may map PC files onto PS5 game data or depend on
  Reloaded-II/dsts-loader to rebuild/redirect content at runtime. The user-reported
  markers make this a concrete compatibility concern, but the exact mappings,
  headers, dependencies and asset versions are still missing. New files can also
  be consumed by a game; “adds only” is not a proof of safe content.
* An incorrectly wrapped/mapped tree could go unused (explaining the benign
  test), replace unexpected data, or have case/type conflicts. Compare manifests,
  title/update metadata and original install mappings before inferring this.
* A kernel/VFS/unionfs interaction, stale mounted layer, launch-versus-swap race,
  or payload/firmware interaction could explain a whole-console shutdown. This
  needs a failing-run kernel/payload trace; a userspace asset error by itself
  does not prove a kernel fault. No code here diagnoses a panic mechanism.
* Neither 514 files nor 282 MB proves a limit violation. The log identifies a
  folder launch, and no upstream unionfs 256-file cap was found in this path.

## Safe next evidence

Use [Export Diagnostics on PS5](../overlay-diagnostics.md) for the current stored
mods, overlay and accessible physical source inventory. It does not activate
anything. Do not restore or reactivate the crashing tree for collection. If only
a preserved snapshot is available, the optional host helper can inventory it. Also provide the exact Akeno build/ELF hash, its install
log and `state.json` mappings, the original archive SHA-256, the vanilla
PPSA24701 manifest and exact `contentVersion`, and failing-run crash/panic/SMP
logs if already available. Record which run each log represents. Read-only
copies of SMP configuration/scan-root ordering and existing mount information
would resolve selection questions; omit credentials and unrelated game contents.

No additional console launch, destructive bisect, loader enablement, modification
of original game files, or hardware verification is part of this PR.

## Native export follow-up and documentation correction

The earlier `docs/shadowmount.md` contract incorrectly said Akeno already
resolved effective scan roots and blocked on higher-priority foreign backports.
That was intended behavior, not the implementation. The corrected contract
describes the fixed-root installer and the separate native diagnostic model.
The model uses the longest physical owning root, custom roots **instead of**
defaults when present, skips managed image roots for backports and retains the
explicit `/data/homebrew` fallback. Unsupported versions, unavailable source
ownership or unreadable candidates yield `unknown`. API/candidate observations
are rechecked, but SMP's cached owner and actual mount state remain unobservable.

Native schema 2 reports carry build identity, detected firmware, game metadata,
stored mappings, SHA-256 inventories, loader dependencies, allowlisted live/disk
configuration and bounded available logs. Folder inventories never copy game
assets; PKG/image runtime paths are not treated as vanilla. Host filesystem/API
fixtures cover precedence, missing evidence, tampering, links, bounds, log
redaction, cancellation, UI invocation and exclusion of mutating API routes.
These checks do not validate console behavior or diagnose a kernel mechanism.
The original crash manifest and failing-run trace remain missing.
