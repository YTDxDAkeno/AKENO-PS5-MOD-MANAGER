# Safety Model

Akeno PS5 Mod Manager runs on a real console with elevated privileges. A bug
must result in **"the mod does not work"**, never in **"console system files
were damaged"**. This document is the contract that every module follows.
Code review checks changes against it.

## 1. Hard rules

| # | Rule | Enforced by |
| --- | --- | --- |
| H1 | Never write to `/system`, `/system_ex`, `/preinst`, `/preinst2`, `/update`, `/system_data`, `/system_tmp`, `/dev`, boot/firmware/kernel storage. Never remount anything writable. | `security::WriteGuard` hard deny-list (checked before the allow-list). There is no remount code in the project. |
| H2 | If the exact destination of a write is not known, do not write. | Every write goes through `WriteGuard::checkWritable()`, which requires an absolute, normalized path inside an allowed root with **no symlink components** below the root. |
| H3 | Never execute downloaded code, including PS5 ELF/SELF/SPRX, Windows PE (`.exe`/`.dll`), scripts, or `fakelib` library overlays. | No `exec`/`dlopen`/payload-load path takes downloaded data. The analyzer (Phase 4) classifies native code as *unsupported*. `fakelib`/`fakelib2` top-level entries are rejected. |
| H4 | If compatibility cannot be established, the label is `UNKNOWN`. Nothing is guessed `VERIFIED`. | Compatibility engine defaults (Phase 4). `VERIFIED` requires a catalogue record that matches title ID **and** game version. |
| H5 | There is always a way back to **Vanilla / no mods** without reinstalling the game. | Mods never modify original files. Removing Akeno's generated backport directory restores vanilla (Phase 5). The Vanilla profile is permanent and cannot be deleted. |

## 2. Allowed write locations

| Root | Purpose | Phase |
| --- | --- | --- |
| `/data/akeno-mod-manager/` (canonicalized at startup) | All Akeno state: database, downloads, cache, staging, mod storage, profiles, logs, backups | 1+ |
| ShadowMountPlus backport directory for a title, e.g. `/data/homebrew/backports/<TITLE_ID>/` | Publishing a generated overlay. Only directories Akeno created (tracked in its own journal), only while the title is not mounted or running | 5 (not in 0.1.0) |

Nothing else is writable. In 0.1.0-alpha the **only** allowed root is
the application directory. Host builds use a developer-chosen directory
(`--data-root`) and the same guard.

## 3. Original game files

* Original game files are **never** opened for writing.
* Mods reach the game only through ShadowMountPlus overlays: unionfs for
  folder/image games, NSFS redirects for installed PKGs. See
  `docs/shadowmount.md`.
* Akeno never "installs by copying over" game files, even when that looks
  easier.

## 4. Untrusted input

Everything that comes from the network or from inside an archive is
untrusted. That includes catalogue JSON, provider API responses,
screenshots, archive entry names and sizes, and server-supplied filenames.

* **Size limits** (see `include/akeno/core/Limits.hpp`): JSON responses,
  images, archive metadata, path lengths, entry counts, and single-file
  extracted size are all bounded *before* allocation.
* **Streaming:** downloads, hashing and extraction stream to and from disk.
  No archive is loaded into RAM.
* **JSON:** non-throwing parser, size checked first, depth limited, all
  fields type-checked, unknown fields ignored, missing required fields
  rejected.
* **Filenames:** local filenames are generated from validated identifiers.
  Server-supplied names are never used as paths.
* **Archives (Phase 4):** reject absolute paths, `..` components, drive
  letters, backslash tricks, NUL bytes, over-long names, hardlinks,
  symlinks, devices, FIFOs and sockets. Extract only into a fresh staging
  directory, then re-scan the extracted tree with `lstat` and canonical
  paths before anything is used.
* **Images:** PNG/JPEG dimensions are read from the header and limited
  before decoding.

## 5. Network

* HTTPS with certificate **and** hostname verification, always. The
  Mozilla CA bundle is embedded on PS5 because the console has no usable
  system bundle for libcurl.
* Plain HTTP is allowed **only** for loopback (the ShadowMountPlus API on
  `127.0.0.1`). Redirects from HTTPS may only go to HTTPS. Redirect count
  is limited.
* Connect and total timeouts, a low-speed abort, and a response size cap
  apply to every request.
* The ShadowMountPlus API is used read-only in Phases 1–4. Akeno never asks
  the user to bind it to the LAN.

## 6. Secrets and privacy

* API keys and tokens (Phase 7) are stored only in the local database. They
  are never written to logs: `logging::Redactor` masks `api_key`, `apikey`,
  `token`, `access_token`, `refresh_token`, `password`, `secret`,
  `authorization`, `bearer` values in every log line.
* No analytics, no telemetry. The installed game list is not sent anywhere.
  Provider searches send only what the search needs.

## 7. Transactions and recovery

* State-changing operations write a journal (`operation_state.json` in the
  app directory) before they start and update it at each step.
* On startup, an interrupted operation is detected and the user is offered
  safe cleanup. Cleanup only ever deletes inside staging or the app root.
* Overlay activation (Phase 5) is build-next → validate → swap with
  renames → keep previous. Failure restores the previous overlay.
* The database is migrated in transactions after a backup copy. A database
  from a *newer* version is never modified.

## 8. Capability gating (Safe Mode)

Features are enabled from detected capabilities, never from the firmware
number. When a capability is missing, the feature is disabled with a reason
on screen. The app does **not** try experimental fallbacks.

| Feature | Requires |
| --- | --- |
| Game library | ShadowMountPlus API reachable on loopback, advertising `list_games` |
| Online mod browsing | Networking OK (Phase 2) |
| Downloading | Networking OK + writable app storage + free space above the reserve (Phase 3) |
| Installation | All of the above + database OK + overlay capability OK (Phase 5) |

In **0.1.0-alpha**, installation is not implemented, so the app always
reports **SAFE MODE: ON** with the reason "overlay installation is not
implemented in this build". That is intentional and truthful.

## 9. Testing ladder on real hardware

Do not skip steps. Record results in `docs/compatibility.md`.

1. Start the app (`--self-check`). The report is written to `logs/`.
2. Read system information (firmware, storage).
3. Connect to ShadowMountPlus (`version`).
4. Read installed games (`--list-games`).
5. Download a tiny harmless file *(Phase 3)*.
6. Verify its SHA-256 *(Phase 3)*.
7. Extract into Akeno's own staging folder *(Phase 4)*.
8. Delete staging *(Phase 4)*.
9. Generate a dry-run overlay plan *(Phase 4)*.
10. Create a harmless isolated overlay *(Phase 5)*.
11. Activate a single known-safe mod *(Phase 5)*.

## 10. Reporting test status honestly

Statuses `compiled`, `unit tested`, `mock tested`, `hardware tested` and
`verified` are separate and are never merged. The README and release notes
must not say a feature "works on 12.20" unless a maintainer ran it on 12.20
hardware and recorded the result.
