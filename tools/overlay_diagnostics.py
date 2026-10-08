#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline overlay inventory. Reads a local snapshot; writes JSON only to stdout.

No Akeno initialization, database, archive extraction, networking, mounting or
activation. See docs/overlay-diagnostics.md for the baseline manifest contract.
"""
import argparse
import hashlib
import json
import os
from pathlib import PurePosixPath
import re
import stat
import sys

MAX_ENTRIES = 20000
MAX_FILE_BYTES = 4 * 1024**3
MAX_TOTAL_BYTES = 16 * 1024**3
MAX_MANIFEST_BYTES = 16 * 1024**2
RESERVED = {'fakelib', 'fakelib2', 'sce_sys', 'sce_module'}
LOADERS = {'modconfig.json', 'modinfo.ini', 'dsts-loader', 'reloaded-ii', 'reloaded.mod.loader'}
PC_EXTENSIONS = {'.exe', '.dll', '.asi', '.sys', '.com', '.scr', '.msi', '.pdb', '.cpl', '.ocx'}
NATIVE_EXTENSIONS = {'.elf', '.self', '.prx', '.sprx', '.so', '.dylib'}


def path_errors(path):
    if not isinstance(path, str) or not path:
        return ['empty or non-string relative path']
    errors = []
    parts = path.split('/')
    if len(os.fsencode(path)) >= 1024 or len(parts) > 64:
        errors.append('relative path exceeds diagnostic length/depth limit')
    if any(p in ('', '.', '..') for p in parts) or '\\' in path or ':' in path:
        errors.append('unsafe relative path')
    try:
        path.encode('utf-8')
    except UnicodeEncodeError:
        errors.append('invalid UTF-8 path')
    if any(ord(c) < 32 or ord(c) == 127 for c in path):
        errors.append('control character in path')
    if any(len(os.fsencode(p)) > 255 for p in parts):
        errors.append('component exceeds 255 bytes')
    return errors


def open_directory(path):
    """Open each ancestor without following symbolic links, including the root."""
    path = os.fspath(path)
    if '..' in path.split('/'):
        raise ValueError('input paths must not contain ..')
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in os.path.abspath(path).split('/'):
            if not part:
                continue
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def signature(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def read_manifest(path, title_id):
    parent, name = os.path.split(os.fspath(path))
    directory = open_directory(parent or '.')
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    finally:
        os.close(directory)
    with os.fdopen(fd, 'rb') as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError('baseline manifest is not a regular file')
        content = stream.read(MAX_MANIFEST_BYTES + 1)
    if len(content) > MAX_MANIFEST_BYTES:
        raise ValueError('baseline manifest exceeds size limit')
    data = json.loads(content)
    if not isinstance(data, dict) or data.get('schemaVersion') != 1 or data.get('titleId') != title_id:
        raise ValueError('baseline schema or title ID does not match')
    entries = data.get('entries')
    if not isinstance(entries, list) or len(entries) > MAX_ENTRIES:
        raise ValueError('baseline needs a bounded entries array')
    if type(data.get('complete')) is not bool:
        raise ValueError('baseline must explicitly declare complete: true or false')
    paths = {}
    for entry in entries:
        if not isinstance(entry, dict) or path_errors(entry.get('path')):
            raise ValueError('unsafe baseline path')
        path = entry['path']
        if path in paths or entry.get('type') not in ('file', 'directory'):
            raise ValueError('duplicate baseline path or unsupported type')
        if 'sha256' in entry and not re.fullmatch('[0-9a-f]{64}', str(entry['sha256'])):
            raise ValueError('invalid baseline hash')
        paths[path] = entry
    # Infer directory entries from files, but reject contradictory manifests.
    for path in list(paths):
        for parent in PurePosixPath(path).parents:
            if str(parent) == '.':
                continue
            if str(parent) in paths and paths[str(parent)]['type'] != 'directory':
                raise ValueError('baseline has a file as a parent directory')
            paths.setdefault(str(parent), {'path': str(parent), 'type': 'directory'})
            if len(paths) > MAX_ENTRIES:
                raise ValueError('baseline inferred directories exceed entry limit')
    data['paths'] = paths
    return data


def diagnose(root, title_id, source_type='unknown', baseline=None, game_version='', firmware='', smp=''):
    if not re.fullmatch('[A-Z]{4}[0-9]{5}', title_id):
        raise ValueError('invalid title ID')
    if source_type not in ('unknown', 'folder', 'image', 'pkg'):
        raise ValueError('invalid source type')
    findings, entries, spellings = [], [], {}
    total = 0
    complete = True

    def finding(code, path, message, level='blocker'):
        findings.append(dict(code=code, path=path, message=message, level=level))

    def walk(fd, prefix='', depth=0):
        nonlocal total, complete
        before = os.fstat(fd)
        # Bound directory enumeration as well as the report size.
        names = []
        with os.scandir(fd) as scan:
            for item in scan:
                names.append(item.name)
                if len(names) + len(entries) > MAX_ENTRIES:
                    raise ValueError('overlay entry limit exceeded')
        for name in sorted(names):
            path = prefix + name
            if len(entries) >= MAX_ENTRIES:
                raise ValueError('overlay entry limit exceeded')
            info = os.stat(name, dir_fd=fd, follow_symlinks=False)
            kind = 'file' if stat.S_ISREG(info.st_mode) else 'directory' if stat.S_ISDIR(info.st_mode) else 'unsupported'
            entry = dict(path=path, type=kind)
            entries.append(entry)
            for error in path_errors(path):
                finding('unsafe-path', path, error)
            if len(os.fsencode('/data/homebrew/backports/' + title_id + '/' + path)) >= 1024:
                finding('smp-path-limit', path, 'Default backport source path exceeds SMP MAX_PATH (1024 incl. NUL).')
            # Conservative on depth; a PKG missing-directory redirect may avoid recursion.
            if depth >= 64:
                complete = False
                finding('depth-limit', path, 'Diagnostic traversal limit reached; subtree was not inspected.')
                continue
            folded = path.lower()
            if folded in spellings and spellings[folded] != path:
                finding('case-collision', path, 'Case differs from ' + spellings[folded])
            spellings[folded] = path
            parts = folded.split('/')
            if parts[0] in RESERVED:
                finding('reserved-path', path, 'Reserved system/library overlay path.')
            if set(parts) & LOADERS or 'ue4ss' in folded:
                finding('pc-loader', path, 'Unsupported PC loader marker; no PS5 loader is supplied by an overlay.')
            if kind == 'directory':
                child = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                try:
                    if signature(os.fstat(child)) != signature(info):
                        raise ValueError('directory changed during inspection')
                    walk(child, path + '/', depth + 1)
                finally:
                    os.close(child)
            elif kind == 'file':
                if info.st_size > MAX_FILE_BYTES or total + info.st_size > MAX_TOTAL_BYTES:
                    raise ValueError('overlay byte limit exceeded')
                file_fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
                with os.fdopen(file_fd, 'rb') as stream:
                    opened = os.fstat(stream.fileno())
                    if not stat.S_ISREG(opened.st_mode) or signature(opened) != signature(info):
                        raise ValueError('file changed during inspection')
                    digest, head, count = hashlib.sha256(), b'', 0
                    while chunk := stream.read(1024 * 1024):
                        head = (head + chunk)[:64]
                        count += len(chunk)
                        if count > MAX_FILE_BYTES or total + count > MAX_TOTAL_BYTES:
                            raise ValueError('overlay grew beyond byte limit')
                        digest.update(chunk)
                    if count != info.st_size or signature(os.fstat(stream.fileno())) != signature(info):
                        raise ValueError('file changed during inspection')
                total += count
                entry.update(size=count, sha256=digest.hexdigest())
                ext = PurePosixPath(folded).suffix
                if head.startswith(b'MZ') or ext in PC_EXTENSIONS:
                    finding('windows-code', path, 'Windows executable/library content or extension.')
                if head.startswith((b'\x7fELF', b'\x4f\x15\x3d\x1d')) or ext in NATIVE_EXTENSIONS or parts[-1] == 'eboot.bin':
                    finding('native-code', path, 'Native executable/library content or extension.')
            else:
                complete = False
                finding('unsupported-entry', path, 'Symlink or special file: not followed or read.')
        if signature(os.fstat(fd)) != signature(before):
            raise ValueError('directory changed during inspection')

    fd = open_directory(root)
    try:
        walk(fd)
    finally:
        os.close(fd)

    baseline_complete = baseline is not None and baseline['complete']
    base = baseline['paths'] if baseline else {}
    folded_base = {}
    for path in base:
        folded_base.setdefault(path.lower(), []).append(path)
    if baseline and game_version and baseline.get('gameVersion') != game_version:
        finding('version-mismatch', '', 'Baseline version does not match the supplied game version.')
    for entry in entries:
        path = entry['path']
        original = base.get(path)
        entry['gamePathCheck'] = 'unknown'
        if original:
            entry['gamePathCheck'] = 'existing-' + original['type']
            if original['type'] != entry['type']:
                finding('type-conflict', path, 'Overlay and baseline types differ.')
            if entry['type'] == 'file':
                finding('replacement', path, 'Replaces a game file; matching path is not format compatibility.', 'warning')
                if original.get('sha256'):
                    entry['sameHashAsBaseline'] = original['sha256'] == entry['sha256']
        elif path.lower() in folded_base:
            entry['gamePathCheck'] = 'case-mismatch'
            finding('baseline-case-mismatch', path, 'Baseline path differs in case: ' + ', '.join(folded_base[path.lower()]))
        elif baseline_complete:
            entry['gamePathCheck'] = 'absent-from-complete-baseline'
        for parent in PurePosixPath(path).parents:
            if str(parent) in base and base[str(parent)]['type'] == 'file':
                finding('parent-conflict', path, 'Baseline has a file where an overlay directory is needed.')

    redirects = dict(applicable=source_type == 'pkg', exact=None, upperBound=None,
                     basis='supplied-baseline-only' if baseline else 'no-baseline')
    if source_type == 'pkg':
        redirects['upperBound'] = len(entries)
        if baseline_complete and complete and not any(f['level'] == 'blocker' for f in findings):
            new_directories, count = [], 0
            for entry in sorted(entries, key=lambda e: (e['path'].count('/'), e['path'])):
                path = entry['path']
                if any(path.startswith(parent + '/') for parent in new_directories):
                    continue
                if entry['type'] == 'file':
                    count += 1
                elif path not in base:
                    new_directories.append(path)
                    count += 1
                elif path.count('/') + 1 >= 64:
                    finding('pkg-depth-limit', path, 'SMP refuses recursion into an existing directory at depth 64.')
            redirects['exact'] = count
            if count > 256:
                finding('pkg-redirect-limit', '', 'Baseline-based redirect count exceeds 256.')
        else:
            finding('pkg-count-unknown', '', 'Exact redirects require a complete matching app0 manifest; file count is insufficient.', 'warning')
    if not baseline:
        finding('baseline-missing', '', 'No game manifest: replacements, path case and game version are unknown.', 'warning')
    finding('format-unverified', '', 'PC/PS5 asset formats and loader dependencies cannot be proved compatible by filenames or hashes.', 'warning')
    return dict(schemaVersion=1, titleId=title_id, sourceType=source_type,
                suppliedContext=dict(gameVersion=game_version, firmware=firmware, shadowMountVersion=smp),
                scanComplete=complete, fileCount=sum(e['type'] == 'file' for e in entries), totalBytes=total,
                entries=entries, findings=findings, pkgRedirects=redirects,
                compatibility='blocked' if any(f['level'] == 'blocker' for f in findings) else 'unknown',
                activationAllowed=False, mountObservation='not-performed', gameConsumption='not-observed',
                hardwareVerified=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--overlay', required=True, help='Local snapshot directory, already mapped relative to the game root')
    parser.add_argument('--title-id', required=True)
    parser.add_argument('--source-type', choices=('unknown', 'folder', 'image', 'pkg'), default='unknown')
    parser.add_argument('--game-manifest')
    parser.add_argument('--game-version', default='')
    parser.add_argument('--firmware', default='')
    parser.add_argument('--shadowmount-version', default='')
    args = parser.parse_args(argv)
    try:
        baseline = read_manifest(args.game_manifest, args.title_id) if args.game_manifest else None
        report = diagnose(args.overlay, args.title_id, args.source_type, baseline, args.game_version,
                          args.firmware, args.shadowmount_version)
    except (OSError, ValueError, RecursionError) as error:
        print(json.dumps(dict(schemaVersion=1, scanComplete=False, compatibility='blocked',
                              activationAllowed=False, error=str(error))))
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return 1 if report['compatibility'] == 'blocked' else 0


if __name__ == '__main__':
    sys.exit(main())
