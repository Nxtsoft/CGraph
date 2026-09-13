#!/usr/bin/env python3
"""Restricted fixture tools and a real, persistent stdio MCP graph connection."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

MAX_TEXT = 24000


def contained(root: Path, value: str) -> Path:
    path = (root / value).resolve()
    if not path.is_relative_to(root.resolve()) or path == root.resolve():
        raise ValueError('Path must name a file inside the task workspace')
    if any(part.startswith('.') or part in ['cgraph-out','graphify-out','__pycache__'] for part in path.relative_to(root.resolve()).parts):
        raise ValueError('Hidden paths are excluded')
    return path


class StdioClient:
    def __init__(self, command: list[str], cwd: Path, stderr_path: Path):
        self.command = command
        self.stderr = stderr_path.open('w')
        self.process = subprocess.Popen(command, cwd=cwd, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr, text=True, bufsize=1)
        self.messages = queue.Queue()
        self.sequence = 0
        threading.Thread(target=self._read, daemon=True).start()
        self.call('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                                'clientInfo': {'name': 'maintenance-eval', 'version': '1'}})
        self.notify('notifications/initialized', {})

    def _read(self):
        try:
            for line in self.process.stdout:
                try:
                    self.messages.put(json.loads(line))
                except json.JSONDecodeError:
                    self.messages.put({'transport_error': 'Non-JSON output on MCP stdout'})
        finally:
            self.messages.put({'transport_error': 'MCP process exited'})

    def notify(self, method, params):
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': method, 'params': params}) + '\n')
        self.process.stdin.flush()

    def call(self, method, params, timeout=90):
        self.sequence += 1
        ident = self.sequence
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': ident, 'method': method, 'params': params}) + '\n')
        self.process.stdin.flush()
        deadline = time.monotonic() + timeout
        while True:
            message = self.messages.get(timeout=max(0.01, deadline-time.monotonic()))
            if 'transport_error' in message:
                raise RuntimeError(message['transport_error'])
            if message.get('id') == ident:
                if 'error' in message:
                    raise RuntimeError(json.dumps(message['error']))
                return message['result']

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=3)
        self.process.stdout.close()
        self.stderr.close()


def tool(name, description, properties, required=()):
    return {'name': name, 'description': description,'annotations':{'readOnlyHint':name in ['workspace_list','workspace_read','workspace_search'],'destructiveHint':name in ['workspace_write','workspace_delete'],'openWorldHint':False}, 'inputSchema': {
        'type': 'object', 'properties': properties, 'required': list(required), 'additionalProperties': False}}


class Gateway:
    def __init__(self, config):
        self.root = Path(config['workspace']).resolve()
        self.log = Path(config['tool_log'])
        self.calls = 0
        self.dirty = False
        self.config = config
        self.limit = config.get('max_calls', 40)
        self.backend = None
        self.backend_names = set()
        self.tools = [
            tool('workspace_list', 'List all visible task files.', {}),
            tool('workspace_read', 'Read a task file with line numbers.', {'path': {'type': 'string'}}, ['path']),
            tool('workspace_search', 'Case-insensitive literal search over task source and documentation.', {'query': {'type': 'string'}}, ['query']),
            tool('workspace_write', 'Replace one task file with complete UTF-8 content.', {'path': {'type': 'string'}, 'content': {'type': 'string'}}, ['path','content']),
            tool('workspace_delete', 'Delete one task file.', {'path': {'type': 'string'}}, ['path']),
        ]
        if config.get('graph_command'):
            self.backend = StdioClient(config['graph_command'], self.root, Path(config['backend_stderr']))
            backend_tools = self.backend.call('tools/list', {})['tools']
            for item in backend_tools:
                if item['name'] in config['graph_tools']:
                    # The caller cannot change corpus/root/path parameters.
                    item = json.loads(json.dumps(item))
                    schema = item['inputSchema']
                    for key in ['project_path', 'path', 'root']:
                        schema.get('properties', {}).pop(key, None)
                        if key in schema.get('required', []):
                            schema['required'].remove(key)
                    self.tools.append(item)
                    self.backend_names.add(item['name'])
        Path(config['tool_log']).with_name('tool-inventory.json').write_text(json.dumps(self.tools,indent=2))
        self.schemas = {item['name']: item['inputSchema'] for item in self.tools}

    def files(self):
        return sorted(p for p in self.root.rglob('*') if p.is_file() and
                      not any(x.startswith('.') or x in ['cgraph-out','graphify-out','__pycache__']
                              for x in p.relative_to(self.root).parts) and
                      p.resolve().is_relative_to(self.root))

    def invoke(self, name, args):
        self.calls += 1
        if self.calls > self.limit:
            raise ValueError('Tool-call budget exhausted. Return the final answer now.')
        if name not in self.schemas:
            raise ValueError('Unknown tool')
        allowed = self.schemas[name].get('properties', {})
        if set(args) - set(allowed):
            raise ValueError('Unrecognized arguments')
        if name in self.backend_names:
            if self.dirty:
                if self.config.get('sync_command'):
                    completed = subprocess.run(self.config['sync_command'], cwd=self.root, capture_output=True, text=True, timeout=90)
                    if completed.returncode:
                        raise RuntimeError('Graph synchronization failed: '+completed.stderr[-2000:])
                else:
                    response = self.backend.call('tools/call', {'name':'graph_update','arguments':{'path':str(self.root)}})
                    if response.get('isError'):
                        raise RuntimeError('Graph synchronization failed: '+json.dumps(response))
                self.dirty = False
            return self.backend.call('tools/call', {'name': name, 'arguments': args})
        if name == 'workspace_list':
            value = '\n'.join(str(p.relative_to(self.root)) for p in self.files())
        elif name == 'workspace_read':
            p = contained(self.root, args['path'])
            value = '\n'.join(f'{n}: {s}' for n,s in enumerate(p.read_text().splitlines(), 1))
        elif name == 'workspace_search':
            needle = args['query'].casefold()
            if not needle:
                raise ValueError('Search query must not be empty')
            value = '\n'.join(f'{p.relative_to(self.root)}:{n}: {s}' for p in self.files()
                              for n,s in enumerate(p.read_text().splitlines(),1) if needle in s.casefold())
        elif name == 'workspace_write':
            p = contained(self.root, args['path'])
            if len(args['content']) > MAX_TEXT:
                raise ValueError('File is too large for this fixture')
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(args['content'])
            self.dirty = True
            value = 'File written.'
        elif name == 'workspace_delete':
            contained(self.root, args['path']).unlink()
            self.dirty = True
            value = 'File deleted.'
        else:
            raise ValueError('Unsupported tool')
        if len(value) > MAX_TEXT:
            value = value[:MAX_TEXT] + '\n[truncated]'
        return {'content': [{'type': 'text', 'text': value}]}

    def request(self, message):
        method = message['method']
        if method == 'initialize':
            return {'protocolVersion': '2024-11-05', 'capabilities': {'tools': {}},
                    'serverInfo': {'name': 'maintenance-eval', 'version': '1'}}
        if method == 'tools/list':
            return {'tools': self.tools}
        if method == 'ping':
            return {}
        if method != 'tools/call':
            raise ValueError('Unsupported MCP method')
        params = message['params']
        start = time.perf_counter()
        try:
            result = self.invoke(params['name'], params.get('arguments', {}))
        except Exception as error:
            result = {'isError': True, 'content': [{'type': 'text', 'text': str(error)}]}
        encoded = json.dumps(result, ensure_ascii=False)
        record = {'name': params['name'], 'arguments': params.get('arguments', {}),
                  'elapsed_seconds': time.perf_counter()-start, 'result': result,
                  'output_utf8_bytes': len(encoded.encode()),
                  'output_tokens_estimate_char4': (len(encoded)+3)//4,
                  'output_tokens_provider': None}
        with self.log.open('a') as out:
            out.write(json.dumps(record) + '\n')
        return result


def serve(config_path, gateway_type=Gateway):
    gateway = gateway_type(json.loads(Path(config_path).read_text()))
    try:
        for line in sys.stdin:
            message = json.loads(line)
            if 'id' not in message:
                continue
            try:
                response = {'jsonrpc':'2.0','id':message['id'],'result':gateway.request(message)}
            except Exception as error:
                response = {'jsonrpc':'2.0','id':message['id'],'error':{'code':-32603,'message':str(error)}}
            print(json.dumps(response), flush=True)
    finally:
        if gateway.backend:
            gateway.backend.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    serve(args.config)


if __name__ == '__main__':
    main()
