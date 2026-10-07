# Phase 0 — Research Findings

Research date: **2026-10-07**. Everything below came from reading the
public sources listed, mostly by cloning the repositories and reading the
code. **None of it has been checked on a real PS5 by this project.** Where a
claim depends on hardware behaviour we have not observed, it is marked
**(unverified)**.

Status vocabulary used across the project (see `docs/compatibility.md`):

| Status | Meaning |
| --- | --- |
| compiled | Builds with the toolchain named |
| unit tested | Host-side automated tests pass |
| mock tested | Exercised against recorded/mocked responses |
| hardware tested | Run on a real PS5 by a maintainer, with notes |
| verified | Hardware tested on the stated firmware with a reproducible result |

---

## 1. Repositories inspected

| Project | Repository | Revision inspected | License |
| --- | --- | --- | --- |
| ShadowMountPlus | `github.com/drakmor/ShadowMountPlus` | `4cde42a2293756132735d8e4280eb71f089865ee` (2026-10-05, branch for 1.7; newest tag `1.7beta4`) | GPL-3.0 |
| PS5 payload SDK | `github.com/ps5-payload-dev/sdk` | `f7fd02e6e195902a449b5e664917be8c01888b34` (2026-09-23), release `v0.43` | GPL-3.0-or-later (BSD for `include/freebsd`) |
| pacbrew ports | `github.com/ps5-payload-dev/pacbrew-repo` | `dbb6998c90910f58d4f781fc2cab0ecb5ac202b4` (2026-09-18), release `v0.40.2` | per package |
| SDL2 PS5 port | `github.com/ps5-payload-dev/SDL` | `ee4c47dc0d617b3bc8f35108f9956baf228a1322` (2026-09-24) | zlib |
| websrv (homebrew launcher) | `github.com/ps5-payload-dev/websrv` | `1afd476c5044d68df4e45dc773769e5de3b2cdd6` (2026-08-10) | GPL-3.0-or-later |
| elfldr | `github.com/ps5-payload-dev/elfldr` | `02cfe91eb3f9697787460ad77d73ff951f801f50` (HEAD, README only) | GPL-3.0-or-later |
| Orbit Store PS5 | `github.com/saawant12/orbit-store-ps5` | `95f075901331492fdf70087db1a6f63ac22c43ba` (v0.8.0-4) | stated GPL-3.0-or-later, **no source code in the repository** |
| kstuff-lite | `github.com/EchoStretch/kstuff-lite` | `48cd2c4ce62aa9ca0c2cc014c1e512f276ae7c17` (HEAD, not read in depth) | see repo |
| etaHEN | `github.com/etaHEN/etaHEN` | `dafa13b562ddb137a4b4a97b9aaa287c0c57cc9c` (HEAD, not read in depth) | see repo |
| nlohmann/json | `github.com/nlohmann/json` | tag `v3.11.3` (`9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`) | MIT (vendored) |
| doctest | `github.com/doctest/doctest` | tag `v2.4.11` (`ae7a13539fb71f270b87eb2e874fbac80bc8dda2`) | MIT (vendored, tests only) |

The prebuilt SDK + library bundle used for local cross-compilation and CI is
`pacbrew-repo` release `v0.40.2` asset `ps5-payload-dev.tar.gz`, SHA-256
`a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d`.

---

## 2. Toolchain (Phase 0 items 1, 9)

* **SDK:** `ps5-payload-dev/sdk` (John Törnblom). Clang/LLD 18+ host tools
  wrap the system LLVM (`prospero-clang`, `prospero-clang++`, `prospero-cmake`,
  `prospero-pkg-config`). Target: x86-64 FreeBSD-derived kernel, static,
  position-independent executables. CMake toolchain file:
  `$PS5_PAYLOAD_SDK/toolchain/prospero.cmake`.
