# gRPC methods and message topics as contracts (CGR-28)

## Why

CGraph joins services over HTTP endpoints, SQL tables, graph labels, headers, JWT claims, env names and DynamoDB tables, but not over gRPC or message queues. Rivals cover both (GitNexus and codebase-memory-mcp list gRPC and topics). On the public cross-service benchmark (`bench/cross-service/`, PR A of CGR-28) CGraph bin-v0.9.1 finds 0 of 36 gRPC edges and 0 of 9 topic edges, because neither kind exists: the corpus is Online Boutique, the OpenTelemetry demo, Sock Shop and two Spring Kafka systems.

## What Changes

- Two new contract kinds in the foundation (`contracts.hpp/.cpp`): `grpc:<Service>/<Method>` and `topic:<name>`. Both are bridged by name (`is_bridged_contract`), like `dynamo:`. No new node or edge shape: the facts are `provides_contract` / `uses_contract`.
- New `messaging_contracts.cpp` (+ header): one handler per grammar, hooked into the extra walks of Go, Java, Kotlin, C#, C++, Python and JavaScript/TypeScript (C# gets an extra walk for the first time). It reads:
  - gRPC servers: a method of a class implementing the generated base (Java `<S>ImplBase` taking a `StreamObserver`, C# `<S>.<S>Base` override, Python `<S>Servicer` method `(self, request, context)` including same-file subclasses, C++ `<S>::Service` override), a key of a Node `addService(<x>.<S>.service, {...})`; in Go a handler method of the type registered with `<pkg>.Register<S>Server`, joined per package directory at resolution (`grpc_server` / `grpc_handler` raw relations, `grpc_server_contract_facts`).
  - gRPC clients: a method call on a generated client constructed inline or bound to a local, field or `self`/`this` member in the same file (Go `New<S>Client`, Java `<S>Grpc.new*Stub` / `<S>*Stub` fields, Python `<S>Stub`, C# `<S>.<S>Client` and `using` aliases of it, TypeScript `new <S>Client` with grpc-js imported).
  - Topics: Kafka, RabbitMQ (default exchange), SQS, SNS and Pub/Sub producers (provide) and consumers (use) across those languages; a name is a literal, an env read's literal default, or a same-file constant / field holding one.
- `resolve_contracts` adds the Go server facts to the generic contract step.
- Seam discover's summary line names gRPC methods and topics; README, `workspace.hpp`, `change_context.hpp`, `seam.hpp` kind lists updated.
- Index version `logic-24`.

## Design choices

- **Service without package.** A Go or Python call site never spells the proto package, a C# or Java one rarely; the method's address is service + method. Two packages with one service name meet; recorded as a caveat.
- **Proto spelling of the method.** Generated stubs rename methods (Java/JS `getCart`, C# `GetCartAsync`); the id is the proto name (first letter upper-cased, `Async` dropped), so all clients meet the server.
- **Implementation, not the .proto, provides.** Every service in a polyglot demo vendors the same `demo.proto`; treating a `.proto` as a provider would make each one a server of every service (GitNexus's wildcard matches link clients to `adservice`, `currencyservice`, `paymentservice` and `protos` for every service on Online Boutique). Only code that implements and registers a service provides it. `.proto` rpcs stay documented endpoints as before.
- **Go servers join across files at resolution.** The registration (`main.go`) and the handlers (`product_catalog.go`) are in different files of one package; extraction records both sides and resolution joins them per directory, with the request and response message types from the registration's package (so a `healthpb` `Check` on the same type does not provide `ProductCatalogService/Check`).
- **Guards against look-alike clients.** A Go client is an interface value: `*pkg.XClient` fields and `pkg.XClient{}` literals are other libraries' structs, and a Go RPC call takes a context first and then only `grpc.X(...)` options (jaeger's `tracing.NewHTTPClient(t).GetJSON(ctx, "/x", url, &out)` and `*esclient.ILMClient` calls were recorded before these guards). Java stubs need the file to name `<S>Grpc`; TypeScript clients need a grpc import; C# clients need the generated `<S>.<S>Client` shape.
- **Producer provides a topic.** The seam's provider is the publisher and its consumers the subscribers, matching how the benchmark labels queue edges.
- **Standard services and broker-reserved names are never recorded**: gRPC `Health` / `ServerReflection`, RabbitMQ `amq.*`.

## Non-goals

- Proto package in the id; a declaration to tell two same-named services apart.
- Topic names from config properties (`@Value("${kafka.topic}")`, `application.yml`), from a function's return (`Topic = getTopic()` in the OpenTelemetry demo's checkout, a cross-package variable), or from an exchange plus routing key (RabbitMQ bindings).
- NestJS `@EventPattern` / `@MessagePattern` and Pub/Sub subscriptions named apart from their topic.
- HTTP route coverage gaps the benchmark also shows (Go net/http and gorilla/mux routes, Next.js pages-router API routes, Rust actix-web, Ruby Sinatra, Python locust clients, JavaScript `request` with hosts from another file): HTTP is 0 of 57 on the public corpus and is a separate follow-up.
