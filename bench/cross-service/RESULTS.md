# Cross-service benchmark: CGraph vs GitNexus vs codebase-memory-mcp

How well does each tool find the edges between services: which service calls which gRPC
method, HTTP route or message topic of which other service? The corpus is five open-source
microservice systems, pinned to commits, with every cross-service edge labelled by hand.

## Corpus

Each service is checked out at its pinned commit and copied into its own git repository
(`materialize.py`), so a monorepo service and a service with its own upstream repository look
the same to every tool. 44 service repositories in all.

| System | Upstream (commit) | Services | gRPC | HTTP | Topic |
|---|---|---:|---:|---:|---:|
| boutique | GoogleCloudPlatform/microservices-demo (`38e7348`) | 13 | 20 | 9 | 0 |
| otel-demo | open-telemetry/opentelemetry-demo (`ab6d01d`) | 16 | 16 | 13 | 2 |
| sock-shop | microservices-demo/{front-end,orders,shipping,queue-master,carts,catalogue,payment,user} (8 repos, `systems.json`) | 8 | 0 | 35 | 1 |
| kafka-saga | piomin/sample-spring-kafka-microservices (`a76daeb`) | 4 | 0 | 0 | 4 |
| kafka-orders | ewolff/microservice-kafka (`103de22`) | 3 | 0 | 0 | 2 |
| **all** | | **44** | **36** | **57** | **9** |

Languages: Go, Java, Kotlin, C#, C++, Python, TypeScript/JavaScript, Rust, Ruby, PHP.

`ground_truth.jsonl` has one line per edge: system, kind, consumer service, provider service,
contract key, and a `file:line` quote for one call site and for the provider. The rules
(written down before any tool was run on the corpus):

- An edge joins two different services of one system. Production code only: no tests, no
  test clients a service ships for itself, no infrastructure (collector, flagd, brokers,
  databases) and no third-party SaaS. In otel-demo only the 16 services listed are in scope.
- `grpc`: key `<Service>/<Method>` without the proto package; the provider is the service that
  implements and registers it (not every service that vendors the `.proto`).
- `http`: key `<METHOD> <full path>` with parameters as `{}`; addresses resolved through the
  repos' manifests (`kubernetes-manifests/`, `compose.yaml`, `.env`, Envoy config).
- `topic`: key is the topic or queue name; provider = producer, consumer = subscriber; an env
  variable with a literal default counts as the literal when the manifests do not override it.

Each system was labelled by one reader working from source only, without looking at any tool's
output. A second reader then re-checked every row (quote on the cited line, address resolution,
key form) and grepped every service for missed calls: 0 wrong rows, 0 missing edges; one
evidence line was replaced (kafka-saga, the first `orders` producer in file order).

## How to run

```sh
bench/cross-service/run.sh --cgraph-bin-dir <dir with cgraph> [--work DIR] [--skip-baselines]
```

