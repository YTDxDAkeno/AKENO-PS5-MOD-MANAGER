# Better Carry Weight x10 (PPSA28000) fixture

`archive.json` records the structure of the real Nexus Mods archive that 0.2.0-alpha installed
on a PS5 (firmware 12.20, ShadowMountPlus 1.7beta4) on 2026-10-08: entry names, sizes and
SHA-256 values, and what the container headers contain. The mod's files themselves are not in
this repository.

`tests/mods/test_bcw_case.cpp` rebuilds synthetic containers with the same names and the same
structure (a version 11 companion `.pak` with no files, a version 8 IoStore `.utoc` with one
`ExportBundleData` chunk for `/Game/_Dawnwalker/Player/BP_PlayerCharacter` and one container
header chunk, BLAKE3-160 chunk hashes, an uncompressed `.ucas`), places them under the same
wrapper folder, and runs the check and the installer against a synthetic game folder laid out
like the observed PPSA28000 installation (`dawnwalker/content/paks/dawnwalker-ps5.*`).

What the tests establish on a host: the wrapper is detected, the files are grouped and verified
as one package set, the proposed target is `dawnwalker/content/paks/~mods` with the confidence
`candidate`, and activation is refused. What they cannot establish: whether the PS5 build of the
game mounts containers from `~mods`, and whether the PC-cooked package works on PS5.
