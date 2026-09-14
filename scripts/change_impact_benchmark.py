#!/usr/bin/env python3
"""Replay initiating edits from pinned history; measure downstream-file retrieval."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import statistics
import subprocess
import tarfile
import time

from bootstrap_eval import relpath_from_root


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], capture_output=True,
                          check=True).stdout


def safe_path(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or '..' in path.parts or '\\' in value or value.startswith('-'):
        raise ValueError('Invalid repository path: ' + value)
    return value


def validate_case(repo, case):
    for key in ('base', 'target'):
        if not re.fullmatch('[0-9a-f]{40}', case[key]):
            raise ValueError('Full commit SHA required: ' + key)
    try:
        parent = git(repo, 'rev-parse', case['target'] + '^').decode().strip()
    except subprocess.CalledProcessError as exc:
        raise ValueError('Target has no parent') from exc
    if parent != case['base']:
        raise ValueError('Base must be the target parent')
    seeds = set(map(safe_path, case['seed_files']))
    if not seeds:
        raise ValueError('At least one initiating file required')
    changed = set(git(repo, 'diff', '--name-only', '--no-renames', case['base'],
                      case['target']).decode().splitlines())
    if not seeds <= changed:
        raise ValueError('Initiating files must change in the pinned commit')
    required = {safe_path(item['path']) for item in case['required']}
    negatives = {safe_path(item['path']) for item in case['negative']}
    if not required or required & seeds or required & negatives or negatives & seeds:
        raise ValueError('Required, negative and initiating paths must be disjoint')
    if any(not git(repo, 'ls-tree', case['base'], '--', path) for path in required | negatives):
        raise ValueError('Judged downstream files must exist in the base input')
    if not case['symbols']:
        raise ValueError('At least one source symbol required')
    for symbol in case['symbols']:
        if safe_path(symbol['path']) not in seeds or not symbol['name']:
            raise ValueError('Symbols must belong to initiating files')
        sources = [git(repo, 'show', case[side] + ':' + symbol['path']).decode()
                   for side in ('base', 'target')
                   if git(repo, 'ls-tree', case[side], '--', symbol['path'])]
        if not any(re.search(r'\b' + re.escape(symbol['name']) + r'\b', source) for source in sources):
            raise ValueError('Input symbol absent from pinned source: ' + symbol['name'])
    for item in case['required'] + case['negative']:
        if item['revision'] not in ('base', 'target') or item['line'] < 1 or not item['reason']:
            raise ValueError('Invalid source witness')
        lines = git(repo, 'show', case[item['revision']] + ':' + item['path']).decode().splitlines()
        index = item['line'] - 1
        if index >= len(lines) or lines[index].strip() != item['quote'].strip():
            raise ValueError('Source witness mismatch: ' + item['path'])


def prepare_case(repo, case, directory):
    directory.mkdir(parents=True, exist_ok=False)
    base, target = directory / 'base', directory / 'candidate'
    base.mkdir()
    with tarfile.open(fileobj=io.BytesIO(git(repo, 'archive', case['base']))) as archive:
        archive.extractall(base, filter='data')
    for entry in filter(None, git(repo, 'ls-tree', '-r', '-z', case['base']).split(b'\0')):
        metadata, name = entry.split(b'\t', 1)
        mode, kind, oid = metadata.split()
        path = base / name.decode()
        if kind != b'blob' or not (path.exists() or path.is_symlink()):
            raise ValueError('Incomplete git archive: ' + name.decode())
        data = os.readlink(path).encode() if mode == b'120000' else path.read_bytes()
        if hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest() != oid.decode():
            raise ValueError('Filtered git archive bytes: ' + name.decode())
    shutil.copytree(base, target, symlinks=True)
    for relative in case['seed_files']:
        destination = target / safe_path(relative)
        if destination.is_symlink() or not destination.resolve().is_relative_to(target.resolve()):
            raise ValueError('Initiating path follows a symlink: ' + relative)
        tree = git(repo, 'ls-tree', case['target'], '--', relative).decode().strip()
        if tree:
            if not tree.startswith(('100644 blob ', '100755 blob ')):
                raise ValueError('Initiating edit must be a regular source file')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(git(repo, 'show', case['target'] + ':' + relative))
        else:
            destination.unlink()
    patch = directory / 'initiating.diff'
    patch.write_bytes(git(repo, 'diff', '--no-ext-diff', '--no-renames', '--no-color',
                         case['base'], case['target'], '--', *case['seed_files']))
    return base, target, patch


def score(paths, case, error=None):
    returned = set(paths) - set(case['seed_files'])
    required = {item['path'] for item in case['required']}
    negatives = {item['path'] for item in case['negative']}
    missing = required - returned
    false_hits = negatives & returned
    return {'required_recall': len(required & returned) / len(required),
            'missing_required': sorted(missing), 'known_negative_hits': sorted(false_hits),
            'unjudged': sorted(returned - required - negatives),
            'returned_files': sorted(returned),
            'passed': not error and not missing and not false_hits, 'error': error}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_digest(root):
    entries = []
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            entries.append((path.relative_to(root).as_posix(), 'link', os.readlink(path)))
        elif path.is_file():
            entries.append((path.relative_to(root).as_posix(), 'file', sha256(path)))
    return hashlib.sha256(json.dumps(entries).encode()).hexdigest()


def validate_response(result, case, base, target, patch):
    if result.get('schema_version') != 1 or result.get('advisory') is not True or result.get('scope') != 'supplied_diff':
        raise ValueError('Missing change-context operation contract')
    if result.get('diff_sha256') != sha256(patch):
        raise ValueError('Change-context diff binding mismatch')
    if result['base']['root'] != str(base.resolve()) or result['target']['root'] != str(target.resolve()):
        raise ValueError('Change-context root binding mismatch')
    observed = set()
    for change in result['changes']:
        for path_key, hash_key, root in (('old_path', 'old_sha256', base), ('new_path', 'new_sha256', target)):
            relative = change[path_key]
            if relative:
                if relative not in case['seed_files'] or change[hash_key] != sha256(root / safe_path(relative)):
                    raise ValueError('Change-context source binding mismatch')
                observed.add(relative)
    if observed != set(case['seed_files']):
        raise ValueError('Change-context change inventory mismatch')


class Commands:
    def __init__(self, directory, timeout):
        self.directory = directory
        directory.mkdir()
        self.timeout = timeout
        self.records = []

    def run(self, command, cwd=None, accepted=(0,)):
        started = time.perf_counter()
        prefix = self.directory / str(len(self.records))
        error = None
        try:
            proc = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True,
                                  timeout=self.timeout)
            code, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as exc:
            code, stdout, stderr = None, exc.stdout or b'', exc.stderr or b''
            error = 'command timeout'
        prefix.with_suffix('.stdout').write_bytes(stdout)
        prefix.with_suffix('.stderr').write_bytes(stderr)
        self.records.append({'command': list(map(str, command)), 'cwd': str(cwd) if cwd else None,
                             'returncode': code, 'elapsed_seconds': time.perf_counter() - started,
                             'stdout_bytes': len(stdout), 'stdout': str(prefix.with_suffix('.stdout')),
                             'stderr': str(prefix.with_suffix('.stderr'))})
        (self.directory / 'commands.json').write_text(json.dumps(self.records, indent=2) + '\n')
        if code not in accepted:
            raise RuntimeError(error or 'command failed: ' + stderr.decode(errors='replace')[-500:])
        return stdout


def operation(commands, executable, graph, name, arguments):
    envelope = json.loads(commands.run([executable, 'seam', 'query', '--graph', graph,
                                        name, json.dumps(arguments)]))
    if not envelope.get('ok'):
        raise RuntimeError('Graph operation failed: ' + str(envelope))
    return envelope['result']


def retrieve(arm, case, base, target, patch, commands, executables, depth, budget):
    paths = set()
    details = {}
    if arm == 'literal_search':
        for root in (base, target):
            command = [executables['rg'], '--files-with-matches', '--null', '--word-regexp',
                       '--fixed-strings', '--hidden', '--glob', '!**/.git/**']
            for name in sorted({symbol['name'] for symbol in case['symbols']}):
                command.extend(['-e', name])
            output = commands.run([*command, '.'], cwd=root, accepted=(0, 1))
            paths.update(value.removeprefix('./') for value in output.decode().split('\0') if value)
    elif arm == 'cgraph_primitives':
        details['snapshots'] = []
        for side, root in (('base', base), ('candidate', target)):
            export = commands.directory / (side + '-graph')
            commands.run([executables['cgraph'], '--root', root, '--out', export])
            graph = export / 'graph.json'
            node_ids = set()
            for symbol in case['symbols']:
                found = operation(commands, executables['cgraph'], graph, 'query',
                                  {'q': symbol['name'], 'file': symbol['path'], 'limit': 0})
                node_ids.update(node['id'] for node in found['nodes']
                                if node['label'] == symbol['name'] and
                                relpath_from_root(str(root), node.get('source_file', '')) == symbol['path'])
            details['snapshots'].append({'side': side, 'resolved_seeds': sorted(node_ids)})
            for node_id in sorted(node_ids):
                result = operation(commands, executables['cgraph'], graph, 'impact',
                                   {'id': node_id, 'direction': 'dependents', 'max_depth': depth, 'limit': 0})
                for node in result['nodes']:
                    relative = relpath_from_root(str(root), node.get('source_file', ''))
                    if relative:
                        paths.add(relative)
    elif arm == 'change_context':
        result = json.loads(commands.run([executables['change_context'], 'change-context',
                                          '--base-root', base, '--target-root', target,
                                          '--diff', patch, '--budget', str(budget),
                                          '--max-depth', str(depth)]))
        validate_response(result, case, base, target, patch)
        paths.update(item['path'] for item in result['impacts'] if item.get('path'))
        details = {key: result[key] for key in ('base', 'target', 'uncertainty', 'truncated',
                                               'omitted', 'tokens_used', 'budget', 'advisory',
                                               'scope', 'diff_sha256', 'changes')}
    else:
        raise ValueError('Unknown arm: ' + arm)
    return sorted(paths), details


def summarize(rows):
    summary = {}
    for arm in sorted({row['arm'] for row in rows}):
        selected = [row for row in rows if row['arm'] == arm]
        successful_commands = [row for row in selected if not row['error']]
        summary[arm] = {'runs': len(selected), 'passed': sum(row['passed'] for row in selected),
                        'tool_failures': sum(bool(row['error']) for row in selected),
                        'mean_required_recall': statistics.mean(row['required_recall'] for row in selected),
                        'known_negative_hits': sum(len(row['known_negative_hits']) for row in selected),
                        'median_unjudged_files': statistics.median(len(row['unjudged']) for row in selected),
                        'median_end_to_end_seconds': statistics.median(row['elapsed_seconds'] for row in successful_commands)
                        if successful_commands else None}
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases', required=True, type=Path)
    parser.add_argument('--cgraph', required=True, type=Path, help='Installed baseline CLI')
    parser.add_argument('--change-context', required=True, type=Path, help='Installed PR72 CLI')
    parser.add_argument('--out', required=True, type=Path, help='New directory for this run only')
    parser.add_argument('--repos', required=True, type=Path, help='Clone cache; one bare repo per corpus repository id')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--max-depth', type=int, default=6)
    parser.add_argument('--budget', type=int, default=24000, help='Change-context response budget; other arms are uncapped')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if args.repeats < 1 or not 0 <= args.max_depth <= 20 or args.budget < 1 or args.timeout < 1:
        parser.error('Invalid repeat, depth, budget or timeout')
    executables = {'cgraph': str(args.cgraph.resolve()), 'change_context': str(args.change_context.resolve()),
                   'rg': shutil.which('rg')}
    if not all(value and Path(value).is_file() for value in executables.values()):
        parser.error('All installed executables, including rg, must exist')
    corpus = json.loads(args.cases.read_text())
    if not corpus['cases'] or len({case['id'] for case in corpus['cases']}) != len(corpus['cases']):
        parser.error('Cases must be nonempty with unique ids')
    args.repos = args.repos.resolve()
    args.repos.mkdir(parents=True, exist_ok=True)
    repositories = {}
    for name, metadata in corpus['repositories'].items():
        if not re.fullmatch(r'[a-z0-9_-]+', name):
            parser.error('Invalid repository id')
        repo = args.repos / (name + '.git')
        if not repo.exists():
            subprocess.run(['git', 'clone', '--bare', metadata['url'], str(repo)], check=True)
        if git(repo, 'remote', 'get-url', 'origin').decode().strip() != metadata['url']:
            raise ValueError('Cached repository origin mismatch: ' + name)
        repositories[name] = repo
    for case in corpus['cases']:
        if not re.fullmatch(r'[a-z0-9_-]+', case['id']):
            parser.error('Invalid case id')
        validate_case(repositories[case['repository']], case)
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = {'schema_version': 1, 'platform': platform.platform(), 'cases_sha256': sha256(args.cases),
                'runner_sha256': sha256(__file__), 'executables': executables,
                'executable_sha256': {key: sha256(path) for key, path in executables.items()},
                'repeats': args.repeats, 'max_depth': args.max_depth, 'change_context_budget': args.budget,
                'corpus': corpus, 'limitations': [
                    'Deterministic dependency retrieval, not agent task completion or a Graphify comparison.',
                    'Six historical Python changes in three repositories; no cross-language superiority claim.',
                    'Required paths are independently reviewed positive examples, not an exhaustive dependency set.',
                    'Only explicit negative labels are errors; other returned files are unjudged workload.',
                    'Literal word search is a reproducible reference, not an adaptive search agent.',
                    'Both graph arms use base and provider-only candidate sources; repaired consumers stay hidden.',
                    'Existing primitives query cold disk exports; this is not warm resident-daemon latency.',
                    'Only change-context has a response token budget; report truncation and raw bytes separately.',
                    'Repeated deterministic runs measure timing variability, not independent quality samples.'
                ]}
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    rows = []
    arms = ['literal_search', 'cgraph_primitives', 'change_context']
    for case in corpus['cases']:
        directory = args.out / case['id']
        base, target, patch = prepare_case(repositories[case['repository']], case, directory)
        input_digests = [source_digest(root) for root in (base, target)]
        for repeat in range(args.repeats):
            for arm in arms[repeat % len(arms):] + arms[:repeat % len(arms)]:
                commands = Commands(directory / f'{arm}-{repeat}', args.timeout)
                started = time.perf_counter()
                paths, details, error = [], {}, None
                try:
                    paths, details = retrieve(arm, case, base, target, patch, commands,
                                             executables, args.max_depth, args.budget)
                except (RuntimeError, ValueError, KeyError, OSError) as exc:
                    error = str(exc)
                elapsed = time.perf_counter() - started
                if input_digests != [source_digest(root) for root in (base, target)]:
                    raise RuntimeError('Benchmark input changed during retrieval')
                row = {'case': case['id'], 'arm': arm, 'repeat': repeat,
                       'elapsed_seconds': elapsed, 'source_digests': input_digests,
                       'stdout_bytes': sum(record['stdout_bytes'] for record in commands.records),
                       'details': details, **score(paths, case, error)}
                rows.append(row)
                (commands.directory / 'result.json').write_text(json.dumps(row, indent=2) + '\n')
                print(json.dumps({key: row[key] for key in ('case', 'arm', 'repeat', 'required_recall', 'error')}), flush=True)
    if any(sha256(path) != manifest['executable_sha256'][key] for key, path in executables.items()):
        raise RuntimeError('Executable changed during run')
    result = {'schema_version': 1, 'manifest': manifest, 'summary': summarize(rows), 'rows': rows}
    (args.out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    (args.out / 'COMPLETE').write_text(sha256(args.out / 'results.json') + '\n')
    print(json.dumps(result['summary'], indent=2))
    return 1 if any(row['error'] for row in rows) else 0


if __name__ == '__main__':
    raise SystemExit(main())
