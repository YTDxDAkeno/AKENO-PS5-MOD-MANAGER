# Architecture

Akeno PS5 Mod Manager is one C++20 code base. The same sources build:

* **PS5 target** (`ps5-payload-sdk`): `AkenoModManager.elf` and the websrv
  homebrew folder (`AkenoModManager/eboot.elf`).
* **Host target** (Linux/macOS): the same application in a desktop window
  for development, plus the unit test suite.

The program is layered so that nearly all logic is host-testable. Only
`platform/` and the SDL backend in `ui/sdl/` differ between targets.

```
┌───────────────────────────────────────────────────────────────┐
│ app/          main, command line, Application wiring,         │
│               SystemCheck (capability detection, Safe Mode)    │
├───────────────────────────────────────────────────────────────┤
│ ui/           screens, widgets, navigation, input actions      │  ← no SDL
│ ui/sdl/       SDL2 canvas, input mapping, image service         │  ← SDL2/ttf/image
├───────────────────────────────────────────────────────────────┤
│ games/        GameInfo, IGameDiscoveryProvider, GameLibrary     │
│ shadowmount/  ShadowMountClient (HTTP API v1), game provider    │
│ providers/    IModProvider (+ Akeno catalogue, Phase 2)         │
│ downloads/    queue, resumable transfers (Phase 3)              │
│ archives/     secure libarchive extraction (Phase 4)            │
│ compatibility/ conflicts/ mods/  analysis, planning (Phase 4–6) │
├───────────────────────────────────────────────────────────────┤
│ network/      IHttpClient, CurlHttpClient, URL policy           │
│ database/     SQLite RAII wrapper, migrations, settings store   │
│ security/     WriteGuard, path normalization, safe names        │
│ logging/      Logger, sinks, Redactor, diagnostics export       │
│ core/         Result/Error, JSON helpers, limits, task runner,  │
│               app storage layout, operation journal             │
│ platform/     IPlatform (firmware, notifications, storage)      │
└───────────────────────────────────────────────────────────────┘
```

Rules:

* Lower layers never include higher layers.
* Nothing outside `platform/` and `ui/sdl/` may contain `#ifdef AKENO_TARGET_PS5`.
* Modules depend on **interfaces** (`IHttpClient`, `IGameDiscoveryProvider`,
  `IModProvider`, `IPlatform`, `ICanvas`). Tests inject mocks.

## Build targets (CMake)

| Target | Contents | Depends on |
| --- | --- | --- |
| `akeno_core` | core, logging, security, platform, network, database, games, shadowmount, providers, app logic | libcurl, SQLite, nlohmann/json |
| `akeno_ui` | screens, widgets, navigation (renders to `ICanvas`) | `akeno_core` |
| `akeno_sdl` | `SdlCanvas`, `SdlInput`, `SdlImageService`, embedded fonts | `akeno_ui`, SDL2, SDL2_ttf, SDL2_image |
| `AkenoModManager` | `main.cpp` | all of the above |
| `akeno_tests` | doctest suite | `akeno_core`, `akeno_ui` |

## Runtime structure

```
           UI thread (SDL event loop, 60 fps cap)
   ┌────────────────────────────────────────────┐
   │ SdlInput → Action → ScreenHost → Screen     │
   │ Screen.render(ICanvas)                      │
   │ MainThreadQueue.drain()  ◄──────────────┐   │
   └─────────────────────────────────────────┼───┘
                                             │ results posted back
   TaskRunner workers (2) ───────────────────┤
     • SystemCheck            • ShadowMount discovery
     • icon/image fetch       • catalogue requests     │
                                                       │ change notices
   DownloadManager worker (1) ─────────────────────────┘
     • one transfer at a time, retries, SHA-256 check
```

* The UI thread never blocks on the network or the disk. Every slow call
  runs on `TaskRunner` and posts its result through `MainThreadQueue`.
* `SdlImageService` decodes and pre-scales on a worker into an
  `SDL_Surface`. Only texture creation happens on the UI thread.
* The SQLite connection is shared and used under its mutex; every store
  (settings, games, downloads) locks it for each statement or transaction.
