# Installing

> **0.1.0-alpha has not been tested on a PS5.** Follow the steps in order and
> stop at the first one that does not behave as described. Please report
> results (see [compatibility.md](compatibility.md#hardware-test-log)).

This release only **reads** information and writes to its own directory,
`/data/akeno-mod-manager/`. It does not modify games, ShadowMountPlus
settings or system files.

## Requirements

* PS5 with a working homebrew environment (kstuff / kstuff-lite) and a payload
  loader (elfldr on port 9021, etaHEN, or a payload manager).
* **ShadowMountPlus 1.7 or newer**, running, with its API enabled. This is the
  default: `api_enabled=1`, bound to `127.0.0.1:10101`. Akeno does **not**
  need `api_bind_address=0.0.0.0`. Please leave the API local.
* For the user interface: **ps5-payload-dev/websrv** and its Homebrew Launcher.

## Verify the download

```sh
sha256sum -c SHA256SUMS
```

## First hardware test (headless)

`AkenoSelfCheck.elf` has no user interface and never touches video output.
It only reads system state, ShadowMountPlus's version and game list, and
writes reports.

1. Start your homebrew environment and ShadowMountPlus.
2. Send `AkenoSelfCheck.elf` to your payload loader, for example:
   `nc -q0 <ps5-ip> 9021 < AkenoSelfCheck.elf`
3. A notification appears: *"Akeno self-check done. ShadowMount: connected.
   Safe mode: ON."* (Safe Mode is always ON in this release: installation is
   not implemented.)
4. Retrieve these files (FTP, or websrv's `/fs/` browser):
   * `/data/akeno-mod-manager/logs/system-check-<time>.txt`
   * `/data/akeno-mod-manager/logs/games-<time>.txt`
   * `/data/akeno-mod-manager/logs/akeno.log`

This covers steps 1–4 of the testing ladder in
[safety-model.md](safety-model.md#9-testing-ladder-on-real-hardware).

## Installing the user interface

1. Unzip `AkenoModManager-homebrew.zip`. Copy the `AkenoModManager` folder to
   the console as `/data/homebrew/AkenoModManager/`:
   ```
   /data/homebrew/AkenoModManager/eboot.elf
   /data/homebrew/AkenoModManager/homebrew.js
   /data/homebrew/AkenoModManager/sce_sys/icon0.png
   ```
   The folder has **no** `sce_sys/param.json`. That is deliberate:
   ShadowMountPlus scans `/data/homebrew` for games and must not try to
   register Akeno as one.
2. Start websrv and open the Homebrew Launcher (PS5 browser or the launcher
   PKG from the websrv project).
3. Select **Akeno PS5 Mod Manager**.

To exit, choose **Settings → Exit Akeno Mod Manager**.

## Where Akeno keeps its data

```
/data/akeno-mod-manager/
  database/akeno.sqlite    settings and game history
  cache/icons/             game icons copied from ShadowMountPlus
  logs/                    akeno.log, reports, diagnostic exports
  backups/                 database backups made before upgrades
  downloads/ staging/ mods/ overlays/ profiles/   (empty until later phases)
```

## Uninstalling

Delete `/data/homebrew/AkenoModManager/` and `/data/akeno-mod-manager/`.
Nothing else is created or changed by this release.
