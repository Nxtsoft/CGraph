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
