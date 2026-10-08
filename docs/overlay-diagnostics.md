# Read-only overlay diagnostics

## Export directly on PS5

Open **Games → game details → Export Diagnostics** to collect one title, including
a title with no installed mods. **Settings → Export Diagnostics** collects titles
with stored mod state (up to 32); the log and system-check screens also offer
export. No PC, Python, internet download, activation or game launch is required.
Only local ShadowMountPlus read APIs are queried. If SMP is unavailable, local
manifests/logs can still be exported, with unavailable facts explicitly marked.

The UI remains responsive and shows progress and the saved path. Select **Cancel
export** to preserve a partial report. While exporting, Akeno blocks concurrent
installation, overlay changes, checks and recovery cleanup. This is not a lock
on other applications or SMP. Cancellation cannot interrupt an in-flight local
HTTP request or blocked filesystem call immediately. Avoid changing the input
trees while collecting evidence. Reading may update filesystem access times.

Reports are JSON files under
`/data/akeno-mod-manager/logs/diagnostics/diagnostic-<title-or-installed>-*.json`
(or the configured app data root). No report is automatically uploaded. Only
the new report is written; game/mod files, state and recovery journals are read.
The existing application may continue writing its normal log.

Native report schema **2** contains:

* Detected firmware (or `known: false`), title ID, game version and source,
  Akeno version, full Git revision, source/configuration SHA-256, compiler and
  target. The fingerprint identifies compiled source inputs including dirty
  edits; it is not the distributed ELF checksum. Match release `SHA256SUMS`
  separately when binary identity matters.
* Stored-mod mappings, recorded ownership/activation state, manifest paths,
  types, sizes and streamed SHA-256 for stored mods and the published overlay.
  Mapping integrity is compared to observed hashes/sizes. Recorded activation
  state is not an observation of a live mount.
* Path validity, case conflicts, special-file/link refusals, absolute SMP path
  limits, PC loader names/signatures and bounded `ModConfig.json`
  `ModDependencies` detection. Compatibility is `blocked` or `unknown`, never
  certified safe; this adds no loader or asset conversion support.
* A physical folder game inventory when SMP supplies the source and its
  `sce_sys/param.json` matches the title. Only paths, sizes and hashes are
  exported, never copyrighted game asset bytes. Version fallback uses that
  metadata. This source is not authenticated against retail/update checksums.
  Runtime mount paths, internal mounted images and installed PKG `app0` are
  not accepted as vanilla; these remain unavailable without mounting. Exact
  case/type and available hashes are compared with overlay entries. Missing
  paths in an incomplete inventory remain unknown.
* SMP version, allowlisted live settings, and relevant allowlisted values from
  `/data/shadowmount/config.ini`. Disk configuration is explicitly **not live
  evidence**; image rules, mount flags and internal owner state are not exposed
  by the beta4 API. Disk values never drive the selection prediction.
* Selection status `predicted-akeno`, `predicted-other`, `none-observed`, or
  `unknown`, with inspected candidates and inferred owning root. The model is
  pinned to upstream release `d7e35e6ce90abc6f9d0880ff40c2a5cc1fbfa075`.
  Custom/default scan precedence and the fallback follow upstream; unreadable
  higher-priority paths fail closed. Changed/unavailable final evidence clears
  the prediction. Neither mount nor game consumption is observed.
* Available tails of Akeno logs (current and rotations 1–4), SMP `debug.log`
  and `.1`, plus a read-only journal summary. Missing logs are reported.
  Known secret patterns are redacted, but paths and log text can still contain
  private information: review before sharing. No kernel/panic trace collection
  is asserted, and successful-run logs must not be labelled crash evidence.

Bounds are shared across each export: 50,000 filesystem entries, 64 GiB hashed,
300 seconds of traversal, 64 levels, 256 stored mods per title, bounded state
and configuration reads, and 256 KiB per log tail. Exceeding a byte budget leaves
null hashes with reasons; cancellation/time/entry limits leave partial trees.
Check `complete`, `hashesComplete`, `hashStatus`, findings and top-level limit
flags. Directory enumeration and hash checks detect some concurrent changes,
but do not create an atomic snapshot. Symlinks in any ancestor, nested devices,
and special files are refused. This can make legitimate aliases unavailable.
Reports larger than 64 MiB fail instead of producing an unbounded export. Export
one title at a time for the most useful comparison.

The installer still publishes to its fixed root: the diagnostic prediction is
not a new installation policy. `activationPerformed`, `gameLaunchPerformed`,
`gameAssetsExported` and `hardwareVerified` remain false. This feature has no
PS5 runtime validation yet and does not establish a cause or fix for the shutdown.

## Optional Linux snapshot helper

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
Use the native PS5 export first; the Linux helper is optional. Useful next inputs
are a manifest/report of the preserved crashing overlay,
the exact Akeno ELF/build hash, the vanilla PS5 title/update manifest, and logs
captured from the original failing run if already available. Preserve existing
state/journal files before attempting recovery; do not delete a foreign backport.
