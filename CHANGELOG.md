# Changelog

All notable changes to this project are documented here. The project follows
semantic versioning once it reaches 1.0. Until then, minor versions may change
anything.

## [0.2.0-alpha] - unreleased

**Phase 5: installing mods through ShadowMountPlus backports.** Unit and mock
tested on a Linux host; **not hardware tested yet.**

### Added
- `src/install/OverlayManager`: a checked download is unpacked again and kept
  in `mods/<TITLE_ID>/<download id>/files/` with a list of files and SHA-256
  (`mods/<TITLE_ID>/state.json`). Applying builds the overlay from copies of
  every enabled mod (later mods win) in a staging folder, re-verifies every
  stored file, allows only plain files and folders, refuses `fakelib`,
  `fakelib2`, `sce_sys`, `sce_module`, over-long or too deep paths and more
  than 256 entries for installed packages, then renames it into
  `/data/homebrew/backports/<TITLE_ID>`. Each step is journaled; the new
  overlay's identity is recorded before it is moved into place.
- Vanilla: removes Akeno's backport folder; mods stay stored.
- Refused: a backport folder Akeno did not create (never touched), a mounted
  or running game, games outside `/data/homebrew` (except installed packages),
  a different drive, a pending recovery, mods the analysis does not allow.
- UI: CROSS on the check screen installs (after confirmation); the game page
  has "Vanilla (mods off)" and shows stored and active mods.
- System check: installation is available when ShadowMountPlus is connected
  and its backports folder is on the same drive as Akeno's storage (Safe Mode
  OFF then).
- `AkenoSelfCheck.elf` ladder step 10: installs, applies, checks, turns off and
  removes the test file for the test title `TEST00000` only.
- `SafeFs::copyFile` uses plain read/write with fsync.
- Nexus Mods provider (Phase 7, official v1 API, the user's own personal API key
  in `nexus-apikey.txt`, never logged): Nexus games matched to installed games by
  name appear in Discover; mods are listed, shown and (Premium only, as Nexus
  requires) downloaded. Always EXPERIMENTAL. Nexus files have no published
  checksum: the size is checked and the SHA-256 recorded. See
  `docs/nexus-mods.md`. Tested with recorded answers only.
- GameBanana provider (free, no account): opt-in setting, because installed
  game names are sent to gamebanana.com to find them; mods listed, searched and
  downloaded for free; always EXPERIMENTAL; no published checksum, so the size
  is checked and the SHA-256 recorded. See `docs/gamebanana.md`. Tested with
  recorded answers only.

## [0.1.0-alpha] - 2026-10-07

First development release: **Phase 0 (research), Phase 1 (safe game browser),
Phase 2 (online mod browser), Phase 3 (download engine) and Phase 4 (mod check:
secure extraction, analysis, dry run)**. Compiled and unit tested on a Linux host.
First partial hardware test on 2026-10-07 (firmware 12.20, ShadowMountPlus
1.7beta4): system check, game list and one test download with unpacking. The user
interface and real mods are **not hardware tested**.

### Fixed after the first hardware test
- The download test and the default catalogue pointed at a branch named `main`,
  which the repository does not have (HTTP 404). They now use the default
  branch (`HEAD`); a stored old default is moved to the new one.
- Game versions were "unknown" with ShadowMountPlus 1.7beta4, which does not
  send them. Akeno now reads `contentVersion` from the game's `param.json`
  (game folder, runtime folder or `/user/appmeta`) and shows where it came from.
- The hard-link probe now records why links fail; the system check exposes the
  result, and the dry-run plan counts the extra space overlay copies would need.
- The built-in testing status describes the partial hardware test.
- The staging folder of a check stayed behind on the PS5 although deleting it
  reported success (second hardware test). Folders are now deleted with plain
  `lstat`/`opendir`/`unlink`/`rmdir` calls and checked afterwards; the download
  test lists anything left in staging; probe clean-up failures are logged.
- Leftover staging entries of earlier builds (`check-*`, `link-probe-*`) are
  removed at start when no operation is in progress (third hardware test).

### Added
- Research notes on ShadowMountPlus 1.7, the PS5 payload SDK, pacbrew libraries,
  the SDL2 PS5 port, websrv and Orbit Store (`docs/research.md`).
- Architecture, safety model and ShadowMountPlus overlay design documents.
- Startup system check with capability-based Safe Mode (firmware is informational only).
- ShadowMountPlus HTTP API v1 client (loopback only, read-only routes) and
  `IGameDiscoveryProvider` implementation.
