"""codebase-memory-mcp predictions: the CROSS_* edges `index_repository --mode
cross-repo-intelligence` stored for every service of a system.

    python3 predict_cbm.py <system> <cbm binary> <service> ...      (JSON lines on stdout)

Run with the same HOME the indexing used. Projects are named `<system>-<service>`.

A cross edge's properties name the other side (`target_project`, `target_function`) and, for
HTTP, `transport` (the method) and `url_path`. One link can be stored in either project:
  - in the caller's project, from the calling code to a route node it does not handle
    (consumer = this project, provider = `target_project`);
  - in the provider's project, from the handler to the route it HANDLES, pointing back at the
    caller (consumer = `target_project`, provider = this project).
Rows are told apart by that HANDLES edge. The two copies of one link are merged on (consumer
service, consumer function, provider service, provider function) before keying, taking the
provider-side `transport` when the caller-side copy has none, so a link stored on both sides
counts once and a provider-side copy is never read as the provider calling its caller.

`query_graph` pages its answer: a page ends at the 200-row LIMIT or earlier, when the reply
reaches its output budget (`truncated: true`, `truncation_reason: output_budget`). Every query
is ordered on a stable key and read page by page while the reply says `has_more: true`,
advancing by the page's `next_offset` (else by its `returned` count); rows are deduplicated.
An absolute `url_path` (`https://host/x`) keys as its path (`/x`). gRPC and async/channel edges
key by `url_path` (a gRPC path `/pkg.Service/Method`, a topic name) or, for async edges without
one, by a `topic` / `channel` property; a row with none of these is skipped and reported on
stderr with its properties.
"""
import json
import re
import subprocess
import sys
from urllib.parse import urlsplit

KINDS = {"CROSS_HTTP_CALLS": "http", "CROSS_GRPC_CALLS": "grpc", "CROSS_ASYNC_CALLS": "topic", "CROSS_CHANNEL": "topic"}
PAGE = 200
ORDER = "ORDER BY a.qualified_name, b.qualified_name, a.name, b.name"
# `  <a.name> "<r as an escaped JSON string>" <b.name>`
ROW = re.compile(r'^  (.*?) "(\{.*\})" (.*)$')


def run_query(cbm, project, cypher):
    args = json.dumps({"project": project, "query": cypher})
    out = subprocess.run([cbm, "cli", "--quiet", "--json", "query_graph", args], check=True, capture_output=True,
                         text=True).stdout
    return json.loads(out)["content"][0]["text"]


def meta(text, name):
    found = re.search(rf"^{name}: (\S+)$", text, re.MULTILINE)
    return found.group(1) if found else None


def rows(cbm, project, match):
    """Every distinct (source name, properties, target name) row of `match`, read page by page."""
    found = {}
    skip = 0
    while True:
        text = run_query(cbm, project, f"{match} RETURN a.name, r, b.name {ORDER} SKIP {skip} LIMIT {PAGE}")
        for line in text.splitlines():
            parsed = ROW.match(line)
            if parsed:
                raw = json.loads('"' + parsed.group(2) + '"')
                found.setdefault((parsed.group(1), raw, parsed.group(3)),
                                 (parsed.group(1), json.loads(raw), parsed.group(3)))
        returned = int(meta(text, "returned") or 0)
        if meta(text, "has_more") != "true" or returned == 0:
            break
        skip += int(meta(text, "next_offset") or returned)
    return list(found.values())


def path_of(url):
    return (urlsplit(url).path or "/") if url.startswith(("http://", "https://")) else url


def key_of(kind, props):
    url = props.get("url_path", "")
    if kind == "http":
        return f"{props.get('transport', '')} {path_of(url)}"
    if kind == "grpc":
        return url.lstrip("/") or None
    return url or props.get("topic") or props.get("channel") or None


def main():
    system, cbm, services = sys.argv[1], sys.argv[2], sys.argv[3:]
    prefix = system + "-"
    # (kind, consumer service, consumer function, provider service, provider function, path) ->
    #   {"caller": props of the caller-side copy, "provider": props of the provider-side copy}
    links = {}
    for service in services:
        project = prefix + service
        for edge_type, kind in KINDS.items():
            everything = rows(cbm, project, f"MATCH (a)-[r:{edge_type}]->(b)")
            handled = rows(cbm, project, f"MATCH (a)-[r:{edge_type}]->(b) MATCH (a)-[h:HANDLES]->(b)")
            provider_side = {(name, json.dumps(props, sort_keys=True), target) for name, props, target in handled}
            for name, props, target_name in everything:
                target = props.get("target_project", "")
                if not target.startswith(prefix):
                    continue
                other = target[len(prefix):]
                remote = props.get("target_function", "")
                path = path_of(props.get("url_path", ""))
                if (name, json.dumps(props, sort_keys=True), target_name) in provider_side:
                    link, side = (kind, other, remote, service, name, path), "provider"
                else:
                    link, side = (kind, service, name, other, remote, path), "caller"
                links.setdefault(link, {})[side] = props
    edges = set()
    for (kind, consumer, _, provider, _, _), copies in links.items():
        props = dict(copies.get("caller", {}))
        for name, value in copies.get("provider", {}).items():
            if not props.get(name):
                props[name] = value
        if kind == "http" and copies.get("provider", {}).get("transport"):
            props["transport"] = copies["provider"]["transport"]
        key = key_of(kind, props)
        if key is None:
            print(f"{kind} link {consumer} -> {provider} has no key: {copies}", file=sys.stderr)
            continue
        if consumer != provider:
            edges.add((kind, consumer, provider, key))
    for kind, consumer, provider, key in sorted(edges):
        print(json.dumps({"system": system, "kind": kind, "consumer": consumer, "provider": provider, "contract": key}))


if __name__ == "__main__":
    main()
