# Akeno Catalogue

This directory is the **published Akeno Catalogue**: the list of mods the
Akeno PS5 Mod Manager shows in its Discover tab. The application reads it from

    https://raw.githubusercontent.com/YTDxDAkeno/AKENO-PS5-MOD-MANAGER/HEAD/catalog/

(`HEAD` is the repository's default branch.)

(Settings > Mod catalogue address can point Akeno at another catalogue.)

**It is empty on purpose.** No mod has been tested on PS5 hardware with Akeno
yet, and the catalogue only lists real, redistributable mods with honest
compatibility information. Fictional demo entries used for development live in
`tests/fixtures/catalog/` and are never published here.

## Format

Schema version 1, described in [docs/mod-format.md](../docs/mod-format.md):

```
catalog/
  index.json                      games that have mods
  games/<game-id>.json            the mods of one game (summaries)
  mods/<game-id>/<mod-id>.json    one manifest per mod
```

## Adding a mod

1. Make sure the files may be redistributed, or link to the author's own
   release. Commercial game content, pirated material, paid mods and files
   whose licence forbids redistribution are rejected.
2. Only content that runs on PS5: assets and configuration. Windows binaries
   (`.dll`, `.exe`), script loaders such as UE4SS, and PS5 native code
   (`.elf`, `.prx`, `.sprx`) are not accepted.
3. `compatibility.status` is `verified` only after a test on a PS5 with each
   game version listed in `game.versions`, recorded in
   [docs/compatibility.md](../docs/compatibility.md#hardware-test-log). When in
   doubt, use `unknown`.
4. `download.url` must be HTTPS and `download.sha256` must be the SHA-256 of
   the exact file served at that address.
5. Validate before opening a pull request:

   ```sh
   cmake --build build --target akeno-catalog-check
   ./build/akeno-catalog-check catalog
   ```

   The checker uses the application's own parser and also verifies that the
   index, game files and manifests agree. CI runs it on every push.
