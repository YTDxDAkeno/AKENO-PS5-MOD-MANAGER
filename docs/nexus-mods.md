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
mod is therefore **EXPERIMENTAL**: downloading needs a deliberate confirmation,
and so does installing. After the download Akeno's check still refuses Windows
programs (`.dll`, `.exe`, `.asi`), UE4SS and script loaders, console code,
`fakelib`, `sce_sys` and `sce_module`. Many PC mods will not work on PS5 even
when they pass (different file formats, different game versions); Vanilla
removes them again.

Nexus archives are installed **as they are laid out** (there is no Akeno
manifest that says where the files belong). The check screen shows exactly
where each file would go, before you confirm.

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
