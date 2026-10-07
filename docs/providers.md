# Mod Providers

Every source of mods implements one interface,
`akeno::providers::IModProvider` (`include/akeno/providers/IModProvider.hpp`).
Nothing outside a provider knows which website a mod came from.

| Operation | Purpose |
|---|---|
| `searchMods`, `browseMods` | lists for a game (text search, featured / popular / newest / verified) |
| `getModDetails` | description, supported title IDs and game versions, engine, mod type, dependencies |
| `getFiles` | downloadable files with size and SHA-256 when the provider has them |
| `getScreenshots` | image URLs (HTTPS only) |
| `getDependencies` | requires / recommends / conflicts / load order hints |
| `resolveDownload` | turns a file into a concrete HTTPS download "ticket" (URL, expected size and hash, headers). The download itself is done by Akeno's download engine, not by the provider |
| `getLatestVersion` | update detection |

Providers return `Unsupported` with an explanation when the service does not
allow an operation. They never work around a provider's rules.

## Planned providers

### Akeno Catalogue (Phase 2): the first and default provider

* Curated list of mods that are known or expected to work on PS5, so users
  do not see thousands of Windows-only PC mods.
* Static JSON on GitHub ([mod-format.md](mod-format.md)), HTTPS, no account.
* Every entry has a size and SHA-256. Native-code mods are not accepted.

### GitHub releases (Phase 7)

For mod authors who publish PS5 builds as GitHub release assets, referenced
from the catalogue.

### Nexus Mods (Phase 7): official API only

Research notes from 2026-10-07 (the Nexus documentation host was not reachable
from the research environment, so this **must be re-verified** before
implementation):

* The v1 REST API serves mod metadata with a personal API key. Download links
  are available directly **only to Premium members**. Free accounts need a
  `key`/`expires` pair that the website generates when the user clicks
  "Download with Manager" (`nxm://` links). A console cannot receive those
  links.
* Plan: the user enters their personal API key with the on-screen keyboard,
  or uses an official device/SSO flow if Nexus provides one for applications.
  The key is stored only in the local database and never logged.
* Premium accounts: direct downloads through the API.
* Free accounts: metadata, screenshots and compatibility only. The details
  screen explains that downloading requires Premium or the Nexus website.
  **No scraping, no bypass.**
* Nexus lists mostly PC mods, so every Nexus mod goes through the full
  compatibility analysis and starts as UNKNOWN or PC ONLY.

### mod.io (Phase 7)

* REST API v1 with an API key. The `X-Modio-Platform: ps5` header makes it
  return files approved for PS5.
* Each game's publisher decides whether mods are available and on which
  platforms. Some games require authenticated download requests (expiring
  links). Akeno will follow those rules.
* Needs re-verification of the API terms for third-party clients before
  implementation.

## Privacy

Providers receive only what a request needs (e.g. a game identifier for a
search). Akeno never uploads the list of installed games, and there is no
telemetry.
