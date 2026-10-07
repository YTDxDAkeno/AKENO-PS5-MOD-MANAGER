# Compatibility and Test Status

## Test status vocabulary

These words have exact meanings in this project. They are never merged or
upgraded without evidence.

| Status | Meaning | Evidence required |
|---|---|---|
| **compiled** | builds with the stated toolchain | CI log |
| **unit tested** | host unit tests pass | CI log |
| **mock tested** | exercised against mocked or recorded responses (e.g. the mock ShadowMountPlus API) | test name, fixture |
| **hardware tested** | run on a real PS5 by a maintainer | entry in the log below |
| **verified** | hardware tested on the stated firmware with a reproducible result, re-checked after changes | entry in the log below |

### Current status (0.1.0-alpha)

| Component | compiled | unit tested | mock tested | hardware tested |
|---|---|---|---|---|
| Core (paths, guard, logging, database, JSON) | ✓ host + PS5 | ✓ | – | partly: database created and migrated (schema v3), logs written, 2026-10-07 |
| HTTPS client (libcurl) | ✓ host + PS5 | ✓ (loopback server) | – | ✓ 2026-10-07: certificate check with the embedded CA bundle, one download from GitHub |
| ShadowMountPlus client and game discovery | ✓ host + PS5 | ✓ | ✓ (fixtures, mock server) | ✓ 2026-10-07 (1.7beta4), including game versions from `param.json` (14 of 14) |
| System check / Safe Mode | ✓ host + PS5 | ✓ | ✓ | ✓ 2026-10-07 (12.20) |
| User interface | ✓ host + PS5 | ✓ (screen logic) | ✓ (desktop, offscreen) | partly: started from the websrv Homebrew Launcher, software renderer, tabs, game list, settings, catalogue, diagnostic export, 2026-10-07 |
| Akeno Catalogue provider, compatibility labels | ✓ host + PS5 | ✓ (fixtures) | ✓ (mock server, real HTTP client) | ✗ |
| Remote images (download, validation, cache) | ✓ host + PS5 | ✓ (loopback server) | ✓ (mock server) | ✗ |
| Search keyboard (system IME dialog) | ✓ PS5 | ✓ (text entry logic) | desktop keyboard only | ✗ |
| Download engine (resume, retry, SHA-256, reserve) | ✓ host + PS5 | ✓ (loopback server, real libcurl) | ✓ (mock server, UI and `--download-test`) | partly: one small file downloaded and verified, 2026-10-07; resume and retries not hardware tested |
| Secure extraction (libarchive) | ✓ host + PS5 | ✓ (hostile archives written by libarchive) | ✓ (UI and `--download-test`) | partly: the one-file test archive unpacked and its staging folder deleted, 2026-10-07 (third run) |
| Mod analyser, conflicts, dry-run plan | ✓ host + PS5 | ✓ | ✓ (demo catalogue) | partly: the test archive analysed and planned (copies), 2026-10-07 |
| Controller mapping on PS5 | ✓ PS5 | ✓ (mapping table) | – | partly: controller and PS5 Remote Control detected and used, 2026-10-07 |
| Notifications, firmware detection | ✓ PS5 | ✓ (version decoding) | – | firmware detection ✓ 2026-10-07; notifications ✗ (not reported) |

"Partly" and "✓" refer to one run on one console. Nothing here is **verified**
yet (that needs a repeated, reproducible result after changes).

## Mod compatibility labels

| Label | Meaning | Colour |
|---|---|---|
| **VERIFIED** | A catalogue record confirms this mod version works with this title ID **and** this game version on PS5 | green |
| **LIKELY** | Same title ID, compatible game version range, data-only content of a known-good type, but no direct test | teal |
| **EXPERIMENTAL** | Plausible but untested combination, or the game version differs from the verified one. Installing requires a deliberate confirmation | amber |
| **UNKNOWN** | Not enough information. This is the default | grey |
| **PC ONLY** | Contains Windows native code or depends on a Windows runtime (`.dll`, `.exe`, UE4SS, script extenders) | purple |
| **INCOMPATIBLE** | Known not to work, or contains content Akeno refuses (e.g. PS5 native code, `fakelib`) | red |

Rules:

* A title ID match alone never produces VERIFIED. The installed game version
  must match a verified version.
* When in doubt, the label is UNKNOWN.

## Hardware test log

Add one entry per test session. Never edit old entries; add a new one.

```
### YYYY-MM-DD - <tester> - Akeno <version> (<git revision>)
Console: PS5 <model>, firmware <xx.xx>
Environment: <jailbreak entry>, kstuff <version>, ShadowMountPlus <version>, loader <name/version>
Steps (docs/safety-model.md §9): <which steps>
Result: <what happened, including anything unexpected>
Logs: <attached system-check / games / akeno.log>
```

### 2026-10-07 - repository owner - Akeno 0.1.0-alpha (e67584ad1c8f)
Console: PS5, firmware 12.20
Environment: homebrew environment with `/data/homebrew` and `/data/shadowmount`, ShadowMountPlus 1.7beta4 (HTTP API v1), payload loader not recorded
Steps (docs/safety-model.md §9): `AkenoSelfCheck.elf` (steps 1 to 6: system check, game list, download test)
Result:
* System check: firmware read as 12.20; homebrew environment detected, `/data` writable;
  ShadowMountPlus 1.7beta4 detected and its API connected (v1); 127 GB free in
  `/data/akeno-mod-manager`; HTTPS to raw.githubusercontent.com with certificate
  verification worked (HTTP 301); SQLite database created with schema v3. Safe Mode ON,
  installation "not yet implemented", as designed.
