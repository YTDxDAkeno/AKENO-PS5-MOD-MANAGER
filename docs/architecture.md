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
   TaskRunner workers (2) ───────────────────┘
     • SystemCheck            • ShadowMount discovery
     • icon fetch + decode    • (Phase 3) download workers
```

* The UI thread never blocks on the network or the disk. Every slow call
  runs on `TaskRunner` and posts its result through `MainThreadQueue`.
* `SdlImageService` decodes and pre-scales on a worker into an
  `SDL_Surface`. Only texture creation happens on the UI thread.
* The SQLite connection is owned by a single service and used under its
  mutex. Phase 3 moves it to a dedicated database worker.

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

Headless modes (`--self-check`, `--list-games`) stop after step 6 or
after one discovery pass and write a report. These are the first steps of
the hardware testing ladder.

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

## Mod pipeline (later phases)

```
DOWNLOAD → VERIFY (size, SHA-256) → ARCHIVE SECURITY SCAN → EXTRACT TO STAGING
  → COMPATIBILITY ANALYSIS → CONFLICT ANALYSIS → INSTALL PLAN → DRY RUN
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
  cache/images/         screenshots (Phase 2)
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
| 2 | Akeno catalogue, HTTPS, metadata, screenshots, search, badges | interfaces only |
| 3 | Downloads, pause/resume, SHA-256, storage checks | not started |
| 4 | Secure extraction, analysis, conflict prediction, dry run | not started |
| 5 | Overlay generation and activation, rollback, vanilla | not started |
| 6 | Load order, profiles, dependencies | not started |
| 7 | Nexus, mod.io | not started |
| 8 | Unreal analysis, PC-mod classification | not started |
