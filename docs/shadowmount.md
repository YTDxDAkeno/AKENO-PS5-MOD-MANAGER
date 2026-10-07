# ShadowMountPlus Integration

This document describes how Akeno uses ShadowMountPlus (SMP). It is based
on the SMP source at `drakmor/ShadowMountPlus@4cde42a`, the 1.7 line (see
`docs/research.md`). Behaviour on real hardware is **unverified** until
it is recorded in `docs/compatibility.md`.

## 1. Connection

* Endpoint: `http://127.0.0.1:10101` (SMP default). The port can be
  changed in Akeno's settings. The host cannot: `ShadowMountClient` rejects
  any non-loopback host with a safety error.
* All JSON calls are `POST` with `Content-Type: application/json` and a
  JSON object body. Request bodies stay far below SMP's 4096-byte limit.
* Response size caps: 256 KiB for `version`/`settings`/`storage`, 16 MiB
  for `games`, 4 MiB per icon.
* A response with a non-zero `status` is an error. Akeno shows SMP's
  `error` text and logs `error_reason`.
* **Never** used: `/system_tmp/shadowmount.sock`, `app.db`, or any
  mutating route in Phases 1–4.

## 2. Capability detection

`POST /api/v1/version` returns `api_version`, `shadowmount_version` and
`capabilities[]`. Akeno requires:

| Capability | Needed for | If missing |
| --- | --- | --- |
| `list_games` | game library | Game library disabled (Safe Mode reason) |
| `game_icon` | icons | Placeholder artwork |
| `storage_space` | free-space display | Akeno falls back to `statvfs` |

`api_version` must be `1`. A different value disables the library with an
explanation, rather than guessing at a new schema.

## 3. Game model mapping

| `GameInfo` field | SMP field | Notes |
| --- | --- | --- |
| `titleId` | `title_id` | Validated `^[A-Z]{4}[0-9]{5}$`. Invalid entries are skipped and logged. |
| `name` | `title_name` | Falls back to the title ID |
| `version` | `version` | `CONTENT_VERSION` (PS5) / `APP_VER` (PS4). **Missing in 1.7beta4** (added in commit `4cde42a`); then Akeno reads it from `param.json`, see below. Otherwise shown as "unknown" |
| `contentId` | `content_id` | |
| `platform` | `platform` | `ps5` / `ps4` / `unknown` |
| `sourceType` | `source_type` | `folder` / `image` / `pkg` |
| `installPath` | `path` | Folder, outer image, or `app.pkg` |
| `runtimePath` | `runtime_path` | Empty for PKGs |
| `mounted` | `mounted` | Used to block overlay swaps while mounted |
| `installedPkg` | `installed_pkg` | Selects the redirect-limit rule |
| icon | `GET /api/v1/games/icon?title_id=<validated id>` | Akeno builds this URL from the validated ID instead of following `icon_url` |

**Game version fallback.** ShadowMountPlus 1.7beta4 (hardware test 2026-10-07)
sends no `version` field, so every game showed "version unknown". When the
field is missing or empty for a PS5 game, Akeno reads `contentVersion` from the
first readable of:

1. `<path>/sce_sys/param.json` (folder games only),
2. `<runtime_path>/sce_sys/param.json`,
3. `/user/appmeta/<TITLE_ID>/param.json`.

