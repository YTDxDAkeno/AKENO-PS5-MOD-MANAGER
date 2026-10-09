# Nexus Mods in Akeno (0.2.0-alpha)

Akeno can list and download mods from [Nexus Mods](https://www.nexusmods.com) through
its **official public API**, with **your own personal API key**. Akeno contains no key
of its own, does not scrape the website and does not work around any Nexus rule.

**Not hardware tested yet**, and not tested against the live API from the
development environment (it cannot reach nexusmods.com); built against the
documented v1 API and tested with recorded answers.

## Setting it up

1. Sign in on nexusmods.com and open *Site preferences → API Keys*. Copy your
   **Personal API Key**.
2. Put it into a text file named `nexus-apikey.txt` (only the key) and copy it to
   `/data/akeno-mod-manager/nexus-apikey.txt` on the PS5 (FTP, or websrv's file
   browser).
3. Start Akeno. The log says `Nexus Mods enabled`. The key is never written to the
   log or to diagnostic reports. Delete the file to switch Nexus off.

## What you see

* **Discover** lists, next to the Akeno Catalogue, every Nexus game whose name
  matches one of your installed games, marked "(Nexus Mods)". Matching ignores
  case and punctuation, so "MONSTER HUNTER STORIES 3: TWISTED REFLECTION" matches
  "Monster Hunter Stories 3 Twisted Reflection".
* Per game: the trending, recently updated and newly added mods (the public API
  returns about ten of each; it has no full search, so search filters these).
  Adult-only and removed mods are not shown.
* Mod details: summary, picture, files (only `.zip` and `.7z`; `.rar` is not
  supported) and the compatibility notes below.

## Compatibility: always EXPERIMENTAL

Nexus has no PS5 section; its mods are made for the **PC version**. Every Nexus
mod is therefore **EXPERIMENTAL** at download time and needs a deliberate
confirmation. After the download, Akeno's check compares the archive with the
installed game (see [compatibility-engine.md](compatibility-engine.md)): it
separates packaging folders from game paths, parses Unreal containers, refuses
Windows programs (`.dll`, `.exe`, `.asi`), UE4SS and script loaders, console
code, `fakelib`, `sce_sys` and `sce_module`, and shows the proposed layout, the
compatibility result and every reason that blocks activation.

PC files are **activated only with recorded evidence** that this game version
loads them from that place and that such PC files work on its PS5 build (a game
adapter). No title has that evidence yet, so Nexus mods can be checked and
studied on the console but are not installed into the overlay.

## Downloads need Nexus Premium

Nexus Mods gives download links to apps **only for Premium members**. With a
free account Akeno shows the mods but tells you it cannot download them; it will
not try any other way. Nexus publishes no SHA-256 checksums: Akeno checks the
exact file size, records the SHA-256 of what it received, and then runs the same
secure unpacking and analysis as for every other mod.

## Rules Akeno keeps

* Only `api.nexusmods.com` with HTTPS and certificate checks; download links
  must be HTTPS.
* Requests identify the app (`Application-Name: Akeno PS5 Mod Manager`).
* Nexus limits how often an app may ask; when it says so, Akeno stops and says so.
* Nothing about your games is sent except the requests for the games you open.
