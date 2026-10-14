#pragma once

// gRPC methods and message topics as contracts (contracts.hpp `grpc:` and
// `topic:`), CGR-28. Both cross repositories by name, like a DynamoDB table:
// a gRPC method's whole address is its service and method name, a topic's is
// its broker and its name there.
//
// gRPC: `grpc:<Service>/<Method>`, the proto service name without its package
// (code calling a generated stub rarely spells the package: Go's
// `pb.NewCartServiceClient`, Python's `demo_pb2_grpc.CartServiceStub`) and the
// method as the proto spells it. Stubs in Java and JavaScript spell a method
// lowerCamel (`getCart`), C# adds an `Async` twin (`GetCartAsync`); both are
// read back to the proto spelling (`GetCart`): first letter upper-cased,
// `Async` dropped. Caveat: two packages each with a `CartService` are one
// service here. Services every process serves or calls for its own reasons are
// never recorded (is_standard_grpc_service: gRPC's `Health`,
// `ServerReflection`, `Channelz`; OTLP's `TraceService`, `MetricsService`,
// `LogsService`, `ProfilesService`).
//
//   provides  a method of a class that implements a generated server base:
//             Java `extends <S>Grpc.<S>ImplBase` (a method taking a
//             `StreamObserver<...>`); C# `: <...>.<S>.<S>Base` (an `override`
//             method taking a `ServerCallContext`, or in a file using `Grpc.*`,
//             or with a base spelled through a `Grpc` namespace); Python `(<m>.<S>Servicer)` with `<m>` a
//             `*_pb2_grpc` module, or `(<S>Servicer)` imported from one, or a
//             same-file subclass of such a class (a method
//             `(self, request, context)`); C++ `: public <ns>::<S>::Service` (an
//             `override` method defined in the class body taking a
//             `ServerContext*`); JavaScript/TypeScript
//             `server.addService(<x>.<S>.service, {m1, m2})` or
//             `addService(<S>Service, {...})` (each key of the object); Go: a
//             method of the type registered with `<pkg>.Register<S>Server(s, impl)`
//             (`impl` a `&T{}`, `T{}`, `new(T)`, or a local assigned one in the
//             same function) with the unary handler shape
//             `(context.Context, *<pkg>.Req) (*<pkg>.Resp, error)`, both message
//             types from the registration's package, anywhere in the same
//             directory (one Go package). Extraction records `grpc_server`
//             (S, T, package alias) and `grpc_handler` (method, T, name,
//             package alias) facts; grpc_server_contract_facts joins them per
//             directory; a type registered for two services provides nothing
//             (which handler serves which is in the .proto, not the code). A
//             streaming handler `(..., <pkg>.<S>_<M>Server) error` names its
//             service and method itself and provides directly.
//   uses      a method call on a generated client: Go `<pkg>.New<S>Client(c)`
//             with one argument and `<pkg>` imported from a generated-stub path
//             (a segment naming proto or grpc, or a `pb` package: `genproto`,
//             `proto-gen/api_v3`, `../protos/demo`; never a connect-go
//             `*connect` package), the call taking a context first, then the
//             request, then only `grpc.X(...)` options; Java
//             `<S>Grpc.new[Blocking|Future]Stub(c)` (and `.withX(...)` chained
//             on it); Python `<m>.<S>Stub(c)` off a `*_pb2_grpc` module or a
//             `<S>Stub` imported from one; C# `new <...>.<S>.<S>Client(c)` in a
//             file using `Grpc.*` or spelled through a `Grpc` namespace;
//             JavaScript/TypeScript, in a file importing `@grpc/grpc-js` or
//             `grpc`, `new <S>Client(...)` with `<S>Client` imported from a
//             generated-stub path, or `new <root>.<...>.<S>(address, credentials)`
//             with `<root>` bound to a `grpc.loadPackageDefinition(...)` result.
//             Called inline or through a name bound to one anywhere in the
//             file: a local, a field or property (`cs.cartSvcClient`,
//             `self.stub`, `this.client`) assigned one or another name bound
//             to one, or declared with the client type (Go `<pkg>.<S>Client`
//             from a generated-stub path, never a pointer: a generated client
//             is an interface; Java `<S>[Blocking|Future]Stub` in a file that
//             imports or names `<S>Grpc` in code; C# `<S>.<S>Client` or a
//             `using A = ...<S>.<S>Client;` alias of it, under the same `Grpc`
//             condition; TypeScript `<S>Client` imported as above). A name bound
//             to two services, or with any other binding in the file (another
//             type, another constructor or call: `client :=
//             dynamodb.NewFromConfig(cfg)` beside `client :=
//             pb.NewCartServiceClient(conn)`; an untyped parameter), records
//             nothing. Methods of the stub machinery itself are not RPCs (Java
//             `withDeadlineAfter`, `getChannel`; JavaScript `close`,
//             `waitForReady`, `makeUnaryRequest`, ...; Python `.future` /
//             `.with_call` read through to the method).
//
// Topics: `topic:<broker>:<name>`, broker one of `kafka`, `rabbitmq` (a queue
// on the default exchange), `sqs`, `sns`, `pubsub`, `jms`, and the name as the
// code spells it, so a Kafka topic and an SQS queue of one name never meet.
// The PRODUCER provides it, a CONSUMER uses it, so the seam's provider is the
// service that publishes and its consumers the ones that subscribe. A name is
// read as: a string literal; an env read with a literal default
// (`os.getenv("X", "orders")`, `process.env.X || 'orders'`,
// `Environment.GetEnvironmentVariable("X") ?? "orders"`,
// `System.getenv("X") ?: "orders"`, `System.getenv().getOrDefault("X", "orders")`):
// the default; or a name every binding of which in the file is one such
// literal (a constant, a field, `this.x` / `self.x`). Any other binding of the
// name anywhere in the file (a parameter in any form, destructured or a bare
// arrow parameter; a loop variable; a destructuring pattern; a rebinding to
// another name or expression, `topic := cfg.Topic`) makes it read as nothing.
// An SQS queue URL (`https://sqs.<region>.amazonaws.com/<account>/<name>`)
// reads as its last segment, an SNS topic ARN as its last `:` segment. A name
// in RabbitMQ's reserved `amq.` space (`amq.rabbitmq.reply-to`) is the
// broker's own and never recorded. Anything else (a config property, a
// placeholder `${...}`, an exchange with a routing key) records nothing.
//
// The receiver of a broker call must be that broker's client: a name whose
// every declared type is one of the listed types, or (declared with none)
// that is assigned from one of the listed constructors or calls. A name the
// file also declares as something else (`KafkaTemplate template` and a
// `JmsTemplate template` parameter) is not one.
//
//   produce   Java `<KafkaTemplate|ReactiveKafkaProducerTemplate>.send(t, ...)`,
//             `new ProducerRecord<>(t, ...)` (kafka), `<SqsTemplate>.send(q, ...)`
//             (sqs), `<RabbitTemplate|AmqpTemplate|RabbitMessagingTemplate>
//             .convertAndSend(q, msg)` / `.send(q, msg)` (two arguments: the
//             default exchange; rabbitmq); Kotlin `ProducerRecord(t, ...)` with
//             `org.apache.kafka` imported; C# (Confluent.Kafka) `.Produce(t, ...)` /
//             `.ProduceAsync(t, ...)` on an `IProducer`, (RabbitMQ.Client)
//             `.BasicPublish[Async]("" or string.Empty, q, ...)` on an
//             `IModel` / `IChannel` (or one from `CreateModel` /
//             `CreateChannel[Async]`); Go `sarama.ProducerMessage{Topic: t}`,
//             kafka-go `kafka.Message{Topic: t}` / `kafka.Writer{Topic: t}` /
//             `kafka.WriterConfig{Topic: t}`, amqp `.Publish("", q, ...)` /
//             `.PublishWithContext(ctx, "", q, ...)` on a `Channel`; Python
//             (kafka-python, aiokafka, confluent-kafka) `.send(t, ...)` /
//             `.send_and_wait(t, ...)` / `.produce(t, ...)` on a name assigned
//             from `KafkaProducer` / `AIOKafkaProducer` / `Producer` /
//             `SerializingProducer`, pika `.basic_publish(exchange="", routing_key=q)`
//             on a name assigned from `.channel()`, boto3
//             `.send_message(QueueUrl=u)` (sqs), `.publish(TopicArn=a)` (sns),
//             Pub/Sub `.publish(<client>.topic_path(p, t), ...)`;
//             JavaScript/TypeScript kafkajs `.send({ topic: t })` on a name
//             assigned from `.producer()`, amqplib `.sendToQueue(q, ...)` /
//             `.publish('', q, ...)` on a name assigned from `.createChannel()` /
//             `.createConfirmChannel()`, AWS SDK
//             `new SendMessageCommand({ QueueUrl: u })` (sqs) /
//             `new PublishCommand({ TopicArn: a })` (sns), Pub/Sub
//             `.topic(t).publish*(...)`.
//   consume   Java `@KafkaListener(topics = ...)`, `@RabbitListener(queues = ...)`,
//             `@SqsListener(...)`, `@JmsListener(destination = ...)`, a
//             `KafkaConsumer.subscribe(List.of(t))` (and `Arrays.asList`,
//             `Collections.singletonList`, `Set.of`),
//             `<StreamsBuilder>.stream(t)` / `.table(t)` / `.globalTable(t)`,
//             `.setQueueNames(q, ...)` with `org.springframework.amqp` imported;
//             Kotlin `subscribe(listOf(t))` with `org.apache.kafka` imported;
//             C# `.Subscribe(t)` on an `IConsumer` (Confluent.Kafka),
//             `.BasicConsume[Async](q, ...)` on a channel (RabbitMQ.Client); Go
//             kafka-go `kafka.ReaderConfig{Topic: t}`, sarama
//             `.ConsumePartition(t, ...)` on a `Consumer` and
//             `.Consume(ctx, []string{t}, h)` on a `ConsumerGroup`, confluent
//             `.SubscribeTopics([]string{t}, ...)` on a `Consumer`, amqp
//             `.Consume(q, ...)` on a `Channel`; Python `KafkaConsumer(t, ...)` /
//             `AIOKafkaConsumer(t, ...)`, `.subscribe([t])` on a Kafka consumer,
//             pika `.basic_consume(queue=q)` on a channel, boto3
//             `.receive_message(QueueUrl=u)`, Pub/Sub
//             `.create_subscription(topic=<client>.topic_path(p, t))`;
//             JavaScript/TypeScript kafkajs `.subscribe({ topic: t })` /
//             `({ topics: [...] })` on a name assigned from `.consumer(...)`,
//             amqplib `.consume(q, ...)` on a channel, AWS SDK
//             `new ReceiveMessageCommand({ QueueUrl: u })`, Pub/Sub
//             `.topic(t).subscription(...)` / `.createSubscription(...)`.
//
// The library must be in the file too: Java, Kotlin, C# and Go check the
// imports where the type alone is not specific (`org.apache.kafka`,
// `org.springframework.amqp`, `Confluent.Kafka`, `RabbitMQ.Client`, `sarama`,
// `kafka-go`, `confluent-kafka-go`, `amqp`); Python and JavaScript the imports
// (`kafka`, `aiokafka`, `confluent_kafka`, `pika`, `boto3`, `pubsub`;
// `kafkajs`, `amqplib`, `amqp-connection-manager`, `@aws-sdk/client-sqs`,
// `@aws-sdk/client-sns`, `@google-cloud/pubsub`). A topic name is
// `[A-Za-z0-9._-]`, 1 to 249 characters.
//
// Every fact's source is the innermost enclosing function; outside any
// function it is, in JavaScript, js_syntax::reading_scope_id, in Python and C#
// the file (a script's or `Program.cs`'s top-level statement, a field
// initializer, a class-body call); in the other languages such a call
// records nothing. A test source (data_contracts.hpp is_test_source_path)
// records nothing: its producers and consumers stand in for other services.

