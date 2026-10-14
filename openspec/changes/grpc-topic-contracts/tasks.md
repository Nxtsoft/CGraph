## 1. Foundation

- [x] 1.1 `contracts.hpp/.cpp`: `grpc` and `topic` contract kinds, bridged by name; docs.
- [x] 1.2 `resolve_contracts` step 8 takes `grpc_server_contract_facts(raw_relations)`: Go registrations joined with their handlers per directory.
- [x] 1.3 Seam summary line, README and the kind lists in `workspace.hpp`, `change_context.hpp`, `seam.hpp`.
- [x] 1.4 Index version `logic-24`.

## 2. Extraction

- [x] 2.1 `messaging_contracts.hpp/.cpp`: per-file binding index (constants, env defaults, client bindings, declared types, Python servicer classes, C# using aliases), built once per file.
- [x] 2.2 gRPC servers: Java `ImplBase`, C# `<S>Base`, Python `Servicer` (and same-file subclasses), C++ `<S>::Service`, Node `addService`, Go `Register<S>Server` + handler methods (unary shape, registration's package) and streaming handlers.
- [x] 2.3 gRPC clients: Go, Java, Python, C#, TypeScript/JavaScript; inline or bound in the file; stub machinery methods skipped; Go calls must take a context first and only `grpc.X(...)` options after the request; Go client types never pointers.
- [x] 2.4 Topics: Kafka (Spring, kafka-clients, Kafka Streams, Confluent .NET, sarama, kafka-go, confluent-kafka-go, kafka-python, aiokafka, confluent-kafka, kafkajs), RabbitMQ on the default exchange (Spring AMQP, RabbitMQ.Client, amqp091, pika, amqplib), SQS, SNS, Pub/Sub; `amq.*` refused.
- [x] 2.5 Hooks in the Go, Java, Kotlin, C# (new extra walk), C++, Python and JavaScript/TypeScript walks; `src/engine/CMakeLists.txt`.

## 3. Verification

- [x] 3.1 `messaging_contracts_test.cpp` (new), `contracts_test` `test_messaging_contract_facts`, `seam_test` `test_messaging_contracts`.
- [x] 3.2 Full suite.
- [x] 3.3 Over-link diff: the four private probe repos give identical graphs (node and edge sets) and an identical seam to bin-v0.9.1; jaegertracing/jaeger, dotnet/eShop and rabbitmq/rabbitmq-tutorials: every new grpc/topic fact listed and checked at source; two Go look-alike client shapes found and guarded.
- [x] 3.4 Public benchmark (`bench/cross-service/run.sh`): gRPC 36/36 at precision 1.00, topics 7/9 at precision 1.00; results in `bench/cross-service/RESULTS.md`.

## 4. Review round 1

- [x] 4.1 Go clients need a generated-stub import path (go-redis `NewClusterClient` / `UniversalClient` refused), Python stubs a grpc import, JavaScript `<S>Client` an import from a relative or generated path.
- [x] 4.2 The per-file index lives in `MessagingContractsFileScope`, held by every extractor that calls a handler, instead of a `thread_local` keyed on the buffer's address and ends (stale after an in-place edit of the same size); the tree is collected with an explicit stack.
- [x] 4.3 A name also bound to a non-client type or object is ambiguous; a parameter shadows a same-named file binding; a Go type registered for two services provides nothing.
- [x] 4.4 `grpc_server` / `grpc_handler` added to graph_builder's raw-relation skip list.
- [x] 4.5 Header: C++ servers only when the override is defined in the class body; file-level source for any Python / C# call outside a function.
- [x] 4.6 Tests for each: go-redis, two-service registration, a parameter, an S3 client in a grpc file, an ambiguous C# name, the same buffer edited in place, a 20000-term expression.

## 5. Review round 2

- [x] 5.1 Topic names: any non-literal binding of a name in the file (parameters in any form, bare arrow and destructured parameters, loop variables, patterns, rebindings such as `topic := cfg.Topic`) makes it read as nothing; tests for Go, TypeScript and Python shapes.
- [x] 5.2 gRPC client names: any binding that is no gRPC client constructor (another call or constructor, an untyped parameter) makes the name no client; a name bound to another name is a client only when that name is one; tests for Go, Python, TypeScript, Java and C#.
- [x] 5.3 Topic ids carry the broker (`topic:<broker>:<name>`); `predict_cgraph.py` keys topics by name for scoring; a seam test shows a Kafka topic and an SQS queue of one name do not join.
- [x] 5.4 gRPC look-alikes: relative imports alone are no generated stub; `new a.B(x, y)` needs `a` bound to `loadPackageDefinition(...)`; Java `<S>Grpc` must be imported or named in code; Python stubs and servicers need a `*_pb2_grpc` module; C# clients need `Grpc.*` or a `Grpc` namespace, servers a `ServerCallContext` parameter or the same; C++ servers a `ServerContext*` parameter; connect-go packages, channelz and OTLP services excluded.
- [x] 5.5 Broker receivers: kafkajs on `.producer()` / `.consumer()`, amqplib on `.createChannel()`, Python Kafka consumers and pika channels, C# `IConsumer` / `IProducer` / channels, Go amqp `Channel` and sarama consumers; Java receivers must be declared only as the broker client.
- [x] 5.6 Spec MODIFIED scenarios use neutral names.
