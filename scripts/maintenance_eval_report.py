#!/usr/bin/env python3
"""Join valid pilot rows, retain infrastructure failures, and expose metric limits."""
import argparse
import json
from pathlib import Path
import statistics


def retries(events):
    """Count identical tool attempts following an earlier failed identical call."""
    failed=set();count=0
    for event in events:
        key=json.dumps([event['name'],event.get('arguments',{})],sort_keys=True)
        if key in failed:
            count+=1
        if event.get('result',{}).get('isError'):
            failed.add(key)
        else:
            failed.discard(key)
    return count


def populated(response):
    if response.get('isError'):
        return False
    texts=[x.get('text','') for x in response.get('content',[]) if x.get('type')=='text']
    if not texts or any('No matching nodes found' in x for x in texts):
        return False
    try:
        data=json.loads(texts[0])
    except json.JSONDecodeError:
        return any('NODE' in x or 'nodes found' in x for x in texts)
    return data.get('ok',True) and bool(data.get('focus') or data.get('included') or data.get('nodes'))


def summarize(run_roots):
    rows=[];excluded=[];seen=set();manifests=[]
    for root in map(Path,run_roots):
        manifests.append({'run':root.name,'manifest':json.loads((root/'manifest.json').read_text())})
        for result in json.loads((root/'results.json').read_text()):
            if result['status']=='index_failed':
                stderr=(root/(result['task']+'--'+result['arm'])/Path(result['cold_index']['stderr_path']).name).read_text()
                if 'no LLM API key found' in stderr and '--code-only' not in result['cold_index']['command']:
                    excluded.append({'run':root.name,'exclusion_reason':'diagnosed pilot configuration: missing deterministic --code-only flag',**result});continue
                rows.append({'run':root.name,'task':result['task'],'arm':result['arm'],'status':'index_failed','completed_behavior_and_reported_edges':False,'model_usage':None,'usage_complete':False,'relationships':{'missing':None,'false':None},'raw_failure':result})
                continue
            key=(result['task'],result['arm'])
            if key in seen:
                raise ValueError('Duplicate scored task/arm: '+str(key))
            seen.add(key)
            suffix='cgraph' if result['arm']=='cgraph_change_context' else result['arm']
            case=root/(result['task']+'--'+suffix)
            logs=[json.loads(x) for x in (case/'tools.jsonl').read_text().splitlines()] if (case/'tools.jsonl').exists() else []
            usage={}
            for event in result['usage_events']:
                for name,value in event['usage'].items():
                    if isinstance(value,(int,float)):
                        usage[name]=usage.get(name,0)+value
            requests=result.get('warm_mcp',{}).get('requests',[])
            successful_warm=[x['elapsed_seconds'] for x in requests[1:] if populated(x['response'])]
            checks=result['grading']['checks']
            rows.append({'run':root.name,'task':result['task'],'arm':result['arm'],'language':result['language'],
                         'kind':result['kind'],'advisory_surface_exercised':result.get('advisory_surface_exercised'),'successful_nonempty_change_context_calls':result.get('successful_nonempty_change_context_calls'),'completed_behavior_and_reported_edges':result['grading']['completed'],
                         'behavior_checks':checks,'relationships':result['grading']['relationships'],
                         'tool_calls':result['tool_calls'],'tool_errors':result['tool_errors'],
                         'exact_failed_tool_retries':retries(logs),'provider_retries':None,
                         'provider_retries_note':'The CLI event stream does not expose an authoritative provider retry counter',
                         'tool_output_tokens_estimate_char4':result['tool_output_tokens_estimate_char4'],
                         'tool_output_provider_tokens':None,'model_usage':usage or None,'usage_complete':bool(usage),'money_usd':None,
                         'cold_index_seconds':result['cold_index']['elapsed_seconds'],
                         'mcp_initialization_seconds':result.get('warm_mcp',{}).get('initialization_seconds'),
                         'first_mcp_request_seconds':requests[0]['elapsed_seconds'] if requests else None,
                         'warm_populated_requests':len(successful_warm),
                         'warm_mcp_median_seconds':statistics.median(successful_warm) if successful_warm else None,
                         'end_of_case_synchronization_seconds':result.get('update',{}).get('elapsed_seconds'),
                         'agent_wall_seconds':result['agent']['elapsed_seconds'],
                         'peak_process_tree_rss_bytes_sampled':result['agent']['peak_process_tree_rss_bytes_sampled'] or None,
                         'memory_scope':result['agent']['memory_scope'],
                         'gateway_sha256':json.loads((case/'support/source-hashes.json').read_text())['gateway.py'],
                         'raw_case':str(case)})
    arms={}
    for arm in sorted({r['arm'] for r in rows}):
        selected=[r for r in rows if r['arm']==arm]
        arms[arm]={'tasks':len(selected),'completed_behavior_and_reported_edges':sum(r['completed_behavior_and_reported_edges'] for r in selected),
                   'missing_reported_edges':sum(len(r['relationships']['missing']) for r in selected) if all(r['relationships']['missing'] is not None for r in selected) else None,
                   'false_reported_edges':sum(len(r['relationships']['false']) for r in selected) if all(r['relationships']['false'] is not None for r in selected) else None,
                   'input_tokens':sum(r['model_usage']['input_tokens'] for r in selected) if all(r['model_usage'] and 'input_tokens' in r['model_usage'] for r in selected) else None,
                   'cached_input_tokens_subset':sum(r['model_usage']['cached_input_tokens'] for r in selected) if all(r['model_usage'] and 'cached_input_tokens' in r['model_usage'] for r in selected) else None,
                   'output_tokens':sum(r['model_usage']['output_tokens'] for r in selected) if all(r['model_usage'] and 'output_tokens' in r['model_usage'] for r in selected) else None,
                   'usage_complete':all(r['usage_complete'] for r in selected),'money_usd':None}
    return {'schema_version':1,'arms':arms,'rows':rows,'excluded_infrastructure_rows':excluded,'manifests':manifests,
            'limitations':[
                'Small independently reviewed Python and JavaScript fixtures; not production repositories or a statistically powered comparison.',
                'Relationship errors compare agent-reported answers with independent references, not exhaustive extractor edge precision/recall.',
                'Maintenance completion here combines hidden behavior tests and reported relationships; separate independent source review checks canonical delegation.',
                'Graphify uses deterministic code-only indexing; docs remain equally available through workspace tools. This does not evaluate semantic model ingestion.',
                'Post-task synchronization may be a no-op after watcher or query-triggered refresh; separate controlled mutation evidence measures freshness.',
                'First MCP request is separated from subsequent populated warm requests; failed or empty retrieval is not a speed success.',
                'Graph MCP tools provide different context representations. Timings do not establish equal output quality or overall superiority.',
                'Memory is sampled every 100 milliseconds, may miss transient peaks, and includes task-matched detached graphd processes.',
                'Tool token counts are explicitly char/4 estimates; provider model usage is retained separately. Cached input is a subset, not an extra input charge.',
                'Dollar costs and provider retry counts are unavailable rather than inferred from undocumented billing.',
                'The optional change-context arm is a four-task post-edit advisory subset, not a full fourth-arm trial.'
            ]}


def main():
    p=argparse.ArgumentParser();p.add_argument('run_roots',nargs='+');p.add_argument('--out',required=True);a=p.parse_args()
    Path(a.out).write_text(json.dumps(summarize(a.run_roots),indent=2))

if __name__=='__main__':main()
