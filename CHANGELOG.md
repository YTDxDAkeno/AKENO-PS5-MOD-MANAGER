# Changelog

All notable changes to this project are documented here. The project follows
semantic versioning once it reaches 1.0. Until then, minor versions may change
anything.

## [0.1.0-alpha] - unreleased

First development release: **Phase 0 (research) and Phase 1 (safe game browser)**.
Compiled and unit tested on a Linux host. **Not tested on PS5 hardware.**

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
- `AkenoSelfCheck.elf`: headless system check payload for first hardware tests.
- PS5 packaging: `AkenoModManager.elf`, websrv homebrew folder zip, `SHA256SUMS`.
- Developer tools: mock ShadowMountPlus API server, `--ui-script` automation,
  icon generator. GitHub Actions for host tests, sanitizers and the PS5 build.