Only absolute paths without `..` are read, at most 1 MiB (SMP's own limit), and
the value must look like a version (`01.000.000`). The source is shown in the
game list. Nothing is written. A version from ShadowMountPlus always wins.

## 4. The overlay: SMP backports

SMP layers a per-title directory, `<scanpath>/backports/<TITLE_ID>/`,
over the game at launch:

* **Folder / image games:** unionfs, `from=<backport>`,
  `fspath=/system_ex/app/<TITLE_ID>` (SMP's own runtime mount),
  `copymode=transparent`. Files in the backport shadow game files with the
  same relative path. New files are added. The game's files are not
  modified.
* **Installed PKG games:** NSFS redirects into `app0`. Each regular file is
  one redirect. Each directory missing from the game is one redirect for
  its whole subtree. **At most 256 redirects** per sandbox, directory depth
  at most 64. Symlinks and special files are skipped.
* **Reserved:** top-level `fakelib/`, `fakelib2/` → mounted as the game's
  system library directory. **Akeno rejects mods that contain them.**
  This is code injection, which the safety model forbids.
* SMP `chmod`s everything under `backports/` to `0777` on UFS/BFS.

### 4.1 Akeno's contract (Phase 5)

Implemented in `src/install/OverlayManager.cpp` (0.2.0-alpha). Simplification of
point 1: Akeno uses `/data/homebrew/backports/<TITLE_ID>` and refuses games outside
`/data/homebrew` (installed packages excepted) instead of computing other scan paths.

1. **Where.** Akeno targets the backport directory that SMP will actually
   use for the title. It computes it the same way SMP does: the owning scan
   path, then scan paths in order (from `POST /api/v1/settings` plus SMP
   defaults), with `/data/homebrew/backports/<TITLE_ID>` as fallback. If a
   *higher-priority* backport directory exists that Akeno does not own, the
   overlay would be ignored. Installation is blocked with an explanation.
2. **Ownership.** Akeno only replaces a backport directory it created. It
   records the directory's device and inode plus a content manifest in
   `mods/<TITLE_ID>/state.json` (implemented in 0.2.0-alpha). **No marker files are placed inside the overlay.** They
   would be visible to the game and would count against the 256-redirect
   limit.
3. **Existing user backports** (e.g. firmware backports with `fakelib`):
   never touched automatically. A later release may offer "adopt as base
   layer" with explicit confirmation. That would move the directory (rename
   on the same filesystem) into `overlays/<TITLE_ID>/base/`, include it
   unmodified as the lowest layer, and restore it exactly on "Vanilla" or
   on uninstall.
4. **Building.** The merged tree is built in
   `/data/akeno-mod-manager/staging/apply-<TITLE_ID>-<time>/next/` (0.2.0-alpha;
   staging, so an interrupted build is cleaned up by the recovery prompt). Regular
   files only. Hard links from `mods/` when the filesystem supports them
   (detected at startup), copies otherwise. **On the first test console
   (firmware 12.20) hard links did not work in Akeno's folder**, so copies
   are the expected case: an installed mod then needs its size twice (stored
   copy and overlay). The dry-run plan states the extra space. Source files are verified
   against their recorded SHA-256 before linking. A game write through a
   read-write unionfs could have changed them.
5. **Validation before activation:**
   * no symlinks, devices, FIFOs, sockets, or names `fakelib`/`fakelib2`
     at the top level;
   * every path ≤ 1023 bytes, depth ≤ 64;
   * PKG titles: computed redirect count ≤ the limit (Akeno warns at 90 %
     and refuses above the limit). Without visibility of `app0` the count is
     the safe upper bound "regular files + directories without files";
   * the title is **not** mounted/running (`mounted == false`).
6. **Activation** (same filesystem, journaled in `operation_state.json`):
   1. `rename(backport → overlays/<TITLE_ID>/overlay.previous)` if an
      Akeno-owned backport exists;
   2. `rename(overlay.next → backport)`;
   3. fsync parent directories and commit the journal.
   A crash between steps 1 and 2 leaves **no** backport, which is vanilla
   and safe. Recovery then either finishes step 2 or restores step 1.
7. **Vanilla.** Removing Akeno's backport directory (renaming it away
   first, then deleting) restores the unmodified game. The Vanilla profile
   does exactly this. It needs no network access and no reinstall.
8. **Timing.** SMP applies the backport at the next launch. Changes made
   while the game is running are refused.

### 4.2 Why not other mechanisms

* Writing into `/system_ex/app/<TITLE_ID>` or the game directory: modifies
  originals and breaks the safety model.
* Mounting our own unionfs/nullfs: duplicates SMP's lifecycle management
  and would race with it.
* Symlinks inside the overlay: skipped by SMP for PKG games.

## 5. Open questions for hardware testing

* Why does `link()` fail in `/data/akeno-mod-manager/staging` on 12.20?
  The second hardware test logged `ENOENT` for the new name although the
  folder and the source file exist. If hard links stay unavailable, the
  question below is moot and overlays use copies.
* Does a hard-linked file in the backport behave identically to a regular
  file for both unionfs and NSFS redirects?
* Does the PS5 build of the first target game (Stellar Blade) load
  additional `.pak`/`.utoc`/`.ucas` files from a `~mods` directory, or
  only replacements of existing files?
* Does the SMP permission repair run on Akeno-published directories before
  the next launch? This affects only timing, not safety.