* **C++:** libc++ is shipped (`target/lib/libc++.a`). A C++20 test program
  using `std::filesystem`, `std::optional` and `std::variant` **compiled and
  linked** in this research environment (system clang 18.1.3, bundle
  v0.40.2). It has not been run on hardware.
* **Firmware awareness in the SDK:** `crt/kernel.c` has kernel offset tables
  for firmware `0x12200000` (12.20), and also 12.00–12.70 and 13.00–13.42.
  `kernel_get_fw_version()` returns the libkernel `sdk_ps5_ver`, encoded as
  `0xMMmm0000` in BCD-like hex (`0x12200000` → "12.20").
* **SCE stub libraries available:** AppInstUtil, AudioOut, Http, Http2,
  ImeDialog, Notification, Pad, Ssl, SystemService, UserService, VideoOut,
  Net, NetCtl, RegMgr, SysCore, and others (`sce_stubs/`).

## 3. ShadowMountPlus (Phase 0 item 2)

ShadowMountPlus (SMP) is a background payload that mounts game folders and
images (`.ffpkg` UFS, `.exfat`, `.ffpfs`, `.ffpfsc`) and registers them with
the system. It needs **kstuff-lite v1.07+**.

### 3.1 Public API

`docs/api.md` and `docs/openapi.yaml` describe **HTTP/JSON API v1**:

* Default bind `127.0.0.1:10101` (loopback only). `api_bind_address=0.0.0.0`
  opens it to the LAN. **The API has no authentication.** Akeno talks to
  loopback only and never asks the user to open it to the LAN.
* Every JSON operation is `POST` with `Content-Type: application/json`
  and a JSON object body (max 4096 bytes). Responses carry `status`
  (0 = success, otherwise errno) and `error` / `error_reason` on failure.
* The internal `/system_tmp/shadowmount.sock` is private. **Akeno must not
  use it.**

Routes Akeno needs (read-only unless noted):

| Route | Use in Akeno |
| --- | --- |
| `POST /api/v1/version` | Detection, version, `capabilities[]` for capability-based gating |
| `POST /api/v1/games` | Game discovery: `title_id`, `title_name`, `version`, `content_id`, `platform`, `path`, `runtime_path`, `source_type` (`folder`/`image`/`pkg`), `mounted`, `installed_pkg`, `icon_url`, … |
| `GET /api/v1/games/icon?title_id=…[&size=thumb]` | Game icon PNG (full or cached 128×128) |
| `POST /api/v1/storage` | Free space per mounted filesystem |
| `POST /api/v1/settings` | Custom scan paths (needed for backport precedence, Phase 5) |

Akeno must **not** use mutating routes such as `games/delete`, `games/move`,
`games/uninstall`, `settings/update` or `manual/*`. The mod manager has no
reason to change the user's game library.

### 3.2 The overlay mechanism: "backports"

SMP already provides a non-destructive overlay for a title. The README calls
it a *backport*. From the source:

* **Location and precedence** (`sm_scan.c: resolve_backport_path_for_title`):
  `<scanpath>/backports/<TITLE_ID>/`. The game's own scan path wins. Then the
  other scan paths are tried in order. `/data/homebrew/backports/<TITLE_ID>/`
  is always available as a fallback.
