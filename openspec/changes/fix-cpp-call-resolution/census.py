#!/usr/bin/env python3
"""Graph census for the fix-cpp-call-resolution change.

Reproducible before/after measurement of every number quoted in proposal.md.
Reads a deterministic graph.json produced by `cgraph --root . --out DIR`.

    python3 openspec/changes/fix-cpp-call-resolution/census.py DIR/graph.json

Reproduce the baseline: `cgraph --root . --out DIR` then run this script on DIR/graph.json.
Before/after captured in measurements/.

Deterministic: the random function pairs used for the path-routing measurement are
drawn from a fixed seed, so before and after sample the same pairs where the node
ids are unchanged.
"""

import collections
import json
import random
import re
import sys

CPP_SUFFIX = (".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    graph = json.load(open(sys.argv[1]))
    nodes = {n["id"]: n for n in graph["nodes"]}
    links = graph["links"]

    def kind(nid):
        return nodes.get(nid, {}).get("type")

    def is_cpp(nid):
        """Classify by the node's own source_file, not by substrings of its id."""
        src = nodes.get(nid, {}).get("source_file", "")
        return src.endswith(CPP_SUFFIX)

    print(f"nodes {len(nodes)}  links {len(links)}")
    print()

    print("== node kinds ==")
    for k, c in collections.Counter(n.get("type") for n in graph["nodes"]).most_common():
        print(f"  {k:12s} {c}")
    print()

    print("== edge relations ==")
    for r, c in collections.Counter(e.get("relation") for e in links).most_common():
        print(f"  {r:14s} {c}")
    print()

    calls = [e for e in links if e.get("relation") == "CALLS"]
    print("== call edges ==")
    print(f"  total CALLS                {len(calls)}")
    print(f"  sourced from a C/C++ file  {sum(1 for e in calls if is_cpp(e['source']))}")
    print("  target kinds:", dict(collections.Counter(kind(e["target"]) for e in calls)))
    bad = [e for e in calls if kind(e["target"]) == "field"]
    print(f"  targeting a field node     {len(bad)}   <- Cause C")
    for e in bad:
        print(f"     {nodes[e['source']].get('label','')[:44]!r} -> {nodes[e['target']].get('label','')!r}")
    print()

    fns = [n for n in graph["nodes"] if n.get("type") == "function"]
    cpp_fns = [n for n in fns if is_cpp(n["id"])]
    cpp_fn_ids = {n["id"] for n in cpp_fns}
    incoming = {e["target"] for e in calls}
    sem = collections.Counter()
    for e in links:
        if e.get("relation") in ("CALLS", "references", "imports"):
            sem[e["source"]] += 1
            sem[e["target"]] += 1
    no_sem = [n for n in fns if sem[n["id"]] == 0]
    zero_arg = [n for n in cpp_fns if re.search(r"\(\s*\)\s*$", n.get("label", ""))]
    # Only C/C++ -> C/C++-function edges, so the zero-arg share is comparable to
    # the zero-arg share of the C/C++ function population.
    cpp_calls = [e for e in calls if is_cpp(e["source"]) and e["target"] in cpp_fn_ids]
    zero_arg_targets = [
        e for e in cpp_calls
        if re.search(r"\(\s*\)\s*$", nodes.get(e["target"], {}).get("label", ""))
    ]
    # `operator()` legitimately contains parentheses as part of its name, so a
    # bare "(" test would report it as signature-bearing. Strip a leading
    # operator name before looking for a parameter list.
    def carries_signature(label):
        if label.startswith("operator"):
            label = label[len("operator"):].lstrip()
            for symbol in ("()", "[]", "->", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
                           "+", "-", "*", "/", "%", "<", ">", "=", "!", "~", "^", "&", "|", ","):
                if label.startswith(symbol):
                    label = label[len(symbol):]
                    break
        return "(" in label

    sig = [n for n in cpp_fns if carries_signature(n.get("label", ""))]
    print("== function connectivity ==")
    print(f"  function nodes                       {len(fns)}")
    print(f"  ... with NO incoming CALLS           {len(fns)-len(set(n['id'] for n in fns) & incoming)}")
    print(f"  ... with NO call/ref/import edge     {len(no_sem)}  ({100*len(no_sem)//len(fns)}%)")
    print(f"  mean semantic degree                 {sum(sem[n['id']] for n in fns)/len(fns):.2f}")
    print()
    print("== Cause A signal ==")
    print(f"  C/C++ function nodes                 {len(cpp_fns)}")
    print(f"  ... whose label carries a signature  {len(sig)}  ({100*len(sig)//max(1,len(cpp_fns))}%)")
    print(f"  ... zero-argument                    {len(zero_arg)}  ({100*len(zero_arg)//max(1,len(cpp_fns))}%)")
    if cpp_calls:
        print(f"  C/C++ -> C/C++ function CALLS edges  {len(cpp_calls)}")
        print(f"  ... targeting a zero-arg function    {len(zero_arg_targets)}  ({100*len(zero_arg_targets)//len(cpp_calls)}%)")
        print("  (a zero-arg share far above the population share means only accidental matches resolve)")
    longest = max((len(n.get("label", "")), n.get("label", "")) for n in cpp_fns) if cpp_fns else (0, "")
    print(f"  longest C/C++ label                  {longest[0]} chars, {longest[1].count(chr(10))+1} lines")
    print()

    print("== Cause D: namespace as class ==")
    cls = [n for n in graph["nodes"] if n.get("type") == "class"]
    ns = [n for n in cls if n.get("label") == "cgraph"]
    ns_ids = {n["id"] for n in ns}
    meth = [e for e in links if e.get("relation") == "method"]
    print(f"  class nodes                          {len(cls)}")
    print(f"  ... labelled exactly 'cgraph'        {len(ns)}  ({100*len(ns)//max(1,len(cls))}%)")
    print(f"  method edges                         {len(meth)}")
    print(f"  ... originating at one of them       {sum(1 for e in meth if e['source'] in ns_ids)}")
    print()

    deg = collections.Counter()
    for e in links:
        deg[e["source"]] += 1
        deg[e["target"]] += 1
    print("== top-degree nodes ==")
    for nid, d in deg.most_common(5):
        n = nodes.get(nid, {})
        print(f"  deg {d:4d}  {str(n.get('type')):9s} {n.get('label','')[:46]!r}")
    print()

    adj = collections.defaultdict(set)
    for e in links:
        adj[e["source"]].add(e["target"])
        adj[e["target"]].add(e["source"])
    fn_ids = sorted(n["id"] for n in fns)
    rng = random.Random(7)
    pairs = [(rng.choice(fn_ids), rng.choice(fn_ids)) for _ in range(300)]
    conn = via = 0
    for a, b in pairs:
        if a == b:
            continue
        prev = {a: None}
        q = collections.deque([a])
        found = False
        while q:
            u = q.popleft()
            if u == b:
                found = True
                break
            for v in adj[u]:
                if v not in prev:
                    prev[v] = u
                    q.append(v)
        if not found:
            continue
        conn += 1
        path, u = [], b
        while u is not None:
            path.append(u)
            u = prev[u]
        if any(x in ns_ids for x in path[1:-1]):
            via += 1
    print("== path routing (300 random function pairs, seed 7) ==")
    print(f"  connected pairs                      {conn}")
    print(f"  ... routed via a namespace node      {via}  ({100*via//max(1,conn)}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
