# Changelog

All notable changes to this project are documented here. The project follows
semantic versioning once it reaches 1.0. Until then, minor versions may change
anything.

## [0.1.0-alpha] - unreleased

First development release: **Phase 0 (research), Phase 1 (safe game browser) and
Phase 2 (online mod browser)**. Compiled and unit tested on a Linux host.
**Not tested on PS5 hardware.**

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
- `AkenoSelfCheck.elf`: headless system check payload for first hardware tests.
- PS5 packaging: `AkenoModManager.elf`, websrv homebrew folder zip, `SHA256SUMS`.
- Developer tools: mock ShadowMountPlus API server, `--ui-script` automation,
  icon generator. GitHub Actions for host tests, sanitizers and the PS5 build.
