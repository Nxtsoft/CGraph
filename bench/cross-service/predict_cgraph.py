"""CGraph predictions: the cross-service edges in one `cgraph seam discover` output.

    python3 predict_cgraph.py <system> <seam-out-dir>      (JSON lines on stdout)

A contract node with SERVED_BY edges is provided by those services; a CONSUMES edge from a
service node is a use. Every (consumer, provider) pair of different services is one edge. A topic
id is `topic:<broker>:<name>`; its key here is the name, as the ground truth spells it.
"""
import collections
import json
import sys
from pathlib import Path

KIND = {"endpoint": "http"}  # every other contract kind keeps its own name


def main():
    system, seam_dir = sys.argv[1], Path(sys.argv[2])
    providers = collections.defaultdict(set)
    consumers = collections.defaultdict(set)
    for chunk in sorted(seam_dir.glob("*.json")):
        for edge in json.loads(chunk.read_text())["edges"]:
            if edge["relation"] == "SERVED_BY":
                providers[edge["source"]].add(edge["target"].split(":", 1)[1])
            elif edge["relation"] == "CONSUMES" and edge["source"].startswith("service:"):
                consumers[edge["target"]].add(edge["source"].split(":", 1)[1])
    rows = set()
    for contract, served_by in providers.items():
        kind, key = contract.split(":", 1)
        if kind == "topic":
            key = key.split(":", 1)[1]  # `topic:<broker>:<name>`: the ground truth keys topics by name
        for consumer in consumers.get(contract, ()):
            for provider in served_by:
                if consumer != provider:
                    rows.add((KIND.get(kind, kind), consumer, provider, key))
    for kind, consumer, provider, key in sorted(rows):
        print(json.dumps({"system": system, "kind": kind, "consumer": consumer, "provider": provider,
                          "contract": key}))


if __name__ == "__main__":
    main()
