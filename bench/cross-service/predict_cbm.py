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
Rows are told apart by that HANDLES edge, so a link stored on both sides counts once and a
provider-side copy is never read as the provider calling its caller.

`query_graph` returns at most 200 rows per call, so every query is paged with SKIP.
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
ROW = re.compile(r'^\s+(.*?) "(\{.*\})"')


def run_query(cbm, project, cypher):
    args = json.dumps({"project": project, "query": cypher})
    out = subprocess.run([cbm, "cli", "--quiet", "--json", "query_graph", args], check=True, capture_output=True,
                         text=True).stdout
    return json.loads(out)["content"][0]["text"]


def rows(cbm, project, match):
    """Every (source name, properties) row of `match`, paged past the 200-row cap."""
    found = []
    skip = 0
    while True:
        text = run_query(cbm, project, f"{match} RETURN a.name, r SKIP {skip} LIMIT {PAGE}")
        page = []
        for line in text.splitlines():
            parsed = ROW.match(line)
            if parsed:
                page.append((parsed.group(1), json.loads(parsed.group(2).encode().decode("unicode_escape"))))
        found.extend(page)
        returned = re.search(r"^returned: (\d+)$", text, re.MULTILINE)
        if returned is None or int(returned.group(1)) < PAGE:
            break
        skip += PAGE
    return found


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
    edges = set()
    for service in services:
        project = prefix + service
        for edge_type, kind in KINDS.items():
            everything = rows(cbm, project, f"MATCH (a)-[r:{edge_type}]->(b)")
            handled = rows(cbm, project, f"MATCH (a)-[r:{edge_type}]->(b) MATCH (a)-[h:HANDLES]->(b)")
            provider_side = {(name, json.dumps(props, sort_keys=True)) for name, props in handled}
            for name, props in everything:
                target = props.get("target_project", "")
                if not target.startswith(prefix):
                    continue
                other = target[len(prefix):]
                key = key_of(kind, props)
                if key is None:
                    print(f"{edge_type} row from {service} has no key: {props}", file=sys.stderr)
                    continue
                if (name, json.dumps(props, sort_keys=True)) in provider_side:
                    consumer, provider = other, service
                else:
                    consumer, provider = service, other
                if consumer != provider:
                    edges.add((kind, consumer, provider, key))
    for kind, consumer, provider, key in sorted(edges):
        print(json.dumps({"system": system, "kind": kind, "consumer": consumer, "provider": provider, "contract": key}))


if __name__ == "__main__":
    main()
