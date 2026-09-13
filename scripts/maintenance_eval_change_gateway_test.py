"""Exercise the optional bound-root gateway over a real installed MCP server."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import shutil
from maintenance_eval_gateway import StdioClient

class ChangeGatewayTests(unittest.TestCase):
    def test_actual_diff_and_rejected_root_override(self):
        configured=os.environ.get('CGRAPH_CHANGE_MCP')
        binary=Path(configured) if configured else None
        if binary is None or not binary.is_file():
            self.skipTest('Set CGRAPH_CHANGE_MCP to the installed change-context MCP executable')
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as temp:
            root=Path(temp);base=root/'base';target=root/'target';base.mkdir();target.mkdir()
            source='def helper():\n    return 1\n'
            (base/'main.py').write_text(source);(target/'main.py').write_text(source)
            config={'workspace':str(target),'tool_log':str(root/'tools.jsonl'),'backend_stderr':str(root/'backend.stderr'),
                    'graph_command':[str(binary),'--root',str(target)],'graph_tools':['graph_change_context'],
                    'base_workspace':str(base),'diff_path':str(root/'actual.diff')}
            config_path=root/'config.json';config_path.write_text(json.dumps(config))
            shutil.copy(Path(__file__).with_name('maintenance_eval_gateway.py'),root/'gateway.py')
            gateway=root/'extension.py'
            shutil.copy(Path(__file__).with_name('maintenance_eval_change_gateway.py'),gateway)
            client=StdioClient([sys.executable,str(gateway),'--config',str(config_path)],target,root/'gateway.stderr')
            try:
                edit=client.call('tools/call',{'name':'workspace_write','arguments':{'path':'main.py','content':source.replace('return 1','return 2').rstrip('\n')}})
                self.assertFalse(edit.get('isError',False))
                denied=client.call('tools/call',{'name':'graph_change_context','arguments':{'base_root':'/','budget':6000}})
                self.assertTrue(denied.get('isError'))
                result=client.call('tools/call',{'name':'graph_change_context','arguments':{'budget':6000}})
                self.assertFalse(result.get('isError',False),result)
                payload=json.loads(result['content'][0]['text'])
                self.assertTrue(payload.get('ok',True),payload)
                self.assertIn('-    return 1',(root/'actual.diff').read_text())
                self.assertIn('+    return 2',(root/'actual.diff').read_text())
                self.assertTrue(payload.get('changes'),payload)
                self.assertIn('\\ No newline at end of file',(root/'actual.diff').read_text())
            finally:
                client.close()

if __name__=='__main__':unittest.main()
