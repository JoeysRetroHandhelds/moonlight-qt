"""Cross-check API identity, CI evidence and exact staged public downloads."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class PublicContractTest(unittest.TestCase):
    def test_identity_and_public_evidence(self):
        n = load('nightly_notes')
        host = (SCRIPTS / 'daily_release.py').exists()
        product = 'Vibepollo' if 'apollo-' in str(SCRIPTS) else 'Vibeshine'
        repo = 'Nonary/' + (product if product == 'Vibepollo' else 'vibeshine') if host else 'Nonary/moonlight-qt'
        tag = '2.0.1-daily.d20261012.1' if host else 'v6.2.0-daily.183'
        source = 'a' * 40
        version = tag + '+' + source[:12] if host else tag[1:]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'assets'
            root.mkdir()
            notes = Path(temp) / 'notes.md'
            if host:
                (root / (product + 'Setup.exe')).write_bytes(b'installer')
                (root / (product.lower() + '-fixture-x86_64.pkg.tar.zst')).write_bytes(b'linux')
                (root / 'daily-manifest.json').write_text(json.dumps({'source_commit': source}))
                (root / 'release-provenance.json').write_text('{}')
            else:
                (root / ('MoonlightSetup-' + version + '.exe')).write_bytes(b'installer')
            def fake_run(*args):
                return '[[]]' if args[0] == 'gh' else source
            with patch.object(n, 'run', fake_run):
                n.generate(repo, tag, version, source, root, notes)
            metadata = json.loads((root / 'nightly-metadata.json').read_text())
            line = notes.read_text().strip().splitlines()[0]
            self.assertEqual(json.loads(line.removeprefix('<!-- nightly-identity: ').removesuffix(' -->')), metadata)
            self.assertEqual(metadata['source_commit'], source)
            self.assertIn('nightly-metadata.json', (root / 'SHA256SUMS').read_text())
            if host:
                self.assertNotIn('daily-manifest.json', (root / 'SHA256SUMS').read_text())
                self.assertNotIn('release-provenance.json', (root / 'SHA256SUMS').read_text())
                self.assertEqual(metadata['qualification']['source_commit'], source)
                d = load('daily_release')
                destination = Path(temp) / 'public'
                d.stage_public_assets(root, destination, product)
                self.assertEqual({p.name for p in destination.iterdir()}, {product + 'Setup.exe', product.lower() + '-fixture-x86_64.pkg.tar.zst', 'nightly-metadata.json', 'SHA256SUMS'})
                self.assertTrue((root / 'release-provenance.json').exists())
                (root / 'garbage.txt').write_text('garbage')
                with self.assertRaises(ValueError):
                    d.stage_public_assets(root, Path(temp) / 'bad-public', product)

if __name__ == '__main__':
    unittest.main()