* **Folder/image games** (`sm_filesystem.c: mount_backport_overlay`): SMP
  mounts **unionfs** with `from=<backport dir>`, `fspath=/system_ex/app/<TITLE_ID>`
  (SMP's own runtime mount of the game), `copymode=transparent`. The layer is
  read-write if the mount below it is read-write. The original game files are
  not modified. The backport directory is layered above them.
* **Installed PKG games** (`sm_pkg_backport.c`): just before the process is
  spawned, SMP creates **NSFS redirects** into the package sandbox's `app0`.
  A regular file becomes one *plain* redirect. A directory missing in `app0`
  becomes one *overlay* redirect covering its whole subtree. **Hard limit:
  `PKG_BACKPORT_MAX_REDIRECTS = 256` entries per sandbox**, and
  `PKG_BACKPORT_MAX_DIRECTORY_DEPTH = 64`. Entries are found with `lstat`.
  Non-regular, non-directory entries (symlinks, devices, FIFOs) are
  **silently skipped**.
* **Reserved names:** top-level `fakelib/` and `fakelib2/` inside a backport
  are **not** overlaid as data. They are mounted into the running game's
  sandbox `common/lib`. That means *system library replacement, i.e. native
  code*. **Akeno must reject any mod that would create a top-level
  `fakelib` or `fakelib2` entry.**
* **Permission repair:** on UFS/BFS, SMP `chmod`s every regular file and
  directory under `backports/` to `0777` (`repair_backport_path_permissions`).
  If Akeno builds overlays from **hard links**, the shared inode of the stored
  mod copy changes mode too. Combined with a read-write unionfs, a game write
  could change the stored mod. **Mitigation:** Akeno records per-file
  SHA-256 for every installed mod and re-verifies sources before every
  overlay rebuild. The overlay mode is a setting (`hardlink` / `copy`).
* **Lifecycle:** with the default `persistent_image_mounts=0`, layers are
  mounted when a game starts and released after it exits. Akeno must never
  swap an overlay while the title is mounted or running (`mounted` field;
  `EBUSY` semantics in the API).
* **Existing user backports:** firmware "backport" folders (often containing
  `fakelib`) are common on lower firmware. Akeno must **never overwrite a
  backport directory it did not create.** See `docs/shadowmount.md` for the
  adoption design.

Conclusion: **the SMP backport directory is the overlay mechanism.** Akeno
generates the final merged tree for a title inside its own storage. In
Phase 5 it will publish that tree as the title's backport directory with a
journaled rename on the same filesystem. Original game files are never
written.

### 3.3 Other relevant limits

`MAX_IMAGE_TITLES 8192`, `MAX_PATH 1024`, `MAX_TITLE_ID 32`,
`MAX_PARAM_JSON_SIZE 1 MiB` (`include/sm_limits.h`). Supported title-ID
pattern for overlays: prefix `PPSA`/`CUSA`/`LAPY`/`FAKE` followed by five
digits.

## 4. Orbit Store PS5 (Phase 0 item 3)

* The repository holds a README, guides, release notes, an **encrypted**
  catalogue (`catalogue-v3.enc`) and release manifests (`tv-app.json`,
  `payloads.json`, each with a SHA-256). **No application source is
  published.** Nothing can be studied or reused at the code level.
* Architecture from the documentation:
  1. a background **download service payload** (`orbit_store.elf`) started
     through an ELF loader on port 9021, which keeps running when the UI
     closes;
  2. a **native TV app** packaged as `PPSA99177.ffpkg`, copied to
     `/data/homebrew/` and registered by ShadowMountPlus as a tile in the
     Games row;
  3. a browser UI and phone pairing over the LAN.
* Release manifests publish URL, size and SHA-256 per artifact. That is a
  good pattern for our self-update work.
* Its catalogue offers **commercial game downloads** (Archive.org,
  Vikingfile). Akeno does not integrate with, link to or reuse that
  catalogue. Only the general architecture (service + UI split, release
  manifest with checksums, ShadowMount-registered native tile) is noted.

## 5. HTTP / TLS stack (Phase 0 item 4)

* **libcurl 8.18.0 with OpenSSL 3.5.2** (pacbrew). Its CA bundle is compiled
  in as a path, `${PREFIX}/etc/ca-bundle.crt` (`/user/homebrew/etc/...` on
  the target), which **does not exist on a stock console**. Akeno therefore
  **embeds the Mozilla CA bundle** (`ca-bundle.crt` from the SDK bundle,
  generated by curl's `mk-ca-bundle.pl`) and passes it with
  `CURLOPT_CAINFO_BLOB`. Peer and host verification stay enabled at all
  times.
* `libSceHttp`/`libSceHttp2`/`libSceSsl` stubs exist (sample `http2_get`).
  libcurl is preferred because it is the same on host and target and is
  testable.
* The SDK's own `prospero-fetchpkg` disables certificate verification. That
  is acceptable for a developer tool, but **Akeno will not copy that
  pattern.**

## 6. JSON (Phase 0 item 5)

jansson 2.14 and json-c 0.19 are ported. Akeno vendors **nlohmann/json
3.11.3** (MIT, header-only) so host and target use identical code. It is
used only through bounded wrappers: input size is checked before parsing,
parsing uses the non-throwing API, nesting depth is limited, and every field
is accessed type-checked.

## 7. Archives (Phase 0 item 6)

**libarchive 3.7.4** (pacbrew) is built with zlib, bzip2, xz and zstd, and
without OpenSSL or libxml2. That gives ZIP (deflate/store), TAR, TAR.GZ,
TAR.XZ, TAR.ZST, TAR.BZ2, and **7z read support (LZMA/LZMA2 via xz)**.
Encrypted ZIP is not available (no OpenSSL in that build). 7z stays behind
a setting until it has been tested on hardware. libarchive reports entry
type, symlink and hardlink targets before extraction, which the secure
extractor relies on. A `7zip` (2409) port also exists but is not needed.

## 8. Images (Phase 0 item 7)

SDL2_image 2.8.2 with libpng 1.6.43, libjpeg-turbo 3.0.2 and libwebp. Akeno
reads PNG/JPEG headers itself to enforce dimension limits *before* decoding,
which protects against decompression bombs.

## 9. Native UI / rendering (Phase 0 item 8)

* **SDL2 PS5 port** (`ps5-payload-dev/SDL`): `src/video/ps5/` drives
  `sceVideoOut` with a 1920×1080 `ABGR8888` software framebuffer.
  OSMesa-based OpenGL is optional. **UI design implication:** use the SDL
  software renderer at a logical 1920×1080, pre-scale images off the UI
  thread, and cache rendered text.
* **Controller:** `src/joystick/ps5/SDL_ps5joystick.c` exposes ScePad as a
  joystick with GUID `0300d0424c050000e60c000011810000` and a fixed button
  order: 0 Cross, 1 Circle, 2 Square, 3 Triangle, 4 Touchpad, 6 Options,
  7 L3, 8 R3, 9 L1, 10 R1, 11–14 D-pad up/down/left/right, 15 L2, 16 R2.
  Akeno maps these raw indices itself and also uses SDL's GameController API
  when a mapping exists.
* **Text entry:** `SDL_StartTextInput` opens the system on-screen keyboard
  (`sceImeDialog`, `SDL_ps5keyboard.c`), for mod search.
* SDL2_ttf 2.22.0 + FreeType 2.13.2. Akeno embeds its own font (DejaVu Sans)
  and reads no system font files.

## 10. Output formats and launching (Phase 0 item 9)

| Format | How it runs | Status for Akeno |
| --- | --- | --- |
| `AkenoModManager.elf` raw payload | Sent to an ELF loader (elfldr / etaHEN / payload managers, port 9021) or autoloaded | **Produced.** Good for `--self-check` (headless system check, report to log + notification). Whether a raw payload process can take over video output is **(unverified)**. |
| websrv homebrew folder `/data/homebrew/AkenoModManager/` with `eboot.elf`, `sce_sys/icon0.png`, `homebrew.js` | Launched from the websrv **Homebrew Launcher**, which runs it in a foreground app context with display and pad (`hbldr.c`) | **Produced** as `AkenoModManager-homebrew.zip`. Recommended UI path for alpha. The folder deliberately has **no `sce_sys/param.json`**, because SMP scans `/data/homebrew` and would try to register it as a game. |
| `.ffpkg` native tile (like Orbit's `PPSA99177.ffpkg`) | UFS2 image with `sce_sys/param.json` + fake-signed `eboot.bin`, registered by SMP | **Not produced yet.** Needs a SELF launcher stub (the SDK sample `install_app/make_fself.py` shows the format). Tracked for a later release. |

Launching a game: `sceSystemServiceLaunchApp(title_id, argv, &ctx)`
(websrv `sys_launch_title`). Note that websrv first kills the running
"big app". When Akeno itself runs as the big app, launching a game ends
Akeno. The UI will warn about this (Phase 5+).

## 11. Firmware 12.20 (Phase 0 item 10)

* Secondary sources report 12.20 jailbreak entry points (Y2JB + P2JB;
  "Relapse" for 7.00–13.60), and kstuff-lite 1.08+ covering 3.00–12.70
  (1.09 reportedly up to 13.42). **(unverified)**
* SMP notes that on FW 12.00+ installation uses the SceShellCore TitleDir
  bridge. That is SMP's concern, not Akeno's.
* The SDK has offsets for 12.20 (`case 0x12200000`).
* **Design consequence:** Akeno shows the firmware for information only and
  never enables or disables a feature based on the firmware number. Gating
  uses detected capabilities: SMP API reachable and advertising the needed
  capabilities, writable storage, hard-link support, free space.

## 12. Mod providers (Phase 7 preparation)

The `nexusmods.com` and `docs.mod.io` hosts were **blocked by the research
environment's egress policy**. The notes below come from search results and
must be re-verified against the official documentation before Phase 7.

* **Nexus Mods:** the v1 REST API returns download links directly
  **only for Premium members**. Non-premium downloads need a `key` +
  `expires` pair that is only generated by the website's "Download with
  Manager" (`nxm://`) flow. A console app cannot receive `nxm://` links.
  Implications: Premium + personal API key (entered via on-screen keyboard)
  or the official SSO/OAuth device flow if available; **no scraping, no
  bypass**. Non-premium direct download will likely be an *explained
  limitation*. A GraphQL v2 API exists for metadata.
* **mod.io:** REST v1. The `X-Modio-Platform: ps5` header makes the API
  return platform-approved files. The `binary_url` of a modfile is the CDN
  download. Some games require downloads to be initiated via an
  authenticated API call (expiring verification hash). Read access needs an
  API key. Whether a given game exposes PS5 files is decided by the game's
  publisher.
* **Version 1 provider** is therefore the project's own curated catalogue
  (`AkenoCatalogProvider`), served as static JSON over HTTPS from GitHub.

## 13. Stellar Blade notes (first target)

PC mods for Stellar Blade (Unreal Engine) ship as `.pak` / `.utoc` / `.ucas`
triplets dropped into `SB/Content/Paks/~mods`. Whether the PS5 build loads
a `~mods` directory from `app0` through an SMP backport has **not been
established**. That is exactly what the Phase 5 "first known-safe mod" test
must determine. Until then no Stellar Blade mod may be labelled *verified*.

## 14. Decisions

1. Overlay = ShadowMountPlus backport directory, generated by Akeno, never
   hand-merged. Publication is journaled and reversible (Phase 5).
2. Detection through the SMP HTTP API on loopback only. No use of
   `app.db` or the private socket.
3. Language/toolchain: C++20 with the ps5-payload-sdk. CMake builds the
   same sources for host (tests, desktop preview) and PS5.
4. Libraries: libcurl+OpenSSL (embedded CA bundle), libarchive, SQLite
   (rollback-journal mode, `synchronous=FULL`), SDL2/SDL2_ttf/SDL2_image,
   nlohmann/json.
5. License: **GPL-3.0-or-later**. That is required in practice by linking
   the GPLv3+ SDK runtime, and matches the ecosystem.
6. Deliverables for 0.1.0-alpha: `AkenoModManager.elf` and the websrv
   homebrew folder zip. `.ffpkg` is deferred until a launcher stub can be
   built and tested.