* The download worker never touches UI state. It calls a listener that posts
  one coalesced refresh to the UI thread, which then reads a snapshot.

## Startup sequence

1. Parse the command line and resolve `AppPaths` (canonical root).
2. Create the storage layout through `WriteGuard`.
3. Start logging: file sink (rotating, `logs/akeno.log`), ring buffer
   (shown in the Log viewer), stderr.
4. Check `operation_state.json` for interrupted operations.
5. Open the database, back it up, and migrate.
6. Run `SystemChecker`. Its result decides the feature availability
   (`FeatureAvailability`) and Safe Mode. Firmware is shown but never
   used for gating.
7. Show the first-run wizard if it has not been completed. Otherwise show
   the System Check screen and then Home.

Headless modes (`--self-check`, `--list-games`, `--download-test`) stop
after step 6, one discovery pass or one test download and write a report.
These are the first steps of the hardware testing ladder. The download
engine is started only by the user interface (and by `--download-test`), so
the headless modes never resume queued downloads.

## Downloads (Phase 3)

```
queued → downloading → checking (SHA-256) → downloaded
            │  ▲                 │
            ▼  │                 ▼
        paused / failed      failed: mismatch, file deleted
```

* `DownloadManager` runs one transfer at a time on its own thread. Records
  live in the `downloads` table (migration 2) and are validated when read.
* Every request is validated first (https or loopback http, known size up
  to 64 GiB, SHA-256, known archive format). Mods that the compatibility
  rules do not allow are refused by `AppController`, whatever a screen offers;
  EXPERIMENTAL ones need an explicit confirmation.
* Files are `downloads/<id>.partial` while transferring and `<id>.<format>`
  after the check. `<id>` is 16 hex digits generated by Akeno.
* Resuming sends `Range: bytes=N-` and accepts only `206` with exactly that
  range and the expected total, or a full `200` (start again). Anything else
  discards the partial file. Transfer compression is disabled for downloads.
* Before each transfer the free space must cover the rest of the file plus
  a 2 GiB reserve. Writes use `SafeFs::openForWriting` (no symlinks, plain
  files only) and are flushed every 8 MiB.
* Network errors, timeouts and HTTP 408/429/5xx are retried three times
  (2 s, 5 s, 15 s), resuming each time. Other errors fail at once.
* At startup, interrupted downloads continue, completed records whose file
  is missing become failed, and files with Akeno's naming pattern that no
  record owns are deleted. Other files in the folder are left alone.

## Game discovery

`IGameDiscoveryProvider` is the only interface the rest of the program
uses. `ShadowMountGameProvider` implements it on top of `ShadowMountClient`:

* `POST /api/v1/version` → capabilities (requires `list_games`)
* `POST /api/v1/games` → title ID, name, version, content ID, platform,
  install path, runtime path, source type, mount state
* `GET /api/v1/games/icon?title_id=…` → PNG

`GameLibrary` sorts and filters the result and stores a snapshot in the
`games` table. It records `previous_version` when a title's version
changes, so later phases can warn that installed mods were checked against
another version.

## Mod check (Phase 4)

```
completed download → inspect headers (pass 1) → free space → journal begin
  → extract into staging/check-<id>-<time>/ (pass 2) → lstat re-scan
  → analyse → delete staging → journal complete → cache/analysis/<id>.json
  → (when shown) conflicts with other checked mods of the game + dry-run plan
```

* `archives::SecureExtractor` enables only the libarchive reader of the
  declared format (zip, tar, tar.gz, 7z). Pass 1 reads headers and refuses
  the whole archive on the first unsafe entry; pass 2 must see exactly the
  same entries. Files are created with `O_EXCL` and written through
  `SafeFs`; sizes are counted, never trusted; permissions, owners and times
  from the archive are ignored. On any failure the staging folder is
  removed.
