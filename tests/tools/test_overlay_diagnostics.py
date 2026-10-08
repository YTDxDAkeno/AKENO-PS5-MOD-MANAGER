# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[2] / 'tools' / 'overlay_diagnostics.py'
spec = importlib.util.spec_from_file_location('overlay_diagnostics', MODULE)
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)
TITLE = 'PPSA24701'


class OverlayDiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'overlay'
        self.root.mkdir()

    def write(self, path, data=b'x'):
        file = self.root / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
        return file

    def baseline(self, entries, complete=True, **extra):
        path = Path(self.temp.name) / 'baseline.json'
        path.write_text(json.dumps(dict(schemaVersion=1, titleId=TITLE,
                                       complete=complete, entries=entries, **extra)))
        return diag.read_manifest(path, TITLE)

    def codes(self, report):
        return {f['code'] for f in report['findings']}

    def test_test_file_is_unknown_never_mount_or_compatibility_proof(self):
        file = self.write('test.txt', b'hello')
        before = file.stat()
        report = diag.diagnose(self.root, TITLE, 'folder')
        self.assertEqual(report['entries'][0]['sha256'], hashlib.sha256(b'hello').hexdigest())
        self.assertEqual(report['totalBytes'], 5)
        self.assertEqual(report['compatibility'], 'unknown')
        self.assertFalse(report['activationAllowed'])
        self.assertFalse(report['hardwareVerified'])
        self.assertEqual(report['mountObservation'], 'not-performed')
        self.assertEqual(report['gameConsumption'], 'not-observed')
        self.assertEqual(file.stat().st_mtime_ns, before.st_mtime_ns)
        self.assertEqual(file.read_bytes(), b'hello')
        self.assertEqual(list(self.root.iterdir()), [file])

    def test_514_files_do_not_trigger_pkg_limit_for_folder_or_image(self):
        for i in range(514):
            self.write(f'data/{i}.bin')
        for source in ('folder', 'image'):
            report = diag.diagnose(self.root, TITLE, source)
            self.assertEqual(report['fileCount'], 514)
            self.assertEqual(report['compatibility'], 'unknown')
            self.assertFalse(report['pkgRedirects']['applicable'])

    def test_pkg_count_collapses_new_subtree_and_is_unknown_without_baseline(self):
        for i in range(514):
            self.write(f'new/{i}.bin')
        unknown = diag.diagnose(self.root, TITLE, 'pkg')
        self.assertIsNone(unknown['pkgRedirects']['exact'])
        baseline = self.baseline([])
        self.assertEqual(diag.diagnose(self.root, TITLE, 'pkg', baseline)['pkgRedirects']['exact'], 1)
        baseline = self.baseline([{'path': 'new', 'type': 'directory'}])
        report = diag.diagnose(self.root, TITLE, 'pkg', baseline)
        self.assertEqual(report['pkgRedirects']['exact'], 514)
        self.assertIn('pkg-redirect-limit', self.codes(report))

    def test_pc_markers_and_renamed_executables_block(self):
        for name, data in [('ModConfig.json', b'{}'), ('dsts-loader/data.bin', b'x'),
                           ('innocent.bin', b'MZxx'), ('native.bin', b'\x7fELF'),
                           ('fakelib/readme.txt', b'x')]:
            self.write(name, data)
        report = diag.diagnose(self.root, TITLE)
        self.assertTrue({'pc-loader', 'windows-code', 'native-code', 'reserved-path'} <= self.codes(report))
        self.assertEqual(report['compatibility'], 'blocked')

    def test_links_and_fifo_are_reported_without_reading_or_blocking(self):
        external = Path(self.temp.name) / 'external'
        external.mkdir()
        (external / 'secret').write_text('must not read')
        (self.root / 'link').symlink_to(external, target_is_directory=True)
        os.mkfifo(self.root / 'pipe')
        report = diag.diagnose(self.root, TITLE)
        self.assertFalse(report['scanComplete'])
        self.assertEqual(report['totalBytes'], 0)
        self.assertTrue(all('sha256' not in e for e in report['entries']))
        with self.assertRaises(OSError):
            diag.diagnose(self.root / 'link', TITLE)
        (external / 'nested').mkdir()
        with self.assertRaises(OSError):
            diag.diagnose(self.root / 'link' / 'nested', TITLE)

    def test_case_and_parent_conflicts_and_replacements(self):
        self.write('Data/a.bin')
        self.write('data/b.bin')
        self.write('file/child.bin')
        baseline = self.baseline([{'path': 'Data/a.bin', 'type': 'file'},
                                  {'path': 'file', 'type': 'file'}])
        report = diag.diagnose(self.root, TITLE, 'folder', baseline)
        self.assertTrue({'case-collision', 'replacement', 'parent-conflict', 'type-conflict'} <= self.codes(report))

    def test_partial_baseline_does_not_establish_absence(self):
        self.write('new.bin')
        report = diag.diagnose(self.root, TITLE, 'pkg', self.baseline([], complete=False))
        self.assertEqual(report['entries'][0]['gamePathCheck'], 'unknown')
        self.assertIsNone(report['pkgRedirects']['exact'])

    def test_malformed_baselines_are_rejected(self):
        for entries in ([{'path': '../escape', 'type': 'file'}],
                        [{'path': 'x', 'type': 'file'}, {'path': 'x/y', 'type': 'file'}],
                        [{'path': 'x', 'type': 'file'}, {'path': 'x', 'type': 'file'}],
                        [{'path': 'x', 'type': 'file', 'sha256': 'bad'}]):
            with self.assertRaises(ValueError):
                self.baseline(entries)
        with self.assertRaises(ValueError):
            self.baseline([], complete='true')

    def test_version_mismatch_and_path_checks_block(self):
        self.write('bad\\name')
        report = diag.diagnose(self.root, TITLE, 'folder', self.baseline([], gameVersion='01.000'), '02.000')
        self.assertIn('version-mismatch', self.codes(report))
        self.assertIn('unsafe-path', self.codes(report))
        self.assertTrue(diag.path_errors('a/../b'))
        self.assertTrue(diag.path_errors('/absolute'))
        self.assertTrue(diag.path_errors('a/' * 65 + 'file'))
        self.assertTrue(diag.path_errors('/'.join(['x' * 250] * 5)))

    def test_limits_and_read_errors_fail_closed_as_json(self):
        self.write('x')
        with patch.object(diag, 'MAX_ENTRIES', 0), contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(diag.main(['--overlay', str(self.root), '--title-id', TITLE]), 2)
        self.assertFalse(json.loads(out.getvalue())['scanComplete'])
        with patch.object(diag.os, 'scandir', side_effect=PermissionError('denied')):
            with self.assertRaises(PermissionError):
                diag.diagnose(self.root, TITLE)

    def test_cli_exit_codes_and_deterministic_report(self):
        self.write('test.txt')
        args = ['--overlay', str(self.root), '--title-id', TITLE]
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(diag.main(args), 0)
        with contextlib.redirect_stdout(io.StringIO()) as second:
            self.assertEqual(diag.main(args), 0)
        self.assertEqual(out.getvalue(), second.getvalue())
        self.write('ModConfig.json', b'{}')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(diag.main(args), 1)


if __name__ == '__main__':
    unittest.main()
