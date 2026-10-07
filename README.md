# Akeno PS5 Mod Manager

**A console-native mod manager for jailbroken PlayStation 5 consoles. The goal: browse, download, verify, install, manage and remove mods on the PS5 itself, with no PC needed.**

> **Status: 0.1.0-alpha, experimental.** This release covers **Phase 1 (a safe game library
> browser)** and **Phase 2 (an online mod browser for the Akeno Catalogue)**.
> It **does not download or install mods yet**, and it **never modifies game files**.
> It has been **compiled and unit tested on a Linux host**. It has **not been tested on PS5 hardware**.
> Do not read anything in this repository as "works on firmware 12.20" until it appears in the
> [hardware test log](docs/compatibility.md#hardware-test-log).

| | |
|---|---|
| Version | `0.1.0-alpha` |
| License | GPL-3.0-or-later |
| Target | PS5 with a homebrew environment (kstuff) and **ShadowMountPlus 1.7+** |
| Firmware testing | none yet (see below) |

---

## What it is

Akeno PS5 Mod Manager is a homebrew application for the PS5. The end goal is
that the whole mod workflow happens on the console:

1. pick an installed game,
2. browse mods that are known to work on PS5,
3. see compatibility information,
4. download over HTTPS on the PS5 and verify SHA-256,
5. extract safely, analyse, check conflicts,
6. apply the mod as a **non-destructive ShadowMountPlus overlay**,
7. enable, disable, reorder, roll back, or return to **Vanilla** at any time.

The original game files are never overwritten. Mods are layered on top through
ShadowMountPlus's per-title *backport* overlay (see [docs/shadowmount.md](docs/shadowmount.md)).

## What works in 0.1.0-alpha

| Feature | State |
|---|---|
| Startup system check (capability-based, Safe Mode) | implemented, unit + mock tested |
| ShadowMountPlus connection (local API on 127.0.0.1) | implemented, mock tested |
| Installed games: names, title IDs, versions, icons, paths | implemented, mock tested |
| Controller UI (DualSense), TV-sized layout | implemented, tested on desktop only |
| Settings, logging with secret redaction, diagnostic export | implemented, unit tested |
| Crash-recovery journal and startup recovery prompt | implemented, unit tested |
| Mod browser: Akeno Catalogue, search, order, details, screenshots | implemented, unit + mock tested |
| Compatibility labels for the installed game version | implemented, unit tested |
| Downloads, installation, profiles | **not yet** (Phases 3–6) |

Status words (`compiled`, `unit tested`, `mock tested`, `hardware tested`,
`verified`) are defined in [docs/compatibility.md](docs/compatibility.md). They are never mixed.

## Supported environment

* A PS5 that can run homebrew ELF payloads (kstuff / kstuff-lite).
* **ShadowMountPlus 1.7 or newer** with its HTTP API enabled (the default:
  `127.0.0.1:10101`). Akeno reads the game library from it and, in later
  releases, will use its backport overlays to apply mods.
* The [websrv](https://github.com/ps5-payload-dev/websrv) Homebrew Launcher, to
  start the user interface with screen and controller.

### Current firmware testing status

| Firmware | Status |
|---|---|
| 12.20 (initial target) | **not tested yet** |
| any other | not tested |

Firmware is shown for information only. Akeno enables features from
**detected capabilities**, never from the firmware number.

## Installation

Full guide: [docs/installing.md](docs/installing.md).

1. Download `AkenoModManager-homebrew.zip`, `AkenoSelfCheck.elf` and
   `SHA256SUMS` from the CI artifacts or a release, and verify the checksums.
2. Copy the `AkenoModManager` folder to `/data/homebrew/AkenoModManager/`. It
   contains `eboot.elf`, `homebrew.js` and `sce_sys/icon0.png`.
3. Start ShadowMountPlus, then websrv, and open the **Homebrew Launcher**.
   Choose **Akeno PS5 Mod Manager**.

For a first, minimal hardware test without the user interface, send
**`AkenoSelfCheck.elf`** to your payload loader (port 9021). It has no user
interface and does not touch video output. It runs the system check, lists
your games, writes both reports to `/data/akeno-mod-manager/logs/` and shows a
notification (see [docs/installing.md](docs/installing.md#first-hardware-test-headless)).

> Installing Akeno itself once may involve a PC (FTP or USB). The "no PC"
> promise is about using mods: once Akeno is on the console, no PC should be
> needed for mod workflows. That arrives with the later phases.

## First launch

The first-run guide checks the homebrew environment, connects to
ShadowMountPlus, detects your games, and explains providers and the Vanilla
profile. After that, every start shows the **System Check**:

```
Firmware                 12.20
Homebrew environment     detected - /data is writable
ShadowMount              detected (version 1.7)
ShadowMount API          connected (ShadowMount+ 1.7, API v1)
Writable data storage    OK - 812 GB free
Networking               OK (secure connection verified)
Database                 OK (schema v1)
Overlay capability       not implemented in this build (Phase 5)

SAFE MODE: ON
Game library: AVAILABLE    Installation: NOT YET IMPLEMENTED
```

Controls: **D-pad / left stick** move, **✕** select, **○** back,
**L1/R1** switch tabs, **△ / □ / OPTIONS** screen actions shown in the footer.

## Supported mod types (planned)

* Data and asset mods: textures, meshes, outfits, already-PS5-compatible
  Unreal `.pak` / `.utoc` / `.ucas` replacements.
* Installed as overlays only. Original files are never touched.

## Unsupported mod types

* Anything that ships native code: Windows `.exe` / `.dll` (e.g. `dinput8.dll`,
  UE4SS), script extenders, BepInEx, DirectX hooks, and PS5 `.elf` / `.sprx`
  or `fakelib` library overlays. Akeno never executes downloaded code.
* PC mods that need conversion. No automatic PC-to-PS5 conversion is claimed.

## Safety model

Summary of [docs/safety-model.md](docs/safety-model.md):

* Never writes to system partitions (`/system`, `/system_ex`, `/preinst`,
  `/update`, `/dev`, …) or to installed game data. Every write is checked
  against a deny-list and an allow-list (only `/data/akeno-mod-manager/` in
  this release), and symlink escapes are refused.
* Never executes downloaded code.
* Unknown compatibility is shown as `UNKNOWN`, never guessed as `VERIFIED`.
* There is always a way back to Vanilla without reinstalling a game.
* HTTPS with certificate verification always. Plain HTTP only to the
  console itself (ShadowMountPlus API). The API is never exposed to the LAN.
* No analytics, no telemetry. Logs are scrubbed of tokens and keys.

## ShadowMountPlus requirement

Akeno uses only ShadowMountPlus's **documented, read-only** API routes in this
release: `version`, `games`, `games/icon`, `storage`, `settings`. Mod
installation (Phase 5) will publish an Akeno-generated overlay as the title's
backport directory. The design is in [docs/shadowmount.md](docs/shadowmount.md).

## Providers

All behind one `IModProvider` interface ([docs/providers.md](docs/providers.md)):

* **Akeno Catalogue** (implemented): a curated, PS5-focused catalogue published
  from this repository's [`catalog/`](catalog/) directory. It is **empty until
  mods have been tested on PS5 hardware**; the mock server serves a fictional
  demo catalogue for development.
* **Nexus Mods** and **mod.io**: through their official APIs only (Phase 7).
  No scraping, no bypassing download rules.

## Building from source

Full guide: [docs/building.md](docs/building.md).

```sh
# Host (development, tests, desktop preview)
sudo apt install cmake ninja-build pkg-config libcurl4-openssl-dev libsqlite3-dev \
                 libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build

# Preview the UI with fictional demo games and the demo mod catalogue
python3 tools/mock_shadowmount.py &
./build/src/ui/AkenoModManager --data-root /tmp/akeno --catalogue-url http://127.0.0.1:10101/catalog/

# PS5 (ps5-payload-sdk + pacbrew libraries in /opt/ps5-payload-sdk)
cmake -S . -B build-ps5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/ps5-payload-sdk/toolchain/prospero.cmake
cmake --build build-ps5   # -> build-ps5/dist/
```

## Development status

| Phase | Scope | State |
|---|---|---|
| 0 | Research ([docs/research.md](docs/research.md)) | done |
| 1 | Safe game browser | implemented (not hardware tested) |
| 2 | Online mod browser (Akeno catalogue) | implemented (not hardware tested) |
| 3 | Download engine | planned |
| 4 | Mod analyser, secure extraction, dry run | planned |
| 5 | ShadowMountPlus overlay, rollback, Vanilla | planned |
| 6 | Load order, profiles, dependencies | planned |
| 7 | Nexus Mods, mod.io | planned |
| 8 | Advanced compatibility (Unreal) | planned |

## Known limitations

* Not tested on a PS5 yet. Controller mapping, video output and notifications
  follow the source of the SDK and SDL port and need hardware confirmation.
* The UI is launched through the websrv Homebrew Launcher. A native `.ffpkg`
  tile is planned but not produced yet.
* Launching a game from Akeno is not implemented.
* The published catalogue has no entries yet (see [catalog/README.md](catalog/README.md)).
* Search uses the console's system keyboard (`sceImeDialog` through the SDL
  port). Its behaviour, especially cancelling, is not hardware tested.
* English only.

## Credits

* **ShadowMountPlus** by drakmor: game mounting and the overlay mechanism Akeno builds on.
* **ps5-payload-sdk**, **websrv**, **elfldr** and the **pacbrew** ports by John Törnblom and contributors.
* **SDL2 PS5 port** (ps5-payload-dev/SDL).
* libcurl, OpenSSL, SQLite, SDL2_ttf, SDL2_image, FreeType, libpng, nlohmann/json, doctest, DejaVu fonts.
* The PS5 homebrew research community.

## Licenses

Akeno PS5 Mod Manager is licensed under the **GNU General Public License v3.0
or later** ([LICENSE](LICENSE)). Third-party components and their licenses are
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Akeno is a mod manager, not a piracy tool. It will not download commercial
games, DLC or anything that bypasses licensing.
