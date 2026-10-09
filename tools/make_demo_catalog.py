#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates the demo Akeno Catalogue used by the tests and by tools/mock_shadowmount.py.

Everything is fictional and inert: the "mods" are tiny zip files with placeholder text.
Some deliberately contain things Akeno must refuse (a fake Windows DLL, a fakelib folder),
so the analyser can be tested. Output is deterministic.

Usage: python3 tools/make_demo_catalog.py [output_dir]   (default: tests/fixtures/catalog)
"""

import hashlib
import io
import json
import os
import struct
import sys
import zipfile

BASE_URL = "http://127.0.0.1:10101/files/"  # served by the mock server (loopback only)
FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def make_zip(entries):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, data)
    return buffer.getvalue()


def placeholder(text):
    return ("AKENO DEMO PLACEHOLDER - not real game content.\n" + text + "\n").encode()


# --- Minimal, structurally valid Unreal containers ------------------------------------------
# Akeno checks .pak/.utoc/.ucas files by their real structure (footers, SHA-1 index hashes,
# BLAKE3 chunk hashes), so the demo containers are well-formed. Their payload is placeholder
# text; nothing here is game data.

_IV = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19]
_PERM = [2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8]
_M32 = 0xFFFFFFFF


def _compress(cv, words, counter, length, flags):
    def rotr(x, n):
        return ((x >> n) | (x << (32 - n))) & _M32

    s = list(cv) + _IV[:4] + [counter & _M32, (counter >> 32) & _M32, length, flags]
    m = list(words)

    def g(a, b, c, d, x, y):
        s[a] = (s[a] + s[b] + x) & _M32
        s[d] = rotr(s[d] ^ s[a], 16)
        s[c] = (s[c] + s[d]) & _M32
        s[b] = rotr(s[b] ^ s[c], 12)
        s[a] = (s[a] + s[b] + y) & _M32
        s[d] = rotr(s[d] ^ s[a], 8)
        s[c] = (s[c] + s[d]) & _M32
        s[b] = rotr(s[b] ^ s[c], 7)

    for r in range(7):
        g(0, 4, 8, 12, m[0], m[1]); g(1, 5, 9, 13, m[2], m[3]); g(2, 6, 10, 14, m[4], m[5]); g(3, 7, 11, 15, m[6], m[7])
        g(0, 5, 10, 15, m[8], m[9]); g(1, 6, 11, 12, m[10], m[11]); g(2, 7, 8, 13, m[12], m[13]); g(3, 4, 9, 14, m[14], m[15])
        if r < 6:
            m = [m[i] for i in _PERM]
    return [s[i] ^ s[i + 8] for i in range(8)] + [s[i + 8] ^ cv[i] for i in range(8)]


def _words(block):
    block = block.ljust(64, b"\0")
    return list(struct.unpack("<16I", block))


def blake3(data, length=32):
    """Unkeyed BLAKE3 (reference algorithm, single-threaded)."""
    chunks = [data[i:i + 1024] for i in range(0, len(data), 1024)] or [b""]
    stack = []
    for index, chunk in enumerate(chunks):
        cv = _IV
        blocks = [chunk[i:i + 64] for i in range(0, len(chunk), 64)] or [b""]
        for n, block in enumerate(blocks):
            flags = (1 if n == 0 else 0) | (2 if n == len(blocks) - 1 else 0)
            last = n == len(blocks) - 1
            if last:
                node = (cv, _words(block), index, len(block), flags)
            else:
                cv = _compress(cv, _words(block), index, 64, flags)[:8]
        if index == len(chunks) - 1:
            break
        value = _compress(*node)[:8]
        total = index + 1
        while total & 1 == 0:
            value = _compress(_IV, stack.pop() + value, 0, 64, 4)[:8]
            total >>= 1
        stack.append(value)
    while stack:
        node = (_IV, stack.pop() + _compress(*node)[:8], 0, 64, 4)
    cv, words, counter, blen, flags = node
    out = b"".join(struct.pack("<16I", *_compress(cv, words, i, blen, flags | 8)) for i in range((length + 63) // 64))
    return out[:length]


def _fstring(text):
    raw = text.encode("ascii") + b"\0"
    return struct.pack("<i", len(raw)) + raw


def _sha1(data):
    return hashlib.sha1(data).digest()


def _pak_footer(version, index_offset, index):
    return (b"\0" * 16 + b"\0" + struct.pack("<IiqQ", 0x5A6F12E1, version, index_offset, len(index)) + _sha1(index)
            + b"\0" * 160)


def pak_stub():
    """An IoStore companion .pak (version 11): a mount point and no files, as the cooker writes it."""
    path_hash_index = b"\0" * 8
    directory_index = struct.pack("<i", 0)
    head = _fstring("../../../") + struct.pack("<iQ", 0, 0)
    size = len(head) + 4 + 36 + 4 + 36 + 4 + 4
    index = (head + struct.pack("<Iqq", 1, size, len(path_hash_index)) + _sha1(path_hash_index)
             + struct.pack("<Iqq", 1, size + len(path_hash_index), len(directory_index)) + _sha1(directory_index)
             + struct.pack("<ii", 0, 0))
    return index + path_hash_index + directory_index + _pak_footer(11, 0, index)


def pak_single(path, data):
    """A version 8 .pak with one stored (uncompressed) file."""
    entry = struct.pack("<qqqI", 0, len(data), len(data), 0) + _sha1(data) + b"\0" + struct.pack("<I", 0)
    blob = entry + data
    index = _fstring("../../../") + struct.pack("<i", 1) + _fstring(path) + entry
    return blob + index + _pak_footer(8, len(blob), index)


def iostore(container_id, package_id, file_name, payload):
    """A version 8 IoStore container: one package chunk (placeholder payload) and its container header."""
    header_chunk = struct.pack("<IIQi", 0x496F436E, 4, container_id, 1) + struct.pack("<Q", package_id)
    chunks = [(package_id, 1, payload), (container_id, 6, header_chunk)]
    block_size = 65536
    cas, blocks, offsets, virtual = b"", [], [], 0
    for _, _, data in chunks:
        offsets.append((virtual, len(data)))
        count = max(1, -(-len(data) // block_size))
        for i in range(count):
            piece = data[i * block_size:(i + 1) * block_size]
            blocks.append((len(cas), len(piece)))
            cas += piece
        virtual += count * block_size
    directory = (_fstring("../../../") + struct.pack("<i", 1) + struct.pack("<IIII", _M32, _M32, _M32, 0)
                 + struct.pack("<i", 1) + struct.pack("<III", 0, _M32, 0) + struct.pack("<i", 1) + _fstring(file_name))
    header = (b"-==--==--==--==-" + struct.pack("<BBHIIIIIIIIIQ", 8, 0, 0, 144, len(chunks), len(blocks), 12, 0, 32,
                                                  block_size, len(directory), 1, container_id)
              + b"\0" * 16 + struct.pack("<BBHIQII", 8, 0, 0, 0, 0xFFFFFFFFFFFFFFFF, 0, 0) + b"\0" * 40)
    toc = header
    for chunk_id, chunk_type, _ in chunks:
        toc += struct.pack("<Q", chunk_id) + b"\0\0\0" + bytes([chunk_type])
    for offset, length in offsets:
        toc += offset.to_bytes(5, "big") + length.to_bytes(5, "big")
    for offset, length in blocks:
        toc += offset.to_bytes(5, "little") + length.to_bytes(3, "little") + length.to_bytes(3, "little") + b"\0"
    toc += directory
    for _, _, data in chunks:
        toc += blake3(data, 20) + b"\0" * 4
    return toc, cas


def iostore_files(base, container_id, package_id, asset_name, text):
    toc, cas = iostore(container_id, package_id, asset_name, placeholder(text))
    return [(base + ".pak", pak_stub()), (base + ".utoc", toc), (base + ".ucas", cas)]


GAMES = [
    {
        "id": "example-blade",
        "name": "Example Blade",
        "titleIds": ["PPSA90001"],
        "engine": "unreal",
        "mods": [
            {
                "id": "crimson-outfit",
                "name": "Crimson Outfit Recolour",
                "author": "DemoAuthor",
                "version": "1.2.0",
                "summary": "Recolours the default outfit in deep crimson.",
                "categories": ["outfits"],
                "status": "verified",
                "gameVersions": ["01.011.000"],
                "modType": "asset-replacement",
                "updatedAt": "2026-09-20T10:00:00Z",
                "files": iostore_files("ExampleBlade/Content/Paks/~mods/CrimsonOutfit_P", 0x1A2B3C4D5E6F7081,
                                       0x0102030405060708, "CrimsonOutfit.uasset", "outfit package"),
                "screenshots": 2,
            },
            {
                "id": "sharper-foliage",
                "name": "Sharper Foliage Textures",
                "author": "TextureFan",
                "version": "0.9.1",
                "summary": "Higher resolution grass and tree textures.",
                "categories": ["textures"],
                "status": "verified",
                "gameVersions": ["01.010.000"],
                "modType": "asset-replacement",
                "updatedAt": "2026-08-01T10:00:00Z",
                "files": [("ExampleBlade/Content/Paks/~mods/Foliage_P.pak",
                           pak_single("ExampleBlade/Content/Foliage/T_Grass.uasset", placeholder("foliage texture")))],
                "screenshots": 1,
            },
            {
                "id": "alt-hair-colors",
                "name": "Alternative Hair Colours",
                "author": "DemoAuthor",
                "version": "2.0.0",
                "summary": "Five hair colour options for the main character.",
                "categories": ["characters"],
                "status": "likely",
                "gameVersions": [],
                "modType": "asset-replacement",
                "updatedAt": "2026-10-02T10:00:00Z",
                "files": [("ExampleBlade/Content/Paks/~mods/HairColours_P.pak",
                           pak_single("ExampleBlade/Content/Characters/Hair/MI_Hair.uasset", placeholder("hair material")))],
                "screenshots": 3,
            },
            {
                "id": "photo-mode-ue4ss",
                "name": "Photo Mode Unlocker (UE4SS)",
                "author": "PcModder",
                "version": "1.0.0",
                "summary": "PC script mod that needs UE4SS. Shown here to demonstrate the PC ONLY label.",
                "categories": ["utilities"],
                "status": "pc-only",
                "gameVersions": [],
                "modType": "script",
                "updatedAt": "2026-07-15T10:00:00Z",
                "files": [("dinput8.dll", b"MZ" + placeholder("fake windows dll")),
                          ("ue4ss/UE4SS.dll", b"MZ" + placeholder("fake ue4ss")),
                          ("ue4ss/Mods/PhotoMode/main.lua", placeholder("lua script"))],
                "screenshots": 0,
            },
            {
                "id": "mystery-pack",
                "name": "Mystery Asset Pack",
                "author": "unknown",
                "version": "0.1",
                "summary": "No compatibility information is available for this pack.",
                "categories": ["misc"],
                "status": "unknown",
                "gameVersions": [],
                "modType": "asset-addition",
                "updatedAt": "2026-06-01T10:00:00Z",
                "files": [("ExampleBlade/Content/Paks/~mods/Mystery_P.pak",
                           pak_single("ExampleBlade/Content/Mystery/Thing.uasset", placeholder("mystery asset"))),
                          ("fakelib/libSceAmpr.sprx", placeholder("library overlay that must be refused"))],
                "screenshots": 0,
            },
            {
                "id": "akeno-test-file",
                "name": "Akeno Harmless Test Package",
                "author": "Akeno",
                "version": "1.0.0",
                "summary": "Adds one text file to the game folder. Used to test the install pipeline safely.",
                "categories": ["test"],
                "status": "verified",
                "gameVersions": ["01.011.000"],
                "modType": "test-harmless",
                "updatedAt": "2026-10-05T10:00:00Z",
                "files": [("akeno-test/README.txt",
                           b"This file was installed by the Akeno PS5 Mod Manager test package.\n"
                           b"It is never read by the game and can be removed at any time.\n")],
                "screenshots": 0,
            },
        ],
    },
    {
        "id": "neon-harbor",
        "name": "Neon Harbor",
        "titleIds": ["PPSA90002"],
        "engine": "unknown",
        "mods": [
            {
                "id": "neon-hud-tint",
                "name": "Teal HUD Tint",
                "author": "HudMaker",
                "version": "1.0.0",
                "summary": "Changes the HUD accent colour to teal.",
                "categories": ["ui"],
                "status": "likely",
                "gameVersions": ["01.004.000"],
                "modType": "asset-replacement",
                "updatedAt": "2026-09-01T10:00:00Z",
                "files": [("data/ui/hud_colours.cfg", placeholder("hud colours"))],
                "screenshots": 1,
            },
        ],
    },
    {
        "id": "distant-tides",
        "name": "Distant Tides (not installed in the demo)",
        "titleIds": ["PPSA90099"],
        "engine": "unknown",
        "mods": [],
    },
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/catalog"
    files_dir = os.path.join(out, "files")
    os.makedirs(files_dir, exist_ok=True)
    index = {"schemaVersion": 1, "name": "Akeno Demo Catalogue", "updatedAt": "2026-10-07T00:00:00Z", "games": []}
    for game in GAMES:
        os.makedirs(os.path.join(out, "mods", game["id"]), exist_ok=True)
        game_doc = {"schemaVersion": 1, "id": game["id"], "name": game["name"], "titleIds": game["titleIds"],
                    "engine": game["engine"], "mods": []}
        for mod in game["mods"]:
            archive = make_zip(mod["files"])
            archive_name = f"{game['id']}-{mod['id']}-{mod['version']}.zip"
            with open(os.path.join(files_dir, archive_name), "wb") as f:
                f.write(archive)
            sha = hashlib.sha256(archive).hexdigest()
            shots = [{"url": f"{BASE_URL}{game['id']}-{mod['id']}-shot{i + 1}.png", "caption": f"Screenshot {i + 1}"}
                     for i in range(mod["screenshots"])]
            thumb = f"{BASE_URL}{game['id']}-{mod['id']}-thumb.png"
            manifest = {
                "schemaVersion": 1,
                "id": mod["id"],
                "name": mod["name"],
                "author": mod["author"],
                "version": mod["version"],
                "summary": mod["summary"],
                "description": mod["summary"] + " This is fictional demo content for testing Akeno; the archive "
                               "only contains placeholder text files.",
                "categories": mod["categories"],
                "license": "demo content, CC0",
                "homepage": "https://github.com/YTDxDAkeno/AKENO-PS5-MOD-MANAGER",
                "updatedAt": mod["updatedAt"],
                "game": {"titleIds": game["titleIds"], "versions": mod["gameVersions"]},
                "platform": "ps5",
                "compatibility": {"status": mod["status"], "engine": game["engine"], "modType": mod["modType"],
                                  "notes": "Demo entry."},
                "download": {"url": BASE_URL + archive_name, "size": len(archive), "sha256": sha, "format": "zip"},
                "installation": {"method": "shadowmount-overlay", "archiveRoot": "", "targetPrefix": ""},
                "media": {"thumbnail": thumb, "screenshots": shots},
                "dependencies": {"requires": [], "recommends": [], "conflictsWith": [], "loadAfter": [],
                                 "loadBefore": []},
            }
            with open(os.path.join(out, "mods", game["id"], mod["id"] + ".json"), "w") as f:
                json.dump(manifest, f, indent=2)
                f.write("\n")
            game_doc["mods"].append({
                "id": mod["id"], "name": mod["name"], "author": mod["author"], "version": mod["version"],
                "summary": mod["summary"], "categories": mod["categories"], "compatibility": mod["status"],
                "gameVersions": mod["gameVersions"], "thumbnail": thumb, "downloadSize": len(archive),
                "updatedAt": mod["updatedAt"],
            })
        with open(os.path.join(out, "games", game["id"] + ".json") if os.path.isdir(os.path.join(out, "games"))
                  else _mkdir_then(os.path.join(out, "games"), game["id"] + ".json"), "w") as f:
            json.dump(game_doc, f, indent=2)
            f.write("\n")
        index["games"].append({"id": game["id"], "name": game["name"], "titleIds": game["titleIds"],
                               "modCount": len(game["mods"])})
    with open(os.path.join(out, "index.json"), "w") as f:
        json.dump(index, f, indent=2)
        f.write("\n")
    print("demo catalogue written to", out)


def _mkdir_then(directory, name):
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, name)


if __name__ == "__main__":
    main()
