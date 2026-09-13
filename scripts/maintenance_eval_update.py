#!/usr/bin/env python3
"""Measure a real source edit through refreshed retrieval, without model calls."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import time

import maintenance_eval as evaluation
from maintenance_eval_gateway import StdioClient


SYMBOL = 'maintenance_update_probe'


def query(client, arm):
    name, arguments = (
        ('graph_query', {'query': SYMBOL}) if arm == 'cgraph'
        else ('query_graph', {'question': SYMBOL, 'token_budget': 2000})
    )
    response = client.call('tools/call', {'name': name, 'arguments': arguments})
    if response.get('isError'):
        raise RuntimeError('Refresh verification query failed: ' + json.dumps(response))
    return response


def run_probe(config, arm, output):
    output.mkdir()
    workspace = output / 'workspace'
    shutil.copytree(evaluation.FIXTURES / 'payments', workspace)
    cold = evaluation.index(config, arm, workspace, output / 'cold-index')
    if cold['returncode']:
        raise RuntimeError('Probe indexing failed; see ' + cold['stderr_path'])
    command, _ = evaluation.graph_config(config, arm, workspace)
    client = StdioClient(command, workspace, output / 'mcp.stderr')
    result = None
    try:
        readiness_started = time.perf_counter()
        readiness_attempts = []
        while True:
            before = query(client, arm)
            readiness_attempts.append({'elapsed_seconds': time.perf_counter() - readiness_started,
                                       'response': before})
            if arm == 'graphify' or json.loads(before['content'][0]['text']).get('freshness', {}).get('verified') is True:
                break
            if time.perf_counter() - readiness_started >= 30:
                raise RuntimeError('Initial cgraph snapshot did not become verified before the source edit')
            time.sleep(0.1)
        readiness_seconds = time.perf_counter() - readiness_started
        if arm == 'cgraph':
            before_nodes = json.loads(before['content'][0]['text'])['nodes']
            already_present = any(node['label'] == SYMBOL for node in before_nodes)
        else:
            before_nodes = json.loads((workspace / 'graphify-out/graph.json').read_text())['nodes']
            already_present = any(node['label'] == SYMBOL + '()' for node in before_nodes)
        if already_present:
            raise RuntimeError('Probe symbol already exists before the measured source edit')
        source = workspace / 'pricing.py'
        old_hash = evaluation.file_hash(source)
        started = time.perf_counter()
        source.write_text(source.read_text() + '\n\ndef ' + SYMBOL + '():\n    return 731\n')
        refresh_started = time.perf_counter()
        if arm == 'cgraph':
            refresh = client.call('tools/call', {'name': 'graph_update',
                                              'arguments': {'path': str(workspace)}})
            if refresh.get('isError'):
                raise RuntimeError('Probe refresh failed: ' + json.dumps(refresh))
        else:
            refresh = evaluation.update_graph(config, arm, workspace, output / 'update')
            if refresh['returncode']:
                raise RuntimeError('Probe refresh failed; see ' + refresh['stderr_path'])
        refresh_seconds = time.perf_counter() - refresh_started
        after = query(client, arm)
        edit_to_retrieval_seconds = time.perf_counter() - started
        if arm == 'cgraph':
            payload = json.loads(after['content'][0]['text'])
            if payload.get('ok') is False:
                raise RuntimeError('cgraph query returned an operation error')
            verified = any(node['label'] == SYMBOL for node in payload['nodes'])
            initial_freshness = json.loads(before['content'][0]['text'])['freshness']
            verified = verified and payload['freshness']['verified'] is True and (
                payload['freshness']['content_root'] != initial_freshness['content_root'])
            evidence = payload
        else:
            graph = json.loads((workspace / 'graphify-out/graph.json').read_text())
            nodes = graph['nodes']
            label = SYMBOL + '()'
            evidence = [node for node in nodes if node['label'] == label
                        and node['source_file'] == 'pricing.py' and node.get('_callable') is True]
            # Verify the resident MCP response also exposes the new symbol.
            verified = any(
                line.startswith('NODE ' + label + ' [src=pricing.py loc=' + node['source_location'] + ' ')
                for node in evidence
                for block in after['content'] if block['type'] == 'text'
                for line in block['text'].splitlines()
            )
        if not verified:
            raise RuntimeError('Updated symbol was absent from refreshed graph evidence')
        result = {
            'arm': arm, 'fixture': 'payments', 'symbol_added': SYMBOL,
            'source_before_sha256': old_hash, 'source_after_sha256': evaluation.file_hash(source),
            'cold_index': cold, 'before_query': before, 'refresh': refresh,
            'initial_snapshot_readiness_seconds': readiness_seconds,
            'initial_snapshot_readiness_attempts': readiness_attempts,
            'refresh_call_seconds': refresh_seconds,
            'edit_to_verified_retrieval_seconds': edit_to_retrieval_seconds,
            'after_query': after, 'symbol_evidence': evidence, 'verified': verified,
            'model_tokens': 0, 'money_usd': 0,
            'measurement_note': 'One deterministic source edit per backend. Edit-to-retrieval includes '
                                'write, refresh and query, including any resident watcher work. '
                                'This is a microprobe, not a repeated latency distribution.',
        }
    finally:
        try:
            if arm == 'cgraph':
                shutdown = client.call('tools/call', {'name': 'graph_shutdown', 'arguments': {}})
                if shutdown.get('isError'):
                    raise RuntimeError('Probe daemon shutdown failed: ' + json.dumps(shutdown))
                if result is not None:
                    result['shutdown'] = shutdown
        finally:
            client.close()
    (output / 'result.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    output = Path(args.out).resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = {'config': config, 'source_sha256': evaluation.file_hash(__file__),
                'harness_sha256': evaluation.file_hash(evaluation.__file__),
                'arms': ['cgraph', 'graphify'], 'model_calls': 0}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    results = [run_probe(config, arm, output / arm) for arm in manifest['arms']]
    (output / 'results.json').write_text(json.dumps(results, indent=2))
    print(json.dumps([{'arm': r['arm'], 'verified': r['verified'],
                       'edit_to_verified_retrieval_seconds': r['edit_to_verified_retrieval_seconds']}
                      for r in results]))


if __name__ == '__main__':
    main()
