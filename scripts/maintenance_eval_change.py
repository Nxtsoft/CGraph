#!/usr/bin/env python3
"""Optional post-edit advisory arm over actual candidate changes, not ideal patches."""
import argparse
import json
from pathlib import Path
import shutil
import maintenance_eval as baseline


def prepare_gateway(config,workspace,support):
    base=support/'base';base.mkdir()
    for relative in baseline.digest_tree(workspace):
        target=base/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy(workspace/relative,target)
    config.update(base_workspace=str(base),diff_path=str(support/'candidate.diff'))


def successful_changes(events):
    count=0
    for event in events:
        if event['name']!='graph_change_context' or event['result'].get('isError'):
            continue
        try:
            payload=json.loads(event['result']['content'][0]['text'])
        except (KeyError,IndexError,json.JSONDecodeError):
            continue
        if payload.get('schema_version')==1 and payload.get('base') and payload.get('target') and payload.get('changes'):
            count+=1
    return count


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--config',required=True);parser.add_argument('--out',required=True)
    parser.add_argument('--mcp',required=True);parser.add_argument('--run',action='store_true');args=parser.parse_args()
    config=json.loads(Path(args.config).read_text());config['cgraph_mcp']=str(Path(args.mcp).resolve())
    tasks=[t for t in json.loads((baseline.RESEARCH/'tasks.json').read_text()) if t['id'] in
           ['payments-interface','routing-import','routing-delete','routing-reexport']]
    refs=json.loads((baseline.RESEARCH/'references.json').read_text())
    extension=Path(__file__).with_name('maintenance_eval_change_gateway.py')
    manifest={'config':config,'arm':'cgraph_change_context','tasks':[t['id'] for t in tasks],
              'extra_tool':'graph_change_context after edits using actual candidate diff',
              'mcp_sha256_before':baseline.file_hash(args.mcp),'wrapper_sha256':baseline.file_hash(__file__),
              'extension_sha256':baseline.file_hash(extension),'baseline_harness_sha256':baseline.file_hash(baseline.__file__),
              'gateway_sha256':baseline.file_hash(Path(__file__).with_name('maintenance_eval_gateway.py'))}
    if not args.run:print(json.dumps(manifest,indent=2));return
    out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=False)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    results=[]
    for task in tasks:
        print('Running '+task['id']+' cgraph_change_context',flush=True)
        result=baseline.run_case(config,task,refs[task['id']],'cgraph',out,gateway_extension=extension,
                                extra_tools=['graph_change_context'],prepare_gateway=prepare_gateway,
                                prompt_suffix='After editing, call graph_change_context with budget 6000 to review the actual change before returning the requested JSON.')
        result['arm']='cgraph_change_context'
        case=out/(task['id']+'--cgraph')
        logs=[json.loads(x) for x in (case/'tools.jsonl').read_text().splitlines()]
        result['change_context_calls']=sum(x['name']=='graph_change_context' for x in logs)
        result['successful_nonempty_change_context_calls']=successful_changes(logs)
        result['advisory_surface_exercised']=bool(result['successful_nonempty_change_context_calls'])
        results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2))
    manifest['mcp_sha256_after']=baseline.file_hash(args.mcp)
    manifest['executable_unchanged']=manifest['mcp_sha256_before']==manifest['mcp_sha256_after']
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps({'runs':len(results),'completed':sum(r['grading']['completed'] for r in results)}))


if __name__=='__main__':main()
