from pathlib import Path
import subprocess
import tempfile
import unittest

from change_impact_benchmark import prepare_case, score, validate_case, validate_response, sha256


class ChangeImpactBenchmarkTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[1] / '.agents' / 'scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / 'history.git'
        subprocess.run(['git', 'init', '--bare', '-q', str(self.repo)], check=True)
        self.base = self.commit({'api.py': 'def old(): return 1\n',
                                 'caller.py': 'from api import old\nold()\n',
                                 'unrelated.py': 'answer = 42\n'})
        self.target = self.commit({'api.py': 'def new(): return 1\n',
                                   'caller.py': 'from api import new\nnew()\n',
                                   'unrelated.py': 'answer = 42\n'}, self.base)
        self.case = {'id': 'rename', 'base': self.base, 'target': self.target,
                     'seed_files': ['api.py'], 'symbols': [{'path': 'api.py', 'name': 'old'}],
                     'required': [{'path': 'caller.py', 'revision': 'base', 'line': 2,
                                   'quote': 'old()', 'reason': 'Caller must migrate.'}],
                     'negative': [{'path': 'unrelated.py', 'revision': 'base', 'line': 1,
                                   'quote': 'answer = 42', 'reason': 'Independent constant.'}]}

    def git(self, *args, data=None):
        return subprocess.run(['git', '-C', str(self.repo), '-c', 'user.name=Benchmark test',
                               '-c', 'user.email=benchmark@example.invalid', *args],
                              input=data, capture_output=True, text=True, check=True).stdout.strip()

    def commit(self, files, parent=None):
        entries = []
        for name, source in sorted(files.items()):
            blob = self.git('hash-object', '-w', '--stdin', data=source)
            entries.append(f'100644 blob {blob}\t{name}\n')
        tree = self.git('mktree', data=''.join(entries))
        return self.git('commit-tree', tree, *(['-p', parent] if parent else []), data='fixture\n')

    def test_only_initiating_edit_reaches_candidate(self):
        validate_case(self.repo, self.case)
        base, candidate, patch = prepare_case(self.repo, self.case, self.root / 'run')
        self.assertEqual((base / 'api.py').read_text(), 'def old(): return 1\n')
        self.assertEqual((candidate / 'api.py').read_text(), 'def new(): return 1\n')
        self.assertEqual((candidate / 'caller.py').read_text(), 'from api import old\nold()\n')
        self.assertNotIn('caller.py', patch.read_text())

    def test_wrong_gold_witness_is_rejected_before_execution(self):
        self.case['required'][0]['quote'] = 'new()'
        with self.assertRaisesRegex(ValueError, 'witness'):
            validate_case(self.repo, self.case)

    def test_archive_filters_cannot_silently_remove_consumers(self):
        self.case['base'] = self.commit({'api.py': 'def old(): return 1\n',
                                         'caller.py': 'from api import old\nold()\n',
                                         '.gitattributes': 'caller.py export-ignore\n'})
        with self.assertRaisesRegex(ValueError, 'archive'):
            prepare_case(self.repo, self.case, self.root / 'filtered')

    def test_parent_mismatch_is_rejected(self):
        self.case['base'], self.case['target'] = self.target, self.base
        with self.assertRaisesRegex(ValueError, 'parent'):
            validate_case(self.repo, self.case)

    def test_seed_path_escape_is_rejected(self):
        self.case['seed_files'] = ['../api.py']
        with self.assertRaisesRegex(ValueError, 'path'):
            validate_case(self.repo, self.case)

    def test_absent_symbol_is_rejected_before_comparison(self):
        self.case['symbols'][0]['name'] = 'old_typo'
        with self.assertRaisesRegex(ValueError, 'symbol'):
            validate_case(self.repo, self.case)

    def test_advisory_must_bind_exact_input_bytes_and_roots(self):
        base, candidate, patch = prepare_case(self.repo, self.case, self.root / 'bindings')
        response = {'schema_version': 1, 'advisory': True, 'scope': 'supplied_diff',
                    'base': {'root': str(base)}, 'target': {'root': str(candidate)},
                    'diff_sha256': sha256(patch), 'impacts': [],
                    'changes': [{'old_path': 'api.py', 'new_path': 'api.py',
                                 'old_sha256': sha256(base / 'api.py'),
                                 'new_sha256': sha256(candidate / 'api.py')}]}
        validate_response(response, self.case, base, candidate, patch)
        response['diff_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'diff'):
            validate_response(response, self.case, base, candidate, patch)
        response['diff_sha256'] = sha256(patch)
        response['target']['root'] = str(base)
        with self.assertRaisesRegex(ValueError, 'root'):
            validate_response(response, self.case, base, candidate, patch)
        response['target']['root'] = str(candidate)
        response['changes'][0]['new_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'source'):
            validate_response(response, self.case, base, candidate, patch)

    def test_delete_is_applied_without_copying_consumer_repair(self):
        self.case['target'] = self.commit({'caller.py': 'print(1)\n',
                                           'unrelated.py': 'answer = 42\n'}, self.base)
        validate_case(self.repo, self.case)
        _, candidate, patch = prepare_case(self.repo, self.case, self.root / 'delete')
        self.assertFalse((candidate / 'api.py').exists())
        self.assertIn('old()', (candidate / 'caller.py').read_text())
        self.assertIn('/dev/null', patch.read_text())

    def test_missing_negative_and_unjudged_are_distinct(self):
        result = score(['unrelated.py', 'other.py'], self.case)
        self.assertEqual(result['required_recall'], 0)
        self.assertEqual(result['missing_required'], ['caller.py'])
        self.assertEqual(result['known_negative_hits'], ['unrelated.py'])
        self.assertEqual(result['unjudged'], ['other.py'])
        self.assertFalse(result['passed'])
        self.assertTrue(score(['caller.py', 'other.py'], self.case)['passed'])

    def test_tool_failure_cannot_pass_even_with_correct_paths(self):
        self.assertFalse(score(['caller.py'], self.case, error='tool timed out')['passed'])


if __name__ == '__main__':
    unittest.main()
