# Catalogue and Mod Manifest Format (schema version 1)

This is the format of the **Akeno Catalogue**, the first mod provider. It is
plain JSON served over HTTPS (GitHub raw content) and parsed with strict size
and type limits. Unknown fields are ignored so the format can grow. A document
with a `schemaVersion` the client does not know is rejected with an explanation;
the client never guesses.

## Layout

```
<catalogue root>/
  index.json                         list of games
  games/<game-id>.json               one game, with a summary of each mod
  mods/<game-id>/<mod-id>.json       full manifest of one mod
```

All identifiers (`game-id`, `mod-id`) match `^[a-z0-9][a-z0-9-]{0,62}$`. Paths
are never taken from the documents: the client builds them from validated
identifiers. That way a catalogue entry cannot point Akeno at an arbitrary
URL path.

Size limits: `index.json` ≤ 1 MiB, a game file ≤ 4 MiB, a manifest ≤ 256 KiB.
Strings shown in the UI are cut to 512 bytes, descriptions to 16 KiB.

## `index.json`

```json
{
  "schemaVersion": 1,
  "name": "Akeno Catalogue",
  "updatedAt": "2026-10-07T00:00:00Z",
  "games": [
    {
      "id": "example-blade",
      "name": "Example Blade",
      "titleIds": ["PPSA90001"],
      "modCount": 3
    }
  ]
}
```

## `games/<game-id>.json`

```json
{
  "schemaVersion": 1,
  "id": "example-blade",
  "name": "Example Blade",
  "titleIds": ["PPSA90001"],
  "engine": "unreal",
  "mods": [
    {
      "id": "example-outfit",
      "name": "Example Outfit",
      "author": "ExampleAuthor",
      "version": "1.0.0",
      "summary": "A recoloured outfit.",
      "categories": ["outfits"],
      "compatibility": "verified",
      "thumbnail": "https://example.org/thumb.png",
      "downloadSize": 123456789,
      "updatedAt": "2026-10-01T00:00:00Z"
    }
  ]
}
```

## `mods/<game-id>/<mod-id>.json` (manifest)

```json
{
  "schemaVersion": 1,

  "id": "example-outfit",
  "name": "Example Outfit",
  "author": "ExampleAuthor",
  "version": "1.0.0",
  "summary": "A recoloured outfit.",
  "description": "Longer text shown on the details screen.",
  "categories": ["outfits"],
  "license": "free to use, credit the author",
  "homepage": "https://example.org/mod",
  "updatedAt": "2026-10-01T00:00:00Z",

  "game": {
    "titleIds": ["PPSA90001"],
    "versions": ["01.010.000", "01.011.000"]
  },

  "platform": "ps5",

  "compatibility": {
    "status": "verified",
    "engine": "unreal",
    "modType": "asset-replacement",
    "notes": "Tested with the base outfit only.",
    "testedOn": [
      { "gameVersion": "01.011.000", "firmware": "12.20", "date": "2026-10-01" }
    ]
  },

  "download": {
    "url": "https://github.com/example/mod/releases/download/v1.0.0/example-outfit.zip",
    "size": 123456789,
    "sha256": "<64 lower-case hex characters>",
    "format": "zip"
  },

  "installation": {
    "method": "shadowmount-overlay",
    "archiveRoot": "",
    "targetPrefix": ""
  },

  "media": {
    "thumbnail": "https://example.org/thumb.png",
    "screenshots": [
      { "url": "https://example.org/1.png", "caption": "Front" }
    ]
  },

  "dependencies": {
    "requires": [],
    "recommends": [],
    "conflictsWith": [],
    "loadAfter": [],
    "loadBefore": []
  }
}
```

### Field rules

| Field | Rule |
|---|---|
| `schemaVersion` | must be `1` |
| `id` | identifier pattern above; must equal the file name |
| `version` | 1–32 characters `[0-9A-Za-z.+-]` |
| `game.titleIds` | 1–16 title IDs, each `^[A-Z]{4}[0-9]{5}$` |
| `game.versions` | game versions the mod was checked against; empty means "unknown" |
| `platform` | `ps5` (anything else: the mod is not offered for installation) |
| `compatibility.status` | `verified`, `likely`, `experimental`, `unknown`, `pc-only`, `incompatible`; anything else is read as `unknown` |
| `compatibility.modType` | `asset-replacement`, `asset-addition`, `config`, `test-harmless`; others are shown but not installable |
| `download.url` | `https://` only |
| `download.size` | bytes, 1 B – 64 GiB, required |
| `download.sha256` | required; 64 lower-case hex characters |
| `download.format` | `zip`, `tar`, `tar.gz` (`7z` reserved) |
| `installation.method` | only `shadowmount-overlay` |
| `installation.archiveRoot` | relative directory inside the archive whose contents form the overlay (`""` = archive root) |
| `installation.targetPrefix` | relative directory inside the game (`app0`) where the files go (`""` = game root) |
| image URLs | `https://` only; images are size-limited and validated before decoding |
| dependency entries | mod identifiers of the same game |

### How the label shown to the user is computed

The catalogue's `compatibility.status` is a claim. Akeno's compatibility
engine (Phase 4) checks it against the installed game. The rules from
[compatibility.md](compatibility.md) apply. For example, `verified` is shown
as **VERIFIED** only if the installed game version is in `game.versions`.
Otherwise it is shown as **EXPERIMENTAL** with the reason. Native code found
in the archive always wins: **PC ONLY** or **INCOMPATIBLE**.

## Contributing a mod to the catalogue

1. Make sure you have the right to redistribute the files, or link to the
   author's own release.
2. Test the mod on a PS5 with the stated game version. Record the firmware.
3. Compute the SHA-256 of the exact file that will be downloaded.
4. Open a pull request against the catalogue repository with the manifest and
   the game file entry.

Catalogue entries for commercial game content, pirated material, or files
whose licence forbids redistribution are rejected.
