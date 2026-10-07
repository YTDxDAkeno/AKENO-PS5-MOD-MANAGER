#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes assets/test/download-test.zip, the harmless file used by `--download-test`.

The archive is deterministic (fixed timestamps, stored entries), so its SHA-256 stays the
same. CMake reads the size and SHA-256 from the file at configure time.
"""

import os
import sys
import zipfile

TEXT = (
    "This harmless file tests downloads in Akeno PS5 Mod Manager.\n"
    "It contains no code and is deleted again after the test.\n"
)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join("assets", "test", "download-test.zip")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_STORED) as archive:
        info = zipfile.ZipInfo("akeno-download-test/README.txt", date_time=(2026, 10, 7, 0, 0, 0))
        info.external_attr = 0o644 << 16
        archive.writestr(info, TEXT)
    print("written", out)


if __name__ == "__main__":
    main()
