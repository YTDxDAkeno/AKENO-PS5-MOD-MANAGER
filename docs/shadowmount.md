# ShadowMountPlus Integration

This document describes how Akeno uses ShadowMountPlus (SMP). It is based
on the SMP source at `drakmor/ShadowMountPlus@4cde42a`, the 1.7 line (see
`docs/research.md`). Behaviour on real hardware is **unverified** until
it is recorded in `docs/compatibility.md`.

For the exact **1.7beta4** comparison, PPSA24701 log evidence and known gaps in
cached mount-state checks, backport selection and rollback, see the
[offline investigation](investigations/ppsa24701-overlay.md). Publication of an
overlay directory, an SMP mount and game file consumption are separate events.

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

### 4.1 Implemented Akeno contract and diagnostic prediction

`src/install/OverlayManager.cpp` publishes only to
`/data/homebrew/backports/<TITLE_ID>` and refuses non-PKG sources outside
`/data/homebrew`. **The installer does not resolve effective scan roots or block
on a higher-priority foreign candidate.** Earlier text in this document described
that intended behavior as implemented; the investigation correctly identified
the fixed-root limitation. The native exporter now models selection separately;
it does not change installation destinations or authorize activation.

1. **Selection diagnostics.** For the released SMP **1.7beta4**, API version 1,
   use a fresh game list and `settings.scan_paths` / `scan_path_count`. Nonempty
   custom roots replace defaults; empty custom roots use the release defaults.
   Managed image roots are appended but skipped as backport candidates. For a
   physical folder/image source, infer the longest matching owning root with a
   path-component boundary, then try other roots in order and the explicit
   `/data/homebrew` fallback. Installed PKGs have no inferred owner. The first
   existing directory wins. An inaccessible candidate, symbolic-link ancestor,
   unsupported version, internal image source or incomplete settings produces
   `unknown`. The exporter rechecks candidates and API evidence after inventory.
   SMP's internal cached owner and image-to-physical-path mapping are not exposed;
   this is a prediction under release rules, not proof of the live mount. See
   [native diagnostics](overlay-diagnostics.md) for report fields and limits.
2. **Ownership.** Only Akeno-owned backports may be replaced. Device/inode identity
   and per-file manifests are stored in `mods/<TITLE_ID>/state.json`. No marker
   files are placed inside the overlay. Foreign backports are not adopted.
3. **Building.** `staging/apply-<TITLE_ID>-<time>/next/` contains independent copies
   of enabled stored mods in order. Source SHA-256 and compatibility are checked
   again; exact-path replacements use the last enabled mod. Ambiguous casing is
   refused. Overlay publication does not use hard links.
4. **Validation.** Regular files only; path length/depth constraints, reserved
   system paths, PC loaders and unsafe replacements are checked. PKG activation
   conservatively counts all files and directories against 256; this can refuse
   a tree SMP could redirect with fewer subtree rules. It is not a unionfs limit.
   Cached `mounted` blocks known mounted titles but is not a launch lock;
   beta4 always reports `mounted=false` for installed PKGs, so false does not
   establish that a game is stopped.
5. **Publication/rollback.** After persisting the operation journal, the old
   owned backport is renamed into staging as `previous`, then `next` is renamed
   to the fixed destination on the same filesystem. Failure handling attempts
   to restore both directory and saved ownership state. If restoration fails,
   staging/journal evidence is retained. Recovery cleanup is not a guarantee of
   automatic restoration after power loss. Original game files are never written.
6. **Vanilla.** Disables stored selections and removes only Akeno's owned
   published directory, with selection rollback on failure. It does not remove
   foreign higher-priority backports or unmount an already applied layer.
   Absence of Akeno's directory alone cannot prove an unmodified live game.
7. **Timing.** SMP normally applies backports at launch. Akeno publication and
   diagnostic export never constitute proof that SMP mounted the tree or that
   the game consumed any file. The export invokes no activation or launch route.

### 4.2 Why not other mechanisms

* Writing into `/system_ex/app/<TITLE_ID>` or the game directory: modifies
  originals and breaks the safety model.
* Mounting our own unionfs/nullfs: duplicates SMP's lifecycle management
  and would race with it.
* Symlinks inside the overlay: skipped by SMP for PKG games.

### 4.3 Observations from the PPSA28000 session (2026-10-08)

* SMP logs `backport overlay mounted (rw)` for a folder game: the unionfs layer is writable.
  FreeBSD's unionfs creates shadow directories in a writable upper layer when lower-only
  directories are looked up, so a mounted run may add empty folders to Akeno's backport
  (hypothesis; Quick diagnostics mark unrecorded overlay entries).
* `[BKP] permissions fixed: root=/data/homebrew/backports entries=3` after a targeted scan:
  the permission repair changes ctime below `backports/`. Diagnostics no longer treat that as a
  content change.
* Mounting is only visible in SMP's debug.log; Akeno logs "overlay-mounted: not-observed".
  See `docs/investigations/ppsa28000-better-carry-weight.md`.

## 5. Open questions for hardware testing

* Why does `link()` fail in `/data/akeno-mod-manager/staging` on 12.20?
  The second hardware test logged `ENOENT` for the new name although the
  folder and the source file exist. If hard links stay unavailable, the
  question below is moot and overlays use copies.
* Independent copies are used for overlays; no hard-link behavior is assumed.
* Does the PS5 build of an Unreal title (first candidates: Stellar Blade,
  PPSA28000) load additional `.pak`/`.utoc`/`.ucas` files from
  `<project>/content/paks/~mods`, or only replacements of existing files?
  Until a test answers this per title and version, Akeno treats `~mods` as a
  candidate path and does not activate PC package sets there.
* Does the SMP permission repair run on Akeno-published directories before
  the next launch? The effect on launch behavior remains unverified.