#include "cgraph/language_config.hpp"

#include <tree_sitter/api.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// Held while one file is extracted (every extractor that calls a handler
// below holds one): the file's constants, client bindings, imports and
// servicer classes are read once, on the first call that needs them, and
// dropped with the scope. Scopes nest; each covers one file on its thread.
// Without one, each call reads the file afresh.
class MessagingContractsFileScope {
 public:
  MessagingContractsFileScope();
  ~MessagingContractsFileScope();
  MessagingContractsFileScope(const MessagingContractsFileScope&) = delete;
  MessagingContractsFileScope& operator=(const MessagingContractsFileScope&) = delete;

 private:
  struct Index;
  std::unique_ptr<Index> index_;
};

// The raw relations Go server registration leaves for resolve_contracts.
inline constexpr std::string_view kGrpcServerRelation = "grpc_server";
inline constexpr std::string_view kGrpcHandlerRelation = "grpc_handler";

// True for a gRPC service every process serves or calls for its own reasons
// (`Health`, `ServerReflection`, `Channelz`, OTLP's `TraceService`,
// `MetricsService`, `LogsService`, `ProfilesService`): never recorded.
[[nodiscard]] bool is_standard_grpc_service(std::string_view service);

// The proto spelling of a method a stub spells `name`: first letter
// upper-cased (`getCart` is `GetCart`).
[[nodiscard]] std::string grpc_method_name(std::string_view name);