* `mods::prepareMod` (shared with the installer) lists the installed game's
  physical folder read-only (`games::probeGameTree`), decides where each file
  goes (`mods::analyzeLayout`: packaging folders, Unreal project layouts,
  game-root folders; confidence `none` to `established`), classifies files
  by magic bytes first (PE, ELF, SELF, `#!`) and then by name
  (`mods::analyzeMod`), parses Unreal containers (`unreal::analyzeUnreal`:
  pak, IoStore, Zen package headers, recomputed chunk hashes), reads the
  game's own container headers (`unreal::probeGameUnreal`), and assesses the
  result (`compatibility::assess`: outcome, category A-D, mapping confidence,
  game loading, platform compatibility, activation). See
  `docs/compatibility-engine.md`.
* `mods::planInstall` describes the Phase 5 steps, additions and
  replacements, overlaps with installed mods, space and verification.
  Nothing executes it; `storeMod` refuses anything the assessment does not
  allow.
* The check refuses to start while an interrupted operation is unresolved, so
  the recovery journal is never overwritten.

## Mod pipeline (later phases)

```
DOWNLOAD → VERIFY (size, SHA-256)   [Phase 3, implemented]
  → ARCHIVE SECURITY SCAN → EXTRACT TO STAGING → ANALYSIS → DRY RUN   [Phase 4, implemented]
  → BUILD overlay.next → VALIDATE → ATOMIC ACTIVATE (journaled renames)
```

* Source mods live in `mods/<TITLE_ID>/<MOD_ID>/`, one directory per
  installed mod version, and are never edited in place.
* The **overlay builder** merges enabled mods by load order into
  `overlays/<TITLE_ID>/overlay.next/` using hard links (or copies), then
  validates it (entry count against the ShadowMountPlus redirect limit,
  forbidden names, file hashes).
* Activation publishes `overlay.next` as the title's ShadowMountPlus
  backport directory and keeps `overlay.previous`. Every step is journaled.
* The same `InstallPlanner` code runs with `dryRun=true` (plan only) and
  `dryRun=false` (execute plan). There is no separate code path.

See `docs/shadowmount.md` for the overlay contract and
`docs/safety-model.md` for the rules.

## Storage layout

```
/data/akeno-mod-manager/
  database/akeno.sqlite
  downloads/            *.partial while downloading
  cache/icons/          game icons (PNG, keyed by title ID + version)
  cache/images/         thumbnails and screenshots, named by URL hash
  staging/              per-operation extraction directories
  mods/<TITLE_ID>/<MOD_ID>/
  overlays/<TITLE_ID>/  overlay.next / overlay.previous (Phase 5)
  profiles/
  logs/                 akeno.log(.1 .2), system-check-*.txt, diagnostic-*.txt
  backups/              database backups before migrations
  operation_state.json  journal of the operation in progress (if any)
```

## Error handling

* `Result<T>` / `Result<void>` carries either a value or an `Error`
  `{code, message, detail}`. `message` is user-facing ("Installation
  failed. The archive attempted to write outside the staging directory.").
  `detail` holds the technical cause.
* No exceptions cross module boundaries. JSON parsing uses the
  non-throwing API. Unexpected exceptions are caught at thread boundaries,
  logged, and turned into errors.

## Phase map

| Phase | Scope | State in 0.1.0-alpha |
| --- | --- | --- |
| 0 | Research | done (`docs/research.md`) |
| 1 | App start, controller, ShadowMount connection, games, icons, versions, settings, logging | implemented; compiled and unit tested; **not hardware tested** |
| 2 | Akeno catalogue, HTTPS, metadata, screenshots, search, badges | implemented; compiled, unit and mock tested; **not hardware tested** |
| 3 | Downloads, pause/resume, SHA-256, storage checks | implemented; compiled, unit and mock tested; **not hardware tested** |
| 4 | Secure extraction, analysis, conflict prediction, dry run | implemented; compiled, unit and mock tested; **not hardware tested** |
| 5 | Overlay generation and activation, rollback, vanilla | not started |
| 6 | Load order, profiles, dependencies | not started |
| 7 | Nexus, mod.io | not started |
| 8 | Unreal analysis, PC-mod classification | container analysis, layout mapping and compatibility engine implemented (host tested); no game adapter or conversion yet |
