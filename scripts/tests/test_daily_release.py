import importlib.util
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('daily_version', ROOT / 'scripts/daily-version.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class DailyVersionTests(unittest.TestCase):
    def test_monotonic_mixed_history(self):
        self.assertEqual(module.next_version(['v6.1.0-vrr17', 'v6.1.0-vrr17.1', 'v6.1.0-vrr18']), '6.1.0-daily.181')
        self.assertEqual(module.next_version(['v6.1.0-daily.181', 'v6.1.0-vrr18.2']), '6.1.0-daily.183')
        self.assertEqual(module.next_version(['v6.1.0-daily.999']), '6.1.0-daily.1000')
    def test_boundary_and_migration_fail_closed(self):
        self.assertEqual(module.next_version(['v6.1.0-daily.65534']), '6.1.0-daily.65535')
        for tags in (['v6.1.0-daily.65535'], ['v6.3.0'], ['v6.1.0-vrr18-bad'], ['v6.1.0-daily.bad']):
            with self.subTest(tags=tags), self.assertRaises(ValueError):
                module.next_version(tags)
    @unittest.skipUnless(__import__('shutil').which('pwsh') or __import__('shutil').which('powershell'), 'PowerShell unavailable')
    def test_pe_mapping_preserves_vrr_and_enforces_daily_limits(self):
        import shutil
        shell = shutil.which('pwsh') or shutil.which('powershell')
        cases = {'6.1.0-vrr17': '6.2.17', '6.1.0-vrr17.1': '6.2.171', '6.1.0-vrr18': '6.2.180', '6.1.0-vrr18.2': '6.2.182', '6.1.0-daily.183': '6.2.183', '6.1.0-daily.65535': '6.2.65535'}
        for ci, expected in cases.items():
            result = subprocess.run([shell, '-NoProfile', '-File', str(ROOT / 'scripts/vrr-pe-version.ps1'), '-CiVersion', ci], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), expected)
        for ci in ('6.1.0-daily.65536', '6.1.0-daily.0', 'not-a-version'):
            result = subprocess.run([shell, '-NoProfile', '-File', str(ROOT / 'scripts/vrr-pe-version.ps1'), '-CiVersion', ci], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)

if __name__ == '__main__':
    unittest.main()