// True for a topic name: 1 to 249 of `[A-Za-z0-9._-]`.
[[nodiscard]] bool is_topic_name(std::string_view name);

// The name an SQS queue URL or SNS topic ARN addresses: the URL's last path
// segment, the ARN's last `:` segment; any other text as it is.
[[nodiscard]] std::string topic_name_from_address(std::string_view address);

// One handler per grammar, called from that language's extra_walk on every
// node with the innermost enclosing function as `function_scope_id`.
void go_messaging_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                            std::vector<RawRelation>& out);
void java_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                              const std::string& function_scope_id, std::vector<RawRelation>& out);
void kotlin_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                                const std::string& function_scope_id, std::vector<RawRelation>& out);
void csharp_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                                const std::string& function_scope_id, std::vector<RawRelation>& out);
void cpp_messaging_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                             std::vector<RawRelation>& out);
void python_messaging_contracts(const TSNode& node, const ExtractionContext& context,
                                const std::string& function_scope_id, std::vector<RawRelation>& out);
void js_messaging_contracts(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                            const Fragment& fragment, std::vector<RawRelation>& out);

// The `provides_contract grpc:<S>/<M>` facts Go servers make: every
// `grpc_handler` (method of type T, package alias P, name M) in the directory
// of a `grpc_server` (service S, type T, alias P), sourced at the method.
[[nodiscard]] std::vector<RawRelation> grpc_server_contract_facts(std::span<const RawRelation> raw_relations);

}  // namespace cgraph
