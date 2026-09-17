#!/usr/bin/env python3
"""Change-context MCP adapter whose public base and target paths are fixed."""
import argparse
import difflib
import json
from pathlib import Path
# Copied alongside the canonical gateway by the runner.
from gateway import Gateway, serve

class ChangeGateway(Gateway):
    def __init__(self, config):
        super().__init__(config)
        for item in self.tools:
            if item['name']=='graph_change_context':
                item['description']+=' Use after task edits; base, target, and actual diff are bound by the harness.'
                for key in ['base_root','target_root','diff_path','expected_base_content_root','expected_target_content_root']:
                    item['inputSchema'].get('properties',{}).pop(key,None)
                    if key in item['inputSchema'].get('required',[]):
                        item['inputSchema']['required'].remove(key)
        self.schemas={item['name']:item['inputSchema'] for item in self.tools}
        Path(config['tool_log']).with_name('tool-inventory.json').write_text(json.dumps(self.tools,indent=2))

    def invoke(self,name,args):
        if name!='graph_change_context':
            return super().invoke(name,args)
        self.calls+=1
        if self.calls>self.limit:
            raise ValueError('Tool-call budget exhausted')
        if set(args)-set(self.schemas[name].get('properties',{})):
            raise ValueError('Unrecognized arguments')
        base=Path(self.config['base_workspace'])
        before={str(p.relative_to(base)):p.read_text() for p in base.rglob('*') if p.is_file()}
        after={str(p.relative_to(self.root)):p.read_text() for p in self.files()}
        diff=[]
        for path in sorted(before.keys()|after.keys()):
            diff.extend(difflib.unified_diff(before.get(path,'').splitlines(True),after.get(path,'').splitlines(True),
                        fromfile='a/'+path if path in before else '/dev/null',
                        tofile='b/'+path if path in after else '/dev/null'))
        diff_path=Path(self.config['diff_path'])
        diff_path.write_text(''.join(line if line.endswith('\n') else line+'\n\\ No newline at end of file\n' for line in diff))
        return self.backend.call('tools/call',{'name':name,'arguments':{
            **args,'base_root':str(base),'target_root':str(self.root),'diff_path':str(diff_path)}})

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--config',required=True);args=parser.parse_args()
    serve(args.config,ChangeGateway)
