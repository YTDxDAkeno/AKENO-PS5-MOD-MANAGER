# Making PC Unreal Engine mods playable on PS5: research and approach

Question (from the PPSA28000 follow-up, 2026-10-09): with activation of PC mods blocked, is there
any way to make mods such as Better Carry Weight x10 playable on a jailbroken PS5? This page
records what public sources and the ShadowMountPlus source establish, what they do not, and the
approach Akeno implements from that. No game was launched for this research.

## What the sources establish

| # | Finding | Source | Consequence for Akeno |
|---|---|---|---|
| 1 | ShadowMountPlus mounts `<scanpath>/backports/<TITLE_ID>` with **unionfs** (`from=<backport>`, `copymode=transparent`) over the game's runtime folder `/system_ex/app/<TITLE_ID>`; read-write when the game's mount is. Files in the backport therefore appear at the same relative path inside the game. | SMP source `src/sm_filesystem.c` `mount_backport_overlay()` and `src/sm_scan.c`, commit `4cde42a` | An added `dawnwalker/content/paks/~mods/x.pak` is visible to the game at that path. The overlay mechanism itself is not the obstacle. |
| 2 | On PC, Unreal Engine mounts `.pak` files found in `<Project>/Content/Paks` **and its subfolders**; mod managers use `Content/Paks/~mods`. Files with the `_P` suffix get a higher mount priority and override the game's packages. IoStore mods ship as `_P.pak` + `.utoc` + `.ucas`. | Stellar Blade install guides (Fextralife); Dmgvol/UE_Modding `UnrealPak.md`; Sifu modding wiki; Unreal Engine `FPakPlatformFile` API docs (`IterateDirectoryRecursively` explores subdirectories) | `~mods` is a convention of the engine's generic pak discovery, not of Windows. Whether a console build runs the same discovery is not documented publicly. |
| 3 | On jailbroken **PS4** consoles, Unreal mods were made to work by adding or replacing `.pak` files in the game's Paks folder (Dragon Ball FighterZ mods rebuilt into the update package; Final Fantasy VII Remake 60 FPS patch edits `pakchunk0-ps4.pak`). The DBFZ modder notes that **PC mods had to be converted to PS4 or showed texture problems**. | psxhax "Unreal Engine 4 (UE4) Game Modder v1.0 & DBFZ PS4 Mods by Markus95"; psxhax FF7R 60FPS FPKG thread; Nexus forum thread on converting PC mods to PS4 (hair textures missing) | Console UE builds do read extra/replaced paks. Platform-specific data (textures) breaks; non-texture data can work. |
| 4 | Official console mod support (mod.io for Unreal) requires each mod to be **cooked separately for each platform**, with the **same engine version and game release version** as the base game; console cooking needs the platform SDK. | mod.io docs: UGC best practices, cloud cooking, PlayStation platform page | A PC-cooked package has no guarantee on PS5. Akeno cannot cook for PS5 (no SDK, no conversion claimed). |
| 5 | Games that **sign** their containers require a matching `.sig` per pak, made with the game's key; failures surface as errors such as "Pak chunk signature check failed" that players have to fix before playing. | Unreal Engine API docs (`GetPakSignatureFile`, signed paks); Hell Let Loose support article "Pak chunk signature check failed" | A mod container cannot be loaded by a signed build; Akeno never offers a test then. |
| 6 | Tools such as UnrealReZen/retoc convert between legacy pak and IoStore and repack assets; they need the game's AES key for encrypted games and cannot add assets with new IDs without the container header. None converts PC-cooked data into PS5 formats. | rm-NoobInCoding/UnrealReZen README; Nexus Mods tool page | No PC tool or Akeno feature turns a PC texture/mesh/audio mod into a PS5 one. |
| 7 | No public report was found of a PC IoStore mod loaded by a PS5 build via `~mods` (searches across Nexus, Reddit, psxhax, GBAtemp, ConsoleMods, 2024-2026). | negative search result | The only way to know for a given game is a test on the console. |

## What this means

* **Data-only packages** (Blueprints, data tables, curves, gameplay values) are cooked into
  platform-neutral serialized objects; with unversioned properties they load only when the
  game's classes match the build they were cooked against. Better Carry Weight x10 is exactly
  this: one Blueprint package, `ExportBundleData` only, no bulk data, no shaders.
* **Textures, meshes, audio and shader code** are stored in platform-specific formats (bulk-data
  and shader chunks). PC versions of them are not PS5 data; they are ruled out.
* **Code mods** (UE4SS, LogicMods, DLLs) need a PC runtime. Ruled out, also as a test.
* Whether a PS5 build mounts extra containers from `~mods`, or from `Content/Paks` itself, is a
  per-game, per-version question that only a console test answers.

## What Akeno implements (the "test install")

A PC mod that Akeno does not activate on its own may be installed **as a test**, after the user
confirms the risks, when every check that can be made on the console passed:

1. It consists only of complete, verified Unreal package sets (`.pak` + `.utoc` + `.ucas`, or a
   classic `.pak` in a game without IoStore) and nothing else is installed.
2. No code, loader, `fakelib`, system folder, damaged container or encryption; compression only
   with methods the game's own containers use.
3. **Data only:** IoStore chunks are `ExportBundleData` and `ContainerHeader` only (no BulkData,
   OptionalBulkData, MemoryMappedBulkData, ShaderCode, ShaderCodeLibrary); classic paks contain
   only `.uasset`/`.uexp`.
4. The game's Unreal package folder was read: container versions not newer than the game's,
   imported `/Game/` packages present in the PS5 game, game containers **not signed**.
5. Every file is **added** inside the game's package folder; nothing replaces or hides a game file.
6. The user has not reported a crash of the same mod version on the same game version.

Names are lower-cased when every name in the game is lower case (PS5 packaging stores them so,
and the filesystem is case-sensitive). Two placements can be tested: `<paks>/~mods` (the PC
convention; default) and `<paks>` itself, next to the game's containers (no subfolder search
needed). After playing, the user reports **works**, **no effect** or **crashed**:

* *works* records the folder as one this game version loads (evidence source `report`), and the
  same mod version on the same game version as working (`VERIFIED_PS5` from the user's report);
* *no effect* suggests the other placement;
* *crashed* turns the mod off at once, keeps it off, and rules out another test of that mod
  version on that game version.

This is the "experiment mode" of the earlier roadmap, limited to what cannot harm the console:
files are only added to Akeno's overlay, the original game is never written, Vanilla removes
everything, and a crash of the game is recoverable by turning the mod off.

## First console attempt (2026-10-09)

The first build with test installs could not inspect the mod on the console: the check showed
"The .pak could not be read: A file cannot be opened" and "Installation path: none". The folder
of the extracted mod opened, but nothing below it: every file below an opened folder is reached
through a duplicated descriptor, and that build duplicated with `fcntl(F_DUPFD_CLOEXEC)`, which
the console refused (the game listing failed the same way, so no installation path could be
found). The hardware-tested 0.2.0-alpha used `dup()`, which is used again. The system check now
reports "folder-relative file access: OK/FAILED (step: errno)" so a console report shows this
directly.

## Unknowns that remain

* Whether PPSA28000 (or any PS5 UE5 title) mounts containers from `~mods` or from `Content/Paks`.
* Whether the PS5 build's `BP_PlayerCharacter` layout matches the PC build the mod was cooked
  against (the package is unversioned; Akeno compares package ids and imports, not class layouts).
* Whether the PS5 build checks container signatures without `.sig` files next to its paks
  (Akeno checks the container flags and `.sig` files it can see).
