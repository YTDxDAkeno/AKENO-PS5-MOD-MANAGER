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
                "files": [("ExampleBlade/Content/Paks/~mods/CrimsonOutfit_P.pak", placeholder("outfit pak")),
                          ("ExampleBlade/Content/Paks/~mods/CrimsonOutfit_P.utoc", placeholder("outfit utoc")),
                          ("ExampleBlade/Content/Paks/~mods/CrimsonOutfit_P.ucas", placeholder("outfit ucas"))],
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
                "files": [("ExampleBlade/Content/Paks/~mods/Foliage_P.pak", placeholder("foliage pak"))],
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
                "files": [("ExampleBlade/Content/Paks/~mods/HairColours_P.pak", placeholder("hair pak"))],
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
                "files": [("ExampleBlade/Content/Paks/~mods/Mystery_P.pak", placeholder("mystery pak")),
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