It materializes the corpus, runs CGraph (`cgraph --root` per service, `cgraph seam discover`
per system, `predict_cgraph.py`), installs GitNexus 1.6.12 into `<work>/tools` with
`npm install --prefix ... --ignore-scripts` and runs it with HOME and storage under the work
dir (`analyze --index-only` per service, `group create/add/sync` per system;
`predict_gitnexus.py` reads the group's `contracts.json` cross-links), downloads
codebase-memory-mcp 0.11.0 (sha256-pinned) and runs it with HOME under the work dir
(`index_repository` per service, then `--mode cross-repo-intelligence` with the system's other
services as targets; `predict_cbm.py` reads the stored `CROSS_*` edges), and scores with
`score.py`. Nothing is installed globally and no tool's `setup` is run. A full run took
33 min 44 s on one machine (most of it GitNexus indexing and codebase-memory-mcp's per-call
daemon start).

Scoring (`score.py`): precision and recall per kind at two levels. **contract**: the exact
(consumer, provider, kind, key). **service**: (consumer, provider, kind), which credits a tool
that links the right two services without naming the operation. Keys are compared in one
canonical form (HTTP parameters `{}`, no query or trailing slash; gRPC package dropped, method
first letter upper-cased). Precision is n/a when a tool predicts nothing of a kind.

## Results: CGraph bin-v0.9.1

`cgraph --version`: `cgraph-native 0.1.0 (d761f0c)` (the bin-v0.9.1 release binary).
GitNexus `1.6.12`, codebase-memory-mcp `0.11.0`.

Contract level:

| Tool | Kind | Truth | Predicted | Correct | Precision | Recall |
|---|---|---:|---:|---:|---:|---:|
| CGraph | grpc | 36 | 0 | 0 | n/a | 0.00 |
| CGraph | http | 57 | 0 | 0 | n/a | 0.00 |
| CGraph | topic | 9 | 0 | 0 | n/a | 0.00 |
| GitNexus | grpc | 36 | 80 | 0 | 0.00 | 0.00 |
| GitNexus | http | 57 | 7 | 1 | 0.14 | 0.02 |
| GitNexus | topic | 9 | 6 | 2 | 0.33 | 0.22 |
| codebase-memory-mcp | grpc | 36 | 0 | 0 | n/a | 0.00 |
| codebase-memory-mcp | http | 57 | 0 | 0 | n/a | 0.00 |
| codebase-memory-mcp | topic | 9 | 0 | 0 | n/a | 0.00 |

Service level:

| Tool | Kind | Truth | Predicted | Correct | Precision | Recall |
|---|---|---:|---:|---:|---:|---:|
| CGraph | all kinds | 49 | 0 | 0 | n/a | 0.00 |
| GitNexus | grpc | 25 | 22 | 7 | 0.32 | 0.28 |
| GitNexus | http | 15 | 7 | 1 | 0.14 | 0.07 |
| GitNexus | topic | 9 | 6 | 2 | 0.33 | 0.22 |
| codebase-memory-mcp | all kinds | 49 | 0 | 0 | n/a | 0.00 |

What each tool did:

- **CGraph bin-v0.9.1 finds no cross-service edge here.** It has no gRPC or topic contract
  kind. On HTTP it does extract some sides (Spring routes in sock-shop's carts and orders, Go
  client calls in otel-demo's checkout: `POST /get-quote`, `POST /ship-order`,
  `POST /send_order_confirmation`), but never both sides of one edge: it reads no Go
  `net/http` / gorilla/mux routes, no Next.js pages-router API routes, no Rust actix-web, Ruby
  Sinatra or PHP Slim routes, no locust clients, and no JavaScript `request` calls whose host
  comes from another file.
- **GitNexus** links gRPC at service level only (`grpc::CartService/*`, all 80 links are
  wildcards), and its provider is whichever repository holds a copy of the `.proto`: on
  boutique every client is linked to `adservice`, `currencyservice`, `paymentservice` and
  `protos` (16 links each), never to the service that implements the method. On kafka-orders it
  links all three services to each other over topic `order` and `GET /`: the Kafka tests'
  senders and listeners under `src/test/` count as producers and consumers
  (`invoicing/.../InvoiceKafkaTest.java`, `shipping/.../ShipmentKafkaTest.java`), and each
  service's Thymeleaf `layout.html` link to `/` is matched to every other service's `GET /`. Its one correct HTTP edge is sock-shop `front-end -> orders POST /orders`.
- **codebase-memory-mcp** reported `"total_cross_edges":0` for all 44 services in
  cross-repo-intelligence mode. Its graphs hold the protos' rpcs as `Route` nodes, but no client
  call becomes an HTTP, gRPC or async edge on this corpus.

Raw outputs of a run: `<work>/predictions/*.jsonl`, `<work>/results.md`, `<work>/results.json`
(with every false positive and miss per tool).

## Results: CGraph with gRPC and topic contracts (CGR-28 part B)

`cgraph --version`: `cgraph-native 0.1.0 (bin-v0.9.1-9-g8c7322f)`, run with
`run.sh --skip-baselines`; GitNexus and codebase-memory-mcp rows are the bin-v0.9.1 run above
(their output does not depend on CGraph; same pinned corpus and tool versions).

Contract level:

| Kind | CGraph P / R | GitNexus P / R | codebase-memory-mcp P / R | CGraph beats both on P and R? |
|---|---|---|---|---|
| gRPC (36) | **1.00 / 1.00** (36 of 36) | 0.00 / 0.00 (0 of 80) | n/a / 0.00 (0 predicted) | yes |
| topic (9) | **1.00 / 0.78** (7 of 7) | 0.33 / 0.22 (2 of 6) | n/a / 0.00 | yes |
| HTTP (57) | n/a / 0.00 (0 predicted) | 0.14 / 0.02 (1 of 7) | n/a / 0.00 | **no**: GitNexus has 1 edge, CGraph 0 |
| all (102) | 1.00 / 0.42 (43 of 43) | 0.03 / 0.03 (3 of 93) | n/a / 0.00 | |

Service level: gRPC 25 of 25 pairs (P 1.00), topic 7 of 7 (R 0.78); GitNexus 0.32 / 0.28 and
0.33 / 0.22; codebase-memory-mcp 0 / 0.

Per system: boutique 20/29, otel-demo 16/31, sock-shop 1/36, kafka-saga 4/4, kafka-orders 2/2,
every predicted edge correct.

Topic contracts are `topic:<broker>:<name>`; `predict_cgraph.py` keys them by name, as the ground
truth does. Review round 2 (broker ids, stricter name resolution and client recognition) left these
numbers unchanged.

Misses:

- **Topic, 2:** otel-demo `checkout` produces `orders` through `Topic: kafka.Topic`, a package
  variable of another package set by `getTopic()` (env read or `"orders"`); a name returned by a
  function in another package is not read. Its consumers (`accounting`, `fraud-detection`) are
  found.
- **HTTP, 57:** CGraph still reads no Go `net/http` / gorilla/mux routes, Next.js pages-router API
  routes, Rust actix-web, Ruby Sinatra or PHP Slim routes, no locust clients, and no JavaScript
  `request` calls whose host comes from another file, so no HTTP edge here has both sides. This
  is the gate CGR-28 does not meet; it needs route coverage, not contract kinds.

Caveats:

- This corpus was used while writing the gRPC and topic extraction, so it is not a held-out test.
  Out-of-corpus checks run on jaegertracing/jaeger (`6ea438c`), dotnet/eShop (`dc7ea49`) and
  rabbitmq/rabbitmq-tutorials (`88b985d`): every gRPC and topic fact listed and read at its
  source line. They found the look-alike Go clients (`tracing.NewHTTPClient`,
  `*esclient.ILMClient`, otlp's `NewGRPCClient`) that the generated-stub and call-shape guards now
  refuse; eShop's WebApp -> Basket.API gRPC calls (3 methods) and the tutorials' queues
  (`hello`, `task_queue`, `rpc_queue` across Python, Node, Go, .NET, Spring) are found.
- Facts with no counterpart do not make edges but are in the graphs: Python servicers in
  boutique also define the health methods `Check` / `Watch` in the service class, so
  `EmailService/Check`, `EmailService/Watch`, `RecommendationService/Check` and
  `RecommendationService/Watch` are provided (no client calls them).
