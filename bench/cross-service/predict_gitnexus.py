"""GitNexus predictions: the cross-links in one group's contracts.json (`gitnexus group sync`).

    python3 predict_gitnexus.py <system> <contracts.json>      (JSON lines on stdout)

Group members are added as `<system>/<service>`, so a link's `from.repo` is the consumer and
`to.repo` the provider. The contract id is `<type>::<rest>`: `http::GET::/cart/{param}`,
`grpc::hipstershop.CartService/GetCart` (or `grpc::CartService/*` for a wildcard match),
`topic::orders`. Every link is kept as GitNexus reports it, wildcards and all.
"""
import json
import sys


def main():
    system, path = sys.argv[1], sys.argv[2]
    rows = set()
    for link in json.load(open(path))["crossLinks"]:
        consumer = link["from"]["repo"].split("/", 1)[1]
        provider = link["to"]["repo"].split("/", 1)[1]
        if consumer == provider:
            continue
        kind, rest = link["contractId"].split("::", 1)
        if kind == "http":
            method, route = rest.split("::", 1)
            key = f"{method} {route}"
        else:
            key = rest
        rows.add((kind, consumer, provider, key))
    for kind, consumer, provider, key in sorted(rows):
        print(json.dumps({"system": system, "kind": kind, "consumer": consumer, "provider": provider,
                          "contract": key}))


if __name__ == "__main__":
    main()
