#!/usr/bin/env python3
"""Run a bounded maintenance pilot with hidden grading and real MCP tool arms.

No model run occurs without --run. Configuration pins executable paths and source
identities. Gold is consumed only by this controller, never copied into a task root.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import threading
import time

from maintenance_eval_gateway import StdioClient

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / 'tests/fixtures/maintenance_eval'
RESEARCH = ROOT / 'research/maintenance_eval'
ARMS = ['search', 'cgraph', 'graphify']


def digest_tree(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file() and not any(
                x.startswith('.') or x in ['__pycache__','cgraph-out','graphify-out']
                for x in p.relative_to(root).parts)}


def file_hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sandbox_profile(hidden_paths, workspace=None):
    # Full task execution inherits this profile, including children and MCPs.
    denies = '\n'.join('(deny file-read* file-write* (subpath ' + json.dumps(str(Path(p).resolve())) + '))'
                       for p in hidden_paths)
    return '(version 1)\n(allow default)\n' + denies + '\n'


def process_tree_rss(pid, workspace):
    rows = subprocess.run(['/bin/ps','-axo','pid=,ppid=,rss=,args='], capture_output=True,text=True,check=True).stdout
    parsed = [line.split(maxsplit=3) for line in rows.splitlines()]
    entries = [tuple(map(int, parts[:3])) for parts in parsed if len(parts) >= 3]
    descendants = {pid} | {int(parts[0]) for parts in parsed if len(parts)==4 and '/graphd' in parts[3] and ('--root '+str(workspace)+' ') in (parts[3]+' ')}
    while True:
        grown = descendants | {child for child,parent,_ in entries if parent in descendants}
        if grown == descendants:
            break
        descendants = grown
    return sum(rss for child,_,rss in entries if child in descendants)*1024


def measured(command, cwd, output, *, stdin=None, timeout=300, env=None):
    start = time.perf_counter()
    with open(str(output)+'.stdout','w') as out, open(str(output)+'.stderr','w') as err:
        proc = subprocess.Popen(command,cwd=cwd,stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                stdout=out,stderr=err,text=True,env=env)
        peak = 0
        if stdin:
            proc.stdin.write(stdin)
            proc.stdin.close()
        timed_out = False
        while proc.poll() is None:
            peak = max(peak, process_tree_rss(proc.pid,cwd))
            if time.perf_counter()-start > timeout:
                proc.terminate()
                timed_out = True
                break
            time.sleep(.1)
        try:
            code = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = proc.wait()
    return {'command': command,'returncode':code,'elapsed_seconds':time.perf_counter()-start,
            'peak_process_tree_rss_bytes_sampled':peak,'memory_scope':'sampled process descendants plus graphd processes matching exact task workspace argument','rss_sample_interval_seconds':.1,
            'timed_out':timed_out,'stdout_path':str(output)+'.stdout','stderr_path':str(output)+'.stderr'}


def isolated_candidate(workspace, script, language, hidden, output):
    # Candidate sees inputs but no expected values. Parent process owns gold.
    command = [sys.executable,'-I','-c',script,str(workspace)] if language == 'python' else ['node','--input-type=module','-e',script,str(workspace)]
    profile = sandbox_profile(hidden) + '(deny network*)\n'
    command = ['/usr/bin/sandbox-exec','-p',profile,*command]
    result = measured(command,workspace,output,timeout=15)
    try:
        value = json.loads(Path(result['stdout_path']).read_text()) if result['returncode'] == 0 else None
    except json.JSONDecodeError:
        value = None
    return value,result


def grade(task, reference, workspace, answer, hidden, output):
    expected = {tuple(pair) for pair in reference['relationships']}
    reported = {(r['source'],r['target']) for r in answer.get('relationships',[])
                if isinstance(r,dict) and isinstance(r.get('source'),str) and isinstance(r.get('target'),str)}
    checks = {}
    before = digest_tree(FIXTURES/task['fixture'])
    after = digest_tree(workspace)
    checks_needed = reference['checks']
    if 'no_edits' in checks_needed:
        checks['no_edits'] = before == after
    if 'renamed' in checks_needed:
        sources = '\n'.join(p.read_text() for p in workspace.glob('*.py'))
        checks['old_name_removed'] = 'amount_due' not in sources
        checks['new_name_defined'] = 'def net_due(' in (workspace/'pricing.py').read_text()
    if 'legacy_preserved' in checks_needed:
        checks['legacy_preserved'] = after.get('legacy.mjs') == before['legacy.mjs']
    if 'legacy_removed' in checks_needed:
        checks['legacy_removed'] = not (workspace/'legacy.mjs').exists()
        checks['legacy_references_removed'] = not any('legacy.mjs' in p.read_text() for p in workspace.glob('*.mjs'))
    candidate = None
    if any(c in checks_needed for c in ['old_behavior','policy_behavior','interface_behavior']):
        symbol = 'net_due' if 'old_behavior' in checks_needed else 'amount_due'
        script = "import sys,json;sys.path.insert(0,sys.argv[1]);import pricing,workflow,receipt,api;cases=[(101,25,33),(1000,250,20),(0,99,100),(100,0,0)];f=getattr(pricing,"+repr(symbol)+");print(json.dumps([[f(*c),workflow.quote_invoice(*c),receipt.render_receipt(*c),api.checkout(*c)] for c in cases]))"
        if 'interface_behavior' in checks_needed:
            script = script.replace('f(*c)', "f(dict(zip(['subtotal_cents','shipping_cents','discount_percent'],c)))")
        value,candidate = isolated_candidate(workspace,script,'python',hidden,output)
        totals = [84,1000,0,100] if any(c in checks_needed for c in ['old_behavior','interface_behavior']) else [93,1050,99,100]
        checks['behavior'] = value == [[x,{'total_cents':x},f'Total: {x} cents',{'total_cents':x}] for x in totals]
    if 'facade_redirected' in checks_needed:
        checks['app_import_unchanged'] = after.get('app.mjs') == before['app.mjs']
        checks['cli_import_unchanged'] = after.get('cli.mjs') == before['cli.mjs']
    if 'modern_behavior' in checks_needed:
        script = "import{pathToFileURL}from'node:url';const base=pathToFileURL(process.argv[1]+'/');const app=await import(new URL('app.mjs',base));const cli=await import(new URL('cli.mjs',base));const inputs=['/A//B/?Q=X?Y','///?UP=YES','/','ABC'];console.log(JSON.stringify([inputs.map(x=>app.routeRequest(x)),app.routeBatch(inputs),inputs.map(x=>cli.main(x))]));"
        value,candidate = isolated_candidate(workspace,script,'javascript',hidden,output)
        routes = ['/a/b?Q=X?Y','/?UP=YES','/','/abc']
        objs = [{'route':x} for x in routes]
        checks['behavior'] = value == [objs,objs,[json.dumps(x,separators=(',',':')) for x in objs]]
    return {'checks':checks,'completion_basis':'behavioral_or_no_edit_checks_and_agent_reported_relationships; canonical delegation needs independent source review','completed':all(checks.values()) and reported == expected,
            'relationships':{'measurement':'agent_reported_edges_against_independent_reference','expected':sorted(expected),'reported':sorted(reported),
                             'missing':sorted(expected-reported),'false':sorted(reported-expected)},
            'changed_files':sorted(k for k in before.keys()|after.keys() if before.get(k)!=after.get(k)),
            'candidate_execution':candidate}


def response_schema():
    return {'type':'object','properties':{'summary':{'type':'string'},'relationships':{'type':'array','items':{
        'type':'object','properties':{'source':{'type':'string'},'target':{'type':'string'}},
        'required':['source','target'],'additionalProperties':False}}},
        'required':['summary','relationships'],'additionalProperties':False}


def graph_config(config,arm,workspace):
    if arm == 'cgraph':
        return [config['cgraph_mcp'],'--root',str(workspace),'--daemon',config['graphd']], ['graph_query','graph_explain','graph_context','graph_impact','graph_path','graph_status','graph_update']
    if arm == 'graphify':
        return [config['graphify_python'],'-m','graphify.serve',str(workspace/'graphify-out/graph.json')], ['query_graph','get_node','get_neighbors','shortest_path','graph_stats']
    return [],[]


def index(config,arm,workspace,output):
    if arm == 'search':
        return {'elapsed_seconds':0,'returncode':0,'not_applicable':True,'model_tokens':0,'money_usd':0}
    command = [config['cgraph'],'--root',str(workspace),'--out',str(workspace/'cgraph-out')] if arm == 'cgraph' else [config['graphify'],'extract',str(workspace),'--no-cluster','--code-only']
    result = measured(command,workspace,output)
    result.update(model_tokens=0,money_usd=0,mode='deterministic source only')
    return result


def probe(config,arm,workspace,output):
    if arm == 'search':
        return {'not_applicable':True}
    command,_ = graph_config(config,arm,workspace)
    start = time.perf_counter()
    client = StdioClient(command,workspace,Path(str(output)+'.stderr'))
    initialization = time.perf_counter()-start
    try:
        request = {'name':'graph_context','arguments':{'query':'amount_due routeRequest','budget':2000}} if arm == 'cgraph' else {'name':'query_graph','arguments':{'question':'amount_due routeRequest','token_budget':2000}}
        values = []
        for _ in range(3):
            start = time.perf_counter()
            response = client.call('tools/call',request)
            values.append({'elapsed_seconds':time.perf_counter()-start,'response':response})
        return {'initialization_seconds':initialization,'requests':values,'fixed_query':request}
    finally:
        client.close()


def update_graph(config,arm,workspace,output):
    if arm == 'search':
        return {'not_applicable':True}
    if arm == 'graphify':
        return measured([config['graphify'],'update',str(workspace),'--no-cluster'],workspace,output)
    command,_ = graph_config(config,arm,workspace)
    client=StdioClient(command,workspace,Path(str(output)+'.stderr'))
    try:
        start=time.perf_counter()
        response=client.call('tools/call',{'name':'graph_update','arguments':{'path':str(workspace)}})
        return {'elapsed_seconds':time.perf_counter()-start,'response':response,'method':'graph_update MCP'}
    finally:
        client.close()


def run_case(config,task,reference,arm,out, *, gateway_extension=None, extra_tools=(), prepare_gateway=None, prompt_suffix=""):
    case = out/(task['id']+'--'+arm)
    case.mkdir()
    workspace = case/'workspace'
    shutil.copytree(FIXTURES/task['fixture'],workspace)
    # Hash the public input independently of either graph's internal node IDs.
    input_hashes = digest_tree(workspace)
    support = case/'support'
    support.mkdir()
    shutil.copy(ROOT/'scripts/maintenance_eval_gateway.py',support/'gateway.py')
    gateway_entry=support/'gateway.py'
    if gateway_extension is not None:
        gateway_entry=support/'gateway_extension.py'
        shutil.copy(gateway_extension,gateway_entry)
    hidden = [str(p) for p in out.parent.iterdir() if p.is_dir() and (p/'results.json').exists() and p != out] + [str(ROOT),str(out/'results.json'),*[str(p) for p in out.iterdir() if p.is_dir() and p != case], *config.get('hidden_paths',[])]
    profile = sandbox_profile(hidden)
    # Exact negative probe under the same profile that wraps the model and all children.
    sentinel = RESEARCH/'references.json'
    isolation = measured(['/usr/bin/sandbox-exec','-p',profile,'/bin/cat',str(sentinel)],workspace,case/'isolation')
    if isolation['returncode'] == 0 or 'Operation not permitted' not in Path(isolation['stderr_path']).read_text():
        raise RuntimeError('Hidden reference isolation probe failed')
    cold = index(config,arm,workspace,case/'cold-index')
    if cold['returncode']:
        return {'task':task['id'],'arm':arm,'cold_index':cold,'status':'index_failed'}
    warm = probe(config,arm,workspace,case/'warm')
    command, names = graph_config(config,arm,workspace)
    names += list(extra_tools)
    gateway_config = {'workspace':str(workspace),'tool_log':str(case/'tools.jsonl'),
                      'backend_stderr':str(case/'backend.stderr'),'graph_command':command,
                      'graph_tools':names,'sync_command':[config['graphify'],'update',str(workspace),'--no-cluster'] if arm == 'graphify' else None,'max_calls':config.get('max_calls',40)}
    if prepare_gateway is not None:
        prepare_gateway(gateway_config,workspace,support)
    (support/'gateway.json').write_text(json.dumps(gateway_config))
    (support/'schema.json').write_text(json.dumps(response_schema()))
    prompt = ('Work only through the maintenance MCP tools. Do not use shell, browser, external services, or any other tools. '
              'The public task workspace is the only allowed information source. '
              'Use search/read tools as needed; if graph tools are available, make at least one graph query before editing. '
              'Finish within '+str(config.get('max_calls',40))+' tool calls. Return the requested JSON. Task: '+task['prompt']+' '+prompt_suffix)
    model_command = [config['codex'],'exec','--json','--ignore-user-config','--ephemeral','--skip-git-repo-check',
                     '--model',config['model'],'--sandbox','read-only','--cd',str(workspace),
                     '--output-schema',str(support/'schema.json'),'--output-last-message',str(case/'answer.json')]
    for feature in ['shell_tool','unified_exec','view_image','apps','browser_use','browser_use_external','computer_use','plugins','multi_agent','skill_search','hooks']:
        model_command += ['--disable',feature]
    for key,value in {
        'project_doc_max_bytes':0,
        'web_search':'disabled',
        'mcp_servers.maintenance.default_tools_approval_mode':'approve',
        'model_reasoning_effort':config.get('reasoning_effort','low'),
        'mcp_servers.maintenance.command':sys.executable,
        'mcp_servers.maintenance.args':[str(gateway_entry),'--config',str(support/'gateway.json')],
        'mcp_servers.maintenance.startup_timeout_sec':90,
    }.items():
        model_command += ['-c',key+'='+json.dumps(value)]
    model_command += ['-']
    run = measured(['/usr/bin/sandbox-exec','-p',profile,*model_command],workspace,case/'agent',stdin=prompt,
                   timeout=config.get('timeout_seconds',180))
    events = []
    for line in Path(run['stdout_path']).read_text().splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    usage = [e for e in events if 'usage' in e]
    try:
        answer = json.loads((case/'answer.json').read_text())
    except (OSError,json.JSONDecodeError):
        answer = {}
    grading = grade(task,reference,workspace,answer,hidden,case/'candidate')
    tool_events = [json.loads(l) for l in (case/'tools.jsonl').read_text().splitlines()] if (case/'tools.jsonl').exists() else []
    graph_calls = [e for e in tool_events if e['name'] in names]
    if run['returncode'] or (arm != 'search' and not graph_calls):
        grading['completed'] = False
    updated = update_graph(config,arm,workspace,case/'update')
    result = {'task':task['id'],'language':task['language'],'kind':task['kind'],'arm':arm,
              'input_sha256':input_hashes,'isolation_probe':isolation,'cold_index':cold,'warm_mcp':warm,
              'agent':run,'usage_events':usage,'money_usd':None,'money_note':'Provider billing unavailable; raw usage retained',
              'grading':grading,'tool_calls':len(tool_events),'graph_calls':len(graph_calls),
              'tool_errors':sum(bool(e['result'].get('isError')) for e in tool_events),
              'tool_output_tokens_estimate_char4':sum(e['output_tokens_estimate_char4'] for e in tool_events),
              'provider_tool_tokens':None,'update':updated,'status':'finished' if run['returncode']==0 else 'agent_failed'}
    if arm == 'cgraph':
        measured([config['cgraph_client'],'--root',str(workspace),'--daemon',config['graphd'],'shutdown'],workspace,case/'daemon-shutdown',timeout=15)
    (case/'result.json').write_text(json.dumps(result,indent=2))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config',required=True)
    parser.add_argument('--out',required=True)
    parser.add_argument('--tasks',nargs='*')
    parser.add_argument('--arms',nargs='+',choices=ARMS,default=ARMS)
    parser.add_argument('--run',action='store_true',help='Authorize the bounded model pilot')
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    tasks = json.loads((RESEARCH/'tasks.json').read_text())
    refs = json.loads((RESEARCH/'references.json').read_text())
    if args.tasks:
        tasks = [t for t in tasks if t['id'] in args.tasks]
    if not tasks:
        raise SystemExit('No tasks selected')
    manifest = {'platform':platform.platform(),'machine':platform.machine(),'python':sys.version,
                'config':config,'task_file_sha256':file_hash(RESEARCH/'tasks.json'),
                'reference_file_sha256':file_hash(RESEARCH/'references.json'),
                'harness_sha256':file_hash(__file__),'gateway_sha256':file_hash(ROOT/'scripts/maintenance_eval_gateway.py'),'executables_sha256':{k:file_hash(config[k]) for k in ['cgraph','graphd','cgraph_mcp','graphify','graphify_python']},'arms':args.arms,'tasks':[t['id'] for t in tasks]}
    if not args.run:
        print(json.dumps(manifest,indent=2))
        return
    out = Path(args.out).resolve()
    if out.is_relative_to(ROOT):
        raise SystemExit('Run directory must be outside the hidden source checkout')
    out.mkdir(parents=True,exist_ok=False)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    results = []
    for task in tasks:
        for arm in args.arms:
            print('Running '+task['id']+' '+arm,flush=True)
            results.append(run_case(config,task,refs[task['id']],arm,out))
            (out/'results.json').write_text(json.dumps(results,indent=2))
    print(json.dumps({'runs':len(results),'completed':sum(r.get('grading',{}).get('completed',False) for r in results)}))


if __name__ == '__main__':
    main()