- Game library: names, title IDs, versions, content IDs, paths, mount state,
  icons (validated, cached), sort and filter, version-change history.
- Controller user interface (SDL2): Home, Games, game details, Settings, About,
  log viewer, first-run guide, recovery prompt. Later-phase tabs are clearly
  marked as not available.
- Settings stored in SQLite with forward-only migrations and pre-upgrade backups.
- Logging with rotation, in-app viewer, credential redaction and diagnostic export.
- Crash-recovery journal (`operation_state.json`) with safe staging cleanup.
- Write guard: deny-list of system locations, allow-list of application roots,
  symlink-escape protection, atomic file replacement.
- HTTPS client (libcurl + OpenSSL) with mandatory certificate verification,
  embedded CA bundle on PS5, size limits, timeouts and cancellation.
- `IModProvider` interface for the provider phases.
- Akeno Catalogue provider (schema v1): validated JSON documents, caching,
  featured / newest / best compatibility / popular order, text search.
- Compatibility rules: labels computed against the installed title ID and game
  version (VERIFIED only for a listed version; otherwise EXPERIMENTAL with the
  reason), risk level, PC-only and native-code handling.
- Discover tab, per-game mod list, mod details (compatibility checklist,
  description, files with SHA-256, dependencies) and a screenshot viewer.
  "Download & install" explains that it is not available yet.
- "Browse mods" on the game details screen when the catalogue lists the game.
- Search through the system keyboard on PS5 (typed text on desktop builds).
- Remote images over HTTPS: validated before decoding, cached in
  `cache/images/` under URL hashes, bounded cache and texture memory.
- Setting and `--catalogue-url` option for the catalogue address (https only,
  loopback http for local testing).
- `akeno-catalog-check`: validates a catalogue directory with the
  application's parser, cross-checks documents and archive hashes.
- Empty published catalogue in `catalog/`; fictional demo catalogue served by
  the mock server.
- Download engine: one transfer at a time on its own thread, queue stored in
  the database (migration 2), pause/resume/remove, automatic retries with
  resume, strict `Content-Range` checks, SHA-256 check before a file is kept,
  2 GiB free-space reserve, file names generated by Akeno, orphan cleanup
  limited to Akeno's own files.
- Streaming HTTP downloads (`IHttpClient::stream`), transfer compression off
  for downloads; `SafeFs::openForWriting` (no symlinks, plain files only).
- Downloads tab; "Download" on mod details (refused for PC ONLY and
  INCOMPATIBLE, confirmation for EXPERIMENTAL); confirmation dialog.
- `--download-test` and a download step in `AkenoSelfCheck.elf` (ladder steps
  5 and 6) with a deterministic harmless test file.
- Secure extraction (libarchive; zip, tar, tar.gz, 7z readers only): two passes,
  the whole archive is refused for absolute paths, `..`, backslashes, colons,
  control or non-UTF-8 names, duplicates, file/folder clashes, symlinks, hard
  links, devices, FIFOs, sockets, encryption, and size, count, depth and
  compression-ratio limits; files are created with `O_EXCL` in a fresh staging
  folder, sizes are counted rather than trusted, archive permissions are
  ignored, and the tree is re-scanned with `lstat` afterwards.
- Mod analyser: file types from magic bytes and names; Windows code and UE4SS
  make a mod PC ONLY, console code, `fakelib`, `sce_sys` and `sce_module` make it
  INCOMPATIBLE; scripts, nested archives, case clashes and the 256-redirect
  limit of installed packages are reported; analysis never improves a label.
- Conflict prediction between checked mods of the same game; dry-run install
  plan describing the overlay steps of Phase 5 (never carried out).
- Mod check pipeline: journaled staging folder deleted afterwards, free-space
  reserve, cancellation; reports kept in `cache/analysis/` and validated when
  read. Check screen in the Downloads tab.
- `--download-test` and `AkenoSelfCheck.elf` also unpack, analyse and plan the
  test archive (ladder steps 7 to 9).
- Database migration 3 stores each download's install layout.
- `SafeFs::openForWriting` gained an exclusive-create mode.
- `AkenoSelfCheck.elf`: headless system check payload for first hardware tests.
- PS5 packaging: `AkenoModManager.elf`, websrv homebrew folder zip, `SHA256SUMS`.
- Developer tools: mock ShadowMountPlus API server, `--ui-script` automation,
  icon generator. GitHub Actions for host tests, sanitizers and the PS5 build.
