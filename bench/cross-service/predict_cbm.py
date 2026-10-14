"""codebase-memory-mcp predictions: the CROSS_* edges `index_repository --mode
cross-repo-intelligence` stored for every service of a system.

    python3 predict_cbm.py <system> <cbm binary> <service> ...      (JSON lines on stdout)

Run with the same HOME the indexing used. Projects are named `<system>-<service>`. A cross edge
lives in the calling project; its properties name the `target_project`. HTTP edges carry
`transport` (the method) and `url_path`. The other cross kinds (gRPC, async, channel) have
no mapping here because no run of this benchmark has produced one: such an edge stops the run
with its properties printed, so the mapping is written from a real example, not guessed.
"""
import json
import re
import subprocess
import sys

KINDS = {"CROSS_HTTP_CALLS": "http", "CROSS_GRPC_CALLS": "grpc", "CROSS_ASYNC_CALLS": "topic", "CROSS_CHANNEL": "topic"}
ROW = re.compile(r'^\s+.*?"(\{.*\})"')


def query(cbm, project, edge_type):
    args = json.dumps({"project": project, "query": f"MATCH (a)-[r:{edge_type}]->(b) RETURN a, r, b LIMIT 100000"})
    out = subprocess.run([cbm, "cli", "--quiet", "--json", "query_graph", args], check=True, capture_output=True,
                         text=True).stdout
    text = json.loads(out)["content"][0]["text"]
    rows = []
    for line in text.splitlines():
        match = ROW.match(line)
        if match:
            rows.append(json.loads(match.group(1).encode().decode("unicode_escape")))
    return rows


def main():
    system, cbm, services = sys.argv[1], sys.argv[2], sys.argv[3:]
    prefix = system + "-"
    rows = set()
    for service in services:
        for edge_type, kind in KINDS.items():
            for props in query(cbm, prefix + service, edge_type):
                target = props.get("target_project", "")
                if not target.startswith(prefix):
                    continue
                provider = target[len(prefix):]
                if kind != "http":
                    sys.exit(f"{edge_type} edge from {service} has no mapping yet: {props}")
                key = f"{props.get('transport', '')} {props.get('url_path', '')}"
                if provider != service:
                    rows.add((kind, service, provider, key))
    for kind, consumer, provider, key in sorted(rows):
        print(json.dumps({"system": system, "kind": kind, "consumer": consumer, "provider": provider, "contract": key}))


if __name__ == "__main__":
    main()
