# Offline overlay diagnostics

Run this **on a Linux host**, on an already copied overlay snapshot. No console
connection is needed. Do not point it at a live/mutating overlay. This tool never
initializes the application, extracts archives, installs a loader, modifies game
files, mounts an overlay, or activates mods. It reads input files and prints JSON
to stdout. Reading may update filesystem access times.

```sh
python3 -B tools/overlay_diagnostics.py \
  --overlay /path/to/copied/PPSA24701 \
  --title-id PPSA24701 --source-type folder \
  --firmware 12.20 --shadowmount-version 1.7beta4 \
  > /path/outside-the-input/overlay-report.json
```

The overlay directory must contain paths **relative to the game root**, exactly
as they would appear inside `backports/PPSA24701`. No wrapper stripping, loader
conversion or automatic path remapping takes place. For an archive, first obtain
a safely extracted local copy; the diagnostic tool does not extract it. Keep
reports outside the input directory so shell redirection does not change it.
Python 3.10+ and the standard library are sufficient.

The report contains:

* A deterministic manifest: every scanned directory/file, relative path, file
  size and SHA-256. Files are streamed; no file contents are included.
* Path checks for traversal-like names, invalid UTF-8, reserved system paths,
  case ambiguity (including directories), component length and SMP's default
  backport source path length. Links and special files are reported but never
  followed/read; symlinked input ancestors are rejected. Traversal, entry counts,
  total bytes and per-file bytes are bounded. Detected changes/read errors fail
  closed; this is not a filesystem snapshot or proof against concurrent writes.
* Conservative PC loader markers (`ModConfig.json`, `modinfo.ini`, `dsts-loader`,
  Reloaded-II, UE4SS), Windows/native executable signatures/extensions, and
  optional comparison against a vanilla game manifest. These are static
  heuristics, not complete dependency or asset-format parsers. An unrecognized
  loader, renamed data bundle, or dependency named only inside configuration
  content can remain unknown. Do not use this report to bypass Akeno's checks.
* PKG-only redirect estimates. With a complete, matching vanilla `app0` manifest,
  an existing directory is traversed and a missing directory counts as one
  subtree redirect, following SMP 1.7beta4. Without that manifest, the exact
  count is null and files plus directories are only an upper bound. Folder/image
  unionfs overlays do not inherit the 256-redirect limit.

Exit status **0** means the scan completed without a detected blocker;
compatibility remains **unknown**. Status **1** means a report with blockers;
**2** means invalid input or an incomplete/failed scan. `activationAllowed` and
`hardwareVerified` are always false. The supplied firmware, SMP and game version
are user context, not detected or verified values. Mounting and game consumption
are explicitly unobserved. No output from this tool certifies a mod as safe.

## Optional vanilla game manifest

Supply `--game-manifest /path/to/vanilla.json` and, when known,
`--game-version <exact-contentVersion>`. Obtain the manifest through a read-only
inventory of the correct PS5 title/update. Do not send game contents. Minimal
format:

```json
{
  "schemaVersion": 1,
  "titleId": "PPSA24701",
  "gameVersion": "<exact-contentVersion>",
  "complete": false,
  "entries": [
    {"path": "data", "type": "directory"},
    {"path": "data/example.bin", "type": "file"}
  ]
}
```

Optional file `sha256` must be 64 lowercase hex digits. Include empty directories
when claiming `complete: true`; parent directories of listed files are inferred.
`complete` is an explicit assertion from the manifest's author, not independently
verified. Missing paths in a partial manifest remain unknown. Exact redirect
counts are relative to this supplied inventory, never observations of the console.
A matching filename/hash does not establish that the game opened the file.
Region/version mismatches, casing, file-versus-directory conflicts, PC container
formats, compression, alignment, offsets and runtime rebuilding requirements all
need separate assessment. The CLI checks an explicitly supplied game version
against the baseline; absent version evidence is still a limitation.

## PPSA24701 investigation

See [the evidence and lifecycle audit](investigations/ppsa24701-overlay.md).
Do not re-enable the 514-file overlay or add a PC loader to gather evidence.
Useful next inputs are a manifest/report of the preserved crashing overlay,
the exact Akeno ELF/build hash, the vanilla PS5 title/update manifest, and logs
captured from the original failing run if already available. Preserve existing
state/journal files before attempting recovery; do not delete a foreign backport.
