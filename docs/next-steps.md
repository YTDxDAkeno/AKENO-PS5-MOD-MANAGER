# Next steps (handover)

State on 2026-10-07, version 0.1.0-alpha. What exists, what was tested, and what
remains, so work can resume later without re-reading the whole history.

## Done and tested on hardware (PS5, firmware 12.20, ShadowMountPlus 1.7beta4)

Testing ladder steps 1 to 9 (`docs/safety-model.md` §9) with `AkenoSelfCheck.elf`,
and the user interface from the websrv Homebrew Launcher. Details and limits:
`docs/compatibility.md`. Findings that shaped the code:

* ShadowMountPlus 1.7beta4 sends no game versions: Akeno reads `param.json`.
* `link()` fails with `ENOENT` in `/data/akeno-mod-manager`: overlays must use copies.
* `std::filesystem::remove_all` reported success but left folders on the PS5:
  `SafeFs::removeTree` uses plain POSIX calls and verifies the result. Prefer
  plain POSIX calls for anything that deletes or moves files on the console.
* The repository has no `main` branch: remote URLs use `HEAD`.

## Phase 5 (0.2.0-alpha): implemented, not hardware tested

Installing and Vanilla are in `src/install/OverlayManager.cpp`. Next hardware steps:
ladder step 10 (`AkenoSelfCheck.elf`, test title only), then step 11 below.

## Not implemented

* **Phase 5 design notes (kept for reference).** Design: `docs/shadowmount.md` §4 (overlay built in
  `overlays/<TITLE_ID>/overlay.next/`, copies, validation, journaled rename into
  `<scanpath>/backports/<TITLE_ID>/`, Vanilla by removing Akeno's backport).
  Existing pieces to build on: `InstallPlan` (`src/mods/ModAnalyzer.cpp`), the
  recovery journal (`src/core/OperationJournal.cpp`), `SafeFs` and the write guard
  (`src/security/`). The write guard must be extended for exactly one backport
  directory per title; never for anything else. Refuse while the title is mounted.
  Never overwrite a backport Akeno did not create.
* Phase 6 (profiles, load order, dependencies), Phase 7 (Nexus Mods, mod.io through
  their official APIs only), Phase 8 (advanced compatibility).
* A native home-screen tile (`.ffpkg`); the UI starts from the websrv launcher.
* Catalogue entries: the published catalogue (`catalog/`) is empty.

## Suggested order to resume

1. Phase 5 on the host with unit tests (fixture backport folders, journal crash tests).
2. Ladder step 10 on hardware: a harmless overlay for a test title only.
3. Ladder step 11: one known-safe mod for one game, then Vanilla, then compare.
4. First real catalogue entry, only after step 11 passed.
