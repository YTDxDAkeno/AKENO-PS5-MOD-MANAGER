#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local stand-in for the ShadowMountPlus HTTP/JSON API v1 (read-only routes).

For developing and demonstrating Akeno on a desktop without a PS5. It answers the routes
Akeno uses, following ShadowMountPlus docs/openapi.yaml (1.7), with fictional demo games:

  POST /api/v1/version   POST /api/v1/games   POST /api/v1/storage   POST /api/v1/settings
  GET  /api/v1/games/icon?title_id=...

Usage:
  python3 tools/mock_shadowmount.py [--port 10101] [--empty] [--busy]

It binds to 127.0.0.1 only. Mutating routes answer 403: Akeno must never call them.
"""

import argparse
import json
import struct
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

CAPABILITIES = [
    "web_ui", "storage_space", "list_images", "list_games", "game_info", "game_icon", "mount_game",
    "unmount_game", "uninstall_game", "move_game_source", "copy_game_source", "delete_game_source",
    "unpack_game_image", "storage_job_status", "storage_job_cancel", "list_manual_sources",
    "add_manual_source", "remove_manual_source", "manage_settings", "read_debug_log", "read_kernel_log",
    "rescan",
]

# Fictional titles with made-up title IDs, for screenshots and manual testing.
DEMO_GAMES = [
    ("PPSA90001", "Example Blade", "01.011.000", "folder", "ps5", "2026-10-01 20:15:00", (64, 96, 200)),
    ("PPSA90002", "Neon Harbor", "01.004.000", "image", "ps5", "2026-09-28 18:00:00", (200, 60, 140)),
    ("PPSA90003", "Skyline Drift", "02.100.000", "image", "ps5", "", (40, 170, 160)),
    ("PPSA90004", "Iron Meridian", "01.000.000", "folder", "ps5", "2026-10-05 21:30:00", (190, 120, 40)),
    ("PPSA90005", "Quiet Orchard", "01.002.000", "pkg", "ps5", "", (90, 160, 70)),
    ("PPSA90006", "Starfall Tactics", "", "folder", "ps5", "", (120, 80, 200)),
    ("PPSA90007", "Lantern Road", "01.050.000", "image", "ps5", "2026-08-12 10:00:00", (210, 90, 70)),
    ("CUSA90008", "Retro Rally (PS4)", "01.05", "pkg", "ps4", "", (110, 110, 120)),
]


def png_gradient(color, size=256):
    """A simple diagonal gradient PNG used as a placeholder game icon."""
    r0, g0, b0 = color
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            t = (x + y) / (2 * size)
            row += bytes((int(r0 * (1 - t) + 20 * t), int(g0 * (1 - t) + 24 * t), int(b0 * (1 - t) + 40 * t)))
        rows.append(bytes(row))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")


def game_entry(title_id, name, version, source, platform, last_access):
    pkg = source == "pkg"
    return {
        "path": f"/user/app/{title_id}/app.pkg" if pkg else f"/data/homebrew/{title_id}",
        "runtime_path": "" if pkg else f"/system_ex/app/{title_id}",
        "source_type": source,
        "image_type": "ufs" if source == "image" else "",
        "platform": platform,
        "title_id": title_id,
        "content_id": f"UP0000-{title_id}_00-DEMOCONTENT00000",
        "title_name": name,
        "version": version,
        "last_access_time": last_access,
        "install_time": "",
        "icon_url": f"/api/v1/games/icon?title_id={title_id}",
        "app_db_size_bytes": 10_000_000_000 + int(title_id[4:]) * 1_000_000,
        "installed": True,
        "managed": not pkg,
        "mounted": False,
        "image_backed": source == "image",
        "source_available": True,
        "installed_pkg": pkg,
        "can_uninstall": True,
        "can_manage_source": not pkg,
        "can_toggle_fakelib": platform == "ps5",
        "fakelib_enabled": True,
        "fakelib_effective_enabled": True,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "MockShadowMount/1.7"

    def send_json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):  # noqa: N802 (http.server naming)
        length = int(self.headers.get("Content-Length") or 0)
        if length > 4096:
            return self.send_json(413, {"status": 27, "error": "request body too large"})
        self.rfile.read(length)
        options = self.server.options
        route = urlparse(self.path).path
        if options.busy:
            return self.send_json(409, {"status": 16, "error": "a game is running", "error_reason": "game_active"})
        if route == "/api/v1/version":
            return self.send_json(200, {"status": 0, "api_version": 1, "shadowmount_version": "1.7 (mock)",
                                        "capabilities": CAPABILITIES})
        if route == "/api/v1/games":
            games = [] if options.empty else [game_entry(*g[:6]) for g in DEMO_GAMES]
            return self.send_json(200, {"status": 0, "count": len(games), "size_included": False, "games": games})
        if route == "/api/v1/storage":
            mount = {"source": "/dev/ssd0.user", "mount_point": "/user", "filesystem": "ufs",
                     "total_bytes": 824_633_720_832, "free_bytes": 412_316_860_416,
                     "available_bytes": 400_000_000_000, "used_bytes": 424_633_720_832, "read_only": False}
            return self.send_json(200, {"status": 0, "count": 1, "destination_count": 0, "mounts": [mount],
                                        "destinations": []})
        if route == "/api/v1/settings":
            return self.send_json(200, {"status": 0, "scan_paths": []})
        return self.send_json(403, {"status": 1, "error": "the mock only implements read-only routes"})

    def do_GET(self):  # noqa: N802
        url = urlparse(self.path)
        if url.path == "/api/v1/games/icon":
            title_id = parse_qs(url.query).get("title_id", [""])[0]
            for game in DEMO_GAMES:
                if game[0] == title_id:
                    body = png_gradient(game[6])
                    self.send_response(200)
                    self.send_header("Content-Type", "image/png")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
            return self.send_json(404, {"status": 2, "error": "unknown title"})
        return self.send_json(404, {"status": 2, "error": "not found"})

    def log_message(self, fmt, *args):
        print("[mock-shadowmount]", fmt % args)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=10101)
    parser.add_argument("--empty", action="store_true", help="report no games")
    parser.add_argument("--busy", action="store_true", help="answer every request with EBUSY")
    options = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", options.port), Handler)
    server.options = options
    print(f"mock ShadowMountPlus API on http://127.0.0.1:{options.port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
