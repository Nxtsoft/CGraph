"""Real gateway protocol and path confinement regression tests."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from maintenance_eval_gateway import contained

class GatewayTests(unittest.TestCase):
    def test_path_and_symlink_escape(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as temp:
            root = Path(temp)
            (root/'escape').symlink_to(root.parent)
            for value in ['../hidden','escape/hidden','.private','graphify-out/graph.json','cgraph-out/graph.json']:
                with self.assertRaises(ValueError):
                    contained(root,value)

    def test_actual_stdio_read_and_rejected_escape(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as temp:
            root = Path(temp)
            workspace = root/'workspace';workspace.mkdir()
            (workspace/'source.py').write_text('def helper(): return 7\n')
            config = root/'config.json'
            config.write_text(json.dumps({'workspace':str(workspace),'tool_log':str(root/'tools.jsonl')}))
            requests = [
                {'jsonrpc':'2.0','id':1,'method':'initialize','params':{}},
                {'jsonrpc':'2.0','id':2,'method':'tools/call','params':{'name':'workspace_read','arguments':{'path':'source.py'}}},
                {'jsonrpc':'2.0','id':3,'method':'tools/call','params':{'name':'workspace_read','arguments':{'path':'../config.json'}}},
            ]
            result = subprocess.run([sys.executable,str(Path(__file__).with_name('maintenance_eval_gateway.py')),'--config',str(config)],input=''.join(json.dumps(x)+'\n' for x in requests),capture_output=True,text=True,check=True)
            responses = [json.loads(x) for x in result.stdout.splitlines()]
            self.assertIn('helper',responses[1]['result']['content'][0]['text'])
            self.assertTrue(responses[2]['result']['isError'])
            self.assertNotIn('tool_log',responses[2]['result']['content'][0]['text'])

if __name__ == '__main__': unittest.main()
