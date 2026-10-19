"""Score cross-service edge predictions against the hand-labelled ground truth.

    python3 score.py ground_truth.jsonl <tool>=<predictions.jsonl> ... [--json out.json]

Every line is {"system", "kind", "consumer", "provider", "contract"}. Two levels are scored:

  contract   (system, kind, consumer, provider, contract): the exact operation
  service    (system, kind, consumer, provider): which service talks to which, over what

Contract keys are compared in one canonical form: an HTTP key is `<METHOD> <path>` with every
parameter segment (`{id}`, `:id`, `<id>`, `[id]`, `{param}`) as `{}`, no query string and no
trailing slash; a gRPC key is `<Service>/<Method>` without the proto package, method first
letter upper-cased (`hipstershop.CartService/getCart` is `CartService/GetCart`); a topic key is
the name as written. Kinds outside the ground truth (tables, headers, ...) are counted but not
scored. Precision is n/a when a tool predicts nothing of a kind.
"""
import collections
import json
import re
import sys

KINDS = ("grpc", "http", "topic")
PARAM = re.compile(r"^(\{[^}]*\}|:[^/]+|<[^>]+>|\[[^\]]+\])$")


def canonical(kind, key):
    if kind == "http":
        method, _, path = key.partition(" ")
        path = path.split("?", 1)[0]
        segments = ["{}" if PARAM.match(s) else s for s in path.split("/") if s]
        return f"{method.upper()} /" + "/".join(segments)
    if kind == "grpc":
        service, _, method = key.partition("/")
        service = service.rsplit(".", 1)[-1]
        return f"{service}/{method[:1].upper()}{method[1:]}"
    return key


def load(path):
    rows = set()
    with open(path) as handle:
        for line in handle:
            if line.strip():
                r = json.loads(line)
                rows.add((r["system"], r["kind"], r["consumer"], r["provider"], canonical(r["kind"], r["contract"])))
    return rows


def prf(predicted, truth):
    tp = len(predicted & truth)
    precision = tp / len(predicted) if predicted else None
    recall = tp / len(truth) if truth else None
    return {"tp": tp, "predicted": len(predicted), "truth": len(truth), "precision": precision, "recall": recall,
            "false_positives": sorted(predicted - truth), "missed": sorted(truth - predicted)}


def fmt(x):
    return "n/a" if x is None else f"{x:.2f}"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--json")]
    out_json = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--json=")), None)
    truth = load(args[0])
    tools = [(a.split("=", 1)[0], load(a.split("=", 1)[1])) for a in args[1:]]
    systems = sorted({t[0] for t in truth})
    report = {}
    for level in ("contract", "service"):
        project = (lambda r: r) if level == "contract" else (lambda r: r[:4])
        truth_level = {project(r) for r in truth}
        print(f"\n## {level} level\n")
        print("| tool | kind | truth | predicted | correct | precision | recall |")
        print("|---|---|---:|---:|---:|---:|---:|")
        for name, predictions in tools:
            pred_level = {project(r) for r in predictions}
            for kind in KINDS + ("all",):
                keep = (lambda r: r[1] in KINDS) if kind == "all" else (lambda r, k=kind: r[1] == k)
                score = prf({r for r in pred_level if keep(r)}, {r for r in truth_level if keep(r)})
                report.setdefault(level, {}).setdefault(name, {})[kind] = score
                print(f"| {name} | {kind} | {score['truth']} | {score['predicted']} | {score['tp']} | "
                      f"{fmt(score['precision'])} | {fmt(score['recall'])} |")
        if level == "contract":
            print("\nPer system (all scored kinds):\n")
            print("| tool | " + " | ".join(systems) + " |")
            print("|---|" + "---|" * len(systems))
            for name, predictions in tools:
                cells = []
                for system in systems:
                    score = prf({r for r in predictions if r[0] == system and r[1] in KINDS},
                                {r for r in truth if r[0] == system})
                    cells.append(f"{score['tp']}/{score['truth']} (P {fmt(score['precision'])})")
                print(f"| {name} | " + " | ".join(cells) + " |")
    print("\nPredictions in kinds the ground truth does not cover (not scored):\n")
    for name, predictions in tools:
        other = collections.Counter(r[1] for r in predictions if r[1] not in KINDS)
        print(f"- {name}: " + (", ".join(f"{k} {v}" for k, v in sorted(other.items())) or "none"))
    if out_json:
        with open(out_json, "w") as handle:
            json.dump(report, handle, indent=1, default=list)


if __name__ == "__main__":
    main()