* Game list: 14 games reported by ShadowMountPlus (13 folder games, 1 installed package).
  **Every version was "unknown":** ShadowMountPlus 1.7beta4 does not send a `version` field
  (it was added later, in commit `4cde42a`). Fixed afterwards: Akeno reads `contentVersion`
  from the game's `sce_sys/param.json` or `/user/appmeta/<TITLE_ID>/param.json` when
  ShadowMountPlus reports no version. Not re-tested yet.
* Download test: **failed with HTTP 404.** The built-in test address pointed at a branch
  named `main`, which this repository does not have. Fixed afterwards: the test file and the
  default catalogue are read from the repository's default branch (`HEAD`). Not re-tested yet.
* Overlay probe: **hard links do not work** in `/data/akeno-mod-manager/staging` (reason not
  recorded by this build; later builds log it). Phase 5 must therefore build overlays from
  copies, which need as much free space again as the mod; the dry-run plan says so.
* Nothing outside `/data/akeno-mod-manager` was written; the test file was deleted.
Logs: system-check, games, download-test and akeno.log (not published: they contain the tester's game list)

### 2026-10-07 - repository owner - Akeno 0.1.0-alpha (48d30bc0d009)
Console: PS5, firmware 12.20
Environment: as in the previous entry (ShadowMountPlus 1.7beta4, HTTP API v1)
Steps (docs/safety-model.md §9): `AkenoSelfCheck.elf` (steps 1 to 9)
Result:
* System check: as before, all OK; Safe Mode ON as designed.
* Game list: 14 games, **all 14 versions read** from `param.json` (13 from the game folder's
  `sce_sys/param.json`, the installed package from `/user/appmeta/<TITLE_ID>/param.json`).
* Download test: **passed.** 276 bytes from raw.githubusercontent.com, SHA-256 matched (433 ms).
* Check: the archive was unpacked into staging and analysed (1 file, 0 findings), and the dry-run
  plan used copies. **But the staging folder was not empty afterwards**, although deleting it
  reported no error, so the step failed. Fixed afterwards: folders are deleted with plain
  `lstat`/`opendir`/`unlink`/`rmdir` calls instead of `std::filesystem::remove_all`, and the
  result is checked; the test report now lists anything left in staging. Not re-tested yet.
* Hard links: `link()` fails with `ENOENT` ("No such file or directory") for
  `/data/akeno-mod-manager/staging/link-probe-b`, although the folder and the source file exist.
  Cause unknown; overlays will use copies.
* The test file was deleted. Nothing outside `/data/akeno-mod-manager` was written.
Logs: system-check, games, download-test and akeno.log (not published)

### 2026-10-07 - repository owner - Akeno 0.1.0-alpha (402ffbba2d2b)
Console: PS5, firmware 12.20
Environment: as in the previous entries
Steps (docs/safety-model.md §9): `AkenoSelfCheck.elf` (steps 1 to 9)
Result:
* System check, game list (14 of 14 versions), download (347 ms, SHA-256 matched), unpacking,
  analysis and dry-run plan (copies): as in the second run.
* The staging folder of this check **was deleted** (the new delete reported success and the
  folder was gone). The report still said "NOT EMPTY", naming only
  `check-a387e572f9c8e51f-20261007-150415`, the folder the second run's build left behind.
  Fixed afterwards: at start, when no operation is recorded in the journal, Akeno removes its
  own leftover entries in staging (`check-*`, `link-probe-*`) and logs each one.
* Hard links: `link()` still fails with `ENOENT`; overlays will use copies.
Logs: akeno.log and download-test (not published)

### 2026-10-07 - repository owner - Akeno 0.1.0-alpha (2fff9384ba1a)
Console: PS5, firmware 12.20
Environment: as in the previous entries
Steps (docs/safety-model.md §9): `AkenoSelfCheck.elf`, **steps 1 to 9 all passed**
Result:
* At start Akeno removed the one leftover staging entry of the second run and logged it.
* System check OK (Safe Mode ON as designed); 14 games with all 14 versions; download of the
  test file (326 ms, SHA-256 matched); unpacking; **staging deleted again**; analysis (1 file,
  0 findings); dry-run plan with copies (hard links still fail with `ENOENT`); test file deleted.
* Not covered: the user interface (`AkenoModManager-homebrew.zip`), real mods, resume/retry.
Logs: akeno.log, system-check, games, download-test (not published)

### 2026-10-07 - repository owner - Akeno 0.1.0-alpha (2fff9384ba1a), user interface
Console: PS5, firmware 12.20
Environment: as before, plus websrv and its Homebrew Launcher
Steps: `AkenoModManager-homebrew.zip` in `/data/homebrew/AkenoModManager/`, started from the launcher
Result:
* First start: **SDL could not start** (`sceKernelAllocateMainDirectMemory: Resource temporarily
  unavailable`), exit code 1. A second start three minutes later worked. Likely the memory was
  still held by something started before (cause not confirmed); see docs/troubleshooting.md.
* Second start: software renderer; a controller and the PS5 Remote Control were detected; system
  check, game list (14 games with versions) and a library refresh worked; settings were saved; the
  catalogue loaded (0 games, as expected); a second system check and a diagnostic export worked;
  the UI closed cleanly (exit code 0).
* Not covered: downloads and mod checks from the UI, the search keyboard, notifications.
Logs: akeno.log, diagnostic report (not published)
