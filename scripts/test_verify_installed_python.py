"""Failure-oriented checks for installed-wheel validation (no native SDK required)."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location('installed_verifier', Path(__file__).with_name('verify-installed-python.py'))
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


class InstalledVerifierTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.source = self.root/'source'
        self.site = self.root/'site'
        self.prefix = self.root/'install'
        self.source.mkdir(); self.site.mkdir(); self.prefix.mkdir()
        self.members = {'rfmodel/__init__.py': 'value=1\n', 'rfmodel/feature.py': 'value=2\n',
                        'rfmodel-0.1.0.dist-info/METADATA': 'Name: rfmodel\nVersion: 0.1.0\n'}
        self.wheel = self.root/'rfmodel-0.1.0-py3-none-any.whl'
        with zipfile.ZipFile(self.wheel,'w') as archive:
            for name,text in self.members.items():
                archive.writestr(name,text)
                installed = self.site/name; installed.parent.mkdir(parents=True,exist_ok=True)
                installed.write_bytes(text.encode('utf-8'))
                if name.endswith('.py'):
                    original = self.source/'python'/name; original.parent.mkdir(parents=True,exist_ok=True)
                    original.write_bytes(text.encode('utf-8'))

    def validate(self):
        return verifier.validate_package(self.wheel,self.site,self.source)

    def test_installed_files_must_match_both_wheel_and_source(self):
        result = self.validate()
        self.assertEqual(len(result),2)
        self.assertEqual({r['sha256'] for r in result},{r['source_sha256'] for r in result})
        (self.site/'rfmodel/feature.py').write_text('value=99\n')
        with self.assertRaisesRegex(ValueError,'differs from wheel'): self.validate()

    def test_missing_and_stale_extra_modules_are_rejected(self):
        (self.site/'rfmodel/feature.py').unlink()
        with self.assertRaisesRegex(ValueError,'inventory'): self.validate()
        (self.site/'rfmodel/feature.py').write_bytes(self.members['rfmodel/feature.py'].encode('utf-8'))
        (self.site/'rfmodel/stale.py').write_text('')
        with self.assertRaisesRegex(ValueError,'inventory'): self.validate()

    def test_wheel_from_different_checkout_is_rejected(self):
        (self.source/'python/rfmodel/feature.py').write_text('value=3\n')
        with self.assertRaisesRegex(ValueError,'current source'): self.validate()

    def test_checkout_cannot_be_used_as_installation(self):
        with self.assertRaisesRegex(ValueError,'checkout Python'):
            verifier.validate_package(self.wheel,self.source/'python',self.source)

    def test_wrong_configuration_missing_and_ambiguous_native_libraries(self):
        name = self.prefix/'lib/librfmodel_c.so'; name.parent.mkdir(); name.write_bytes(b'release')
        self.assertEqual(verifier.installed_library(self.prefix,'Release','Linux'),name.resolve())
        with self.assertRaisesRegex(ValueError,'found 0'):
            verifier.installed_library(self.prefix,'Debug','Linux')
        other = self.prefix/'lib64/librfmodel_c.so';other.parent.mkdir();other.write_bytes(b'duplicate')
        with self.assertRaisesRegex(ValueError,'found 2'):
            verifier.installed_library(self.prefix,'Release','Linux')

    def test_native_library_names_cover_windows_and_macos(self):
        for system,name in [('Windows','rfmodel_cd.dll'),('Darwin','librfmodel_cd.dylib')]:
            path=self.prefix/name;path.write_bytes(b'debug')
            self.assertEqual(verifier.installed_library(self.prefix,'Debug',system),path.resolve())

    def test_new_binding_suite_cannot_silently_skip_installed_mode(self):
        tests=self.source/'tests';tests.mkdir();(tests/'test_example.py').write_text('print(1)')
        with self.assertRaisesRegex(ValueError,'installed mode'):verifier.discover_suites(self.source)
        (tests/'test_example.py').write_text('# --installed\n')
        self.assertEqual([p.name for p in verifier.discover_suites(self.source)],['test_example.py'])

    def test_cli_guard_detects_source_leak_and_extra_paths(self):
        command=['python','-m','rfmodel','input.json']
        self.assertTrue(verifier.check_cli_environment(command,{'PYTHONPATH':str(self.site)},self.site))
        for value in ['',str(self.source/'python'),str(self.site)+os.pathsep+str(self.source/'python')]:
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'installed package'):
                verifier.check_cli_environment(command,{'PYTHONPATH':value},self.site)
        self.assertFalse(verifier.check_cli_environment(['other-tool'],{},self.site))

    def test_inherited_python_settings_are_removed(self):
        from unittest.mock import patch
        with patch.dict(os.environ,{'PYTHONPATH':'source','PYTHONHOME':'wrong','PYTHONSTARTUP':'startup'}):
            env=verifier.clean_environment(self.site)
        self.assertEqual(env['PYTHONPATH'],str(self.site))
        self.assertNotIn('PYTHONHOME',env);self.assertNotIn('PYTHONSTARTUP',env)
        self.assertEqual(env['PYTHONNOUSERSITE'],'1')

    def test_preflight_failure_is_not_reported_as_success(self):
        work=self.root/'failed-run'
        with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            status=verifier.verify(['--wheel',str(self.wheel),'--source-root',str(self.source),
                   '--install-prefix',str(self.prefix),'--configuration','Release','--work-dir',str(work)])
        report=json.loads((work/'installed-python-validation.json').read_text(encoding='utf-8'))
        self.assertEqual(status,1);self.assertFalse(report['passed']);self.assertEqual(report['total_tests'],0)
        self.assertIn('Expected exactly one installed',report['error'])
        before=(work/'installed-python-validation.json').read_bytes()
        with self.assertRaises(FileExistsError):
            verifier.verify(['--wheel',str(self.wheel),'--install-prefix',str(self.prefix),
                             '--configuration','Release','--work-dir',str(work)])
        self.assertEqual(before,(work/'installed-python-validation.json').read_bytes())


if __name__=='__main__':
    unittest.main()
