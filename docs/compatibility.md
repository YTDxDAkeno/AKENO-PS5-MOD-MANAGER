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
| Core (paths, guard, logging, database, JSON) | ✓ host + PS5 | ✓ | – | ✗ |
| HTTPS client (libcurl) | ✓ host + PS5 | ✓ (loopback server) | – | ✗ |
| ShadowMountPlus client and game discovery | ✓ host + PS5 | ✓ | ✓ (fixtures, mock server) | ✗ |
| System check / Safe Mode | ✓ host + PS5 | ✓ | ✓ | ✗ |
| User interface | ✓ host + PS5 | ✓ (screen logic) | ✓ (desktop, offscreen) | ✗ |
| Akeno Catalogue provider, compatibility labels | ✓ host + PS5 | ✓ (fixtures) | ✓ (mock server, real HTTP client) | ✗ |
| Remote images (download, validation, cache) | ✓ host + PS5 | ✓ (loopback server) | ✓ (mock server) | ✗ |
| Search keyboard (system IME dialog) | ✓ PS5 | ✓ (text entry logic) | desktop keyboard only | ✗ |
| Controller mapping on PS5 | ✓ PS5 | ✓ (mapping table) | – | ✗ |
| Notifications, firmware detection | ✓ PS5 | ✓ (version decoding) | – | ✗ |

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

*No entries yet.*
