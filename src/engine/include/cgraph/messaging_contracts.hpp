#pragma once

// gRPC methods and message topics as contracts (contracts.hpp `grpc:` and
// `topic:`), CGR-28. Both cross repositories by name, like a DynamoDB table:
// a gRPC method's whole address is its service and method name, a topic's is
// its name on the broker.
//
// gRPC: `grpc:<Service>/<Method>`, the proto service name without its package
// (code calling a generated stub rarely spells the package: Go's
// `pb.NewCartServiceClient`, Python's `demo_pb2_grpc.CartServiceStub`) and the
// method as the proto spells it. Stubs in Java and JavaScript spell a method
// lowerCamel (`getCart`), C# adds an `Async` twin (`GetCartAsync`); both are
// read back to the proto spelling (`GetCart`): first letter upper-cased,
// `Async` dropped. Caveat: two packages each with a `CartService` are one
// service here. The standard services every server registers (`Health`,
// `ServerReflection`) are never recorded.
//
//   provides  a method of a class that implements a generated server base:
//             Java `extends <S>Grpc.<S>ImplBase` (a method taking a
//             `StreamObserver<...>`), C# `: <S>.<S>Base` (an `override`
//             method), Python `(<module>.<S>Servicer)` or a same-file subclass
//             of one (a method `(self, request, context)`), C++
//             `: public <ns>::<S>::Service` (an `override` method);
//             JavaScript/TypeScript `server.addService(<x>.<S>.service, {m1, m2})`
//             or `addService(<S>Service, {...})` (each key of the object);
//             Go: a method of the type registered with
//             `<pkg>.Register<S>Server(s, impl)` (`impl` a `&T{}`, `T{}`,
//             `new(T)`, or a local assigned one in the same function) with the
//             unary handler shape `(context.Context, *<pkg>.Req) (*<pkg>.Resp,
//             error)`, where both message types come from the registration's
//             package, anywhere in the same directory (one Go package).
//             Extraction records `grpc_server` (S, T, package alias) and
//             `grpc_handler` (method, T, name, package alias) facts;
//             grpc_server_contract_facts joins them per directory. A
//             streaming handler `(..., <pkg>.<S>_<M>Server) error` names its
//             service and method itself and provides directly.
//   uses      a method call on a generated client: Go `<pkg>.New<S>Client(c)`
//             (one argument; the call takes a context first, then the request,
//             then only `grpc.X(...)` call options), Java
//             `<S>Grpc.new[Blocking|Future]Stub(c)`
//             (and `.withX(...)` chained on it), Python
//             `<module>.<S>Stub(c)` / `<S>Stub(c)`, C# `new <S>.<S>Client(c)`,
//             JavaScript/TypeScript `new <S>Client(...)` or
//             `new <x>.<S>(address, credentials)` in a file importing
//             `@grpc/grpc-js` or `grpc`; called inline or through a name bound
//             to one anywhere in the file: a local, a field or property
//             (`cs.cartSvcClient`, `self.stub`, `this.client`) assigned one,
//             or declared with the client type (Go `<pkg>.<S>Client`, never a
//             pointer: a generated client is an interface; Java
//             `<S>[Blocking|Future]Stub` in a file naming `<S>Grpc`, C#
//             `<S>.<S>Client` or a `using A = ...<S>.<S>Client;` alias of it,
//             TypeScript `<S>Client` with the grpc import).
//             A name bound to two services records nothing. Methods of the
//             stub machinery itself are not RPCs (Java `withDeadlineAfter`,
//             `getChannel`; JavaScript `close`, `waitForReady`,
//             `makeUnaryRequest`, ...; Python `.future` / `.with_call` read
//             through to the method).
//
// Topics: `topic:<name>`, the topic, queue or subject name as the code spells
// it (Kafka topic, RabbitMQ queue on the default exchange, SQS queue, SNS
// topic, Pub/Sub topic). The PRODUCER provides it, a CONSUMER uses it, so the
// seam's provider is the service that publishes and its consumers the ones
// that subscribe. A name is read as: a string literal; an env read with a
// literal default (`os.getenv("X", "orders")`, `process.env.X || 'orders'`,
// `Environment.GetEnvironmentVariable("X") ?? "orders"`,
// `System.getenv("X") ?: "orders"`, `System.getenv().getOrDefault("X", "orders")`):
// the default; or a name bound to one of those in the same file (a constant, a
// field, `this.x` / `self.x`), unless the file binds it to two values. An SQS
// queue URL (`https://sqs.<region>.amazonaws.com/<account>/<name>`) reads as
// its last segment, an SNS topic ARN as its last `:` segment. A name in
// RabbitMQ's reserved `amq.` space (`amq.rabbitmq.reply-to`) is the broker's
// own and never recorded. Anything else
// (a parameter, a config property, a placeholder `${...}`, an exchange with a
// routing key) records nothing.
//
//   produce   Java `<KafkaTemplate>.send(t, ...)`, `new ProducerRecord<>(t, ...)`,
//             `<RabbitTemplate|AmqpTemplate>.convertAndSend(q, msg)` /
//             `.send(q, msg)` (two arguments: the default exchange),
//             `<SqsTemplate>.send(q, ...)`; Kotlin `ProducerRecord(t, ...)`;
//             C# (Confluent.Kafka) `.Produce(t, ...)` / `.ProduceAsync(t, ...)`,
//             (RabbitMQ.Client) `.BasicPublish("" or string.Empty, q, ...)`; Go
//             `sarama.ProducerMessage{Topic: t}`, kafka-go
//             `kafka.Message{Topic: t}` / `kafka.Writer{Topic: t}` /
//             `kafka.WriterConfig{Topic: t}`, amqp
//             `.Publish("", q, ...)` / `.PublishWithContext(ctx, "", q, ...)`;
//             Python (kafka-python, aiokafka, confluent-kafka)
//             `<producer>.send(t, ...)` / `.send_and_wait(t, ...)` /
//             `.produce(t, ...)`, pika `.basic_publish(exchange="", routing_key=q)`,
//             boto3 `.send_message(QueueUrl=u)`, `.publish(TopicArn=a)`,
//             Pub/Sub `.publish(<client>.topic_path(p, t), ...)`;
//             JavaScript/TypeScript kafkajs `.send({ topic: t })`, amqplib
//             `.sendToQueue(q, ...)` / `.publish('', q, ...)`, AWS SDK
//             `new SendMessageCommand({ QueueUrl: u })` /
//             `new PublishCommand({ TopicArn: a })`, Pub/Sub
//             `.topic(t).publish*(...)`.
//   consume   Java `@KafkaListener(topics = ...)`, `@RabbitListener(queues = ...)`,
//             `@SqsListener(...)`, `@JmsListener(destination = ...)`,
//             `<consumer>.subscribe(List.of(t))` (and
//             `Arrays.asList`, `Collections.singletonList`, `Set.of`),
//             `<StreamsBuilder>.stream(t)` / `.table(t)` / `.globalTable(t)`,
//             `.setQueueNames(q, ...)`; Kotlin `subscribe(listOf(t))` in a file
//             importing `org.apache.kafka`; C# `.Subscribe(t)` (Confluent.Kafka),
//             `.BasicConsume(q, ...)` (RabbitMQ.Client); Go kafka-go
//             `kafka.ReaderConfig{Topic: t}`, sarama `.ConsumePartition(t, ...)`
//             and `.Consume(ctx, []string{t}, h)`, confluent
//             `.SubscribeTopics([]string{t}, ...)`, amqp `.Consume(q, ...)`;
//             Python `KafkaConsumer(t, ...)` / `AIOKafkaConsumer(t, ...)`,
//             `<consumer>.subscribe([t])`, pika `.basic_consume(queue=q)`, boto3
//             `.receive_message(QueueUrl=u)`, Pub/Sub
//             `.create_subscription(topic=<client>.topic_path(p, t))`;
//             JavaScript/TypeScript kafkajs `.subscribe({ topic: t })` /
//             `({ topics: [...] })`, amqplib `.consume(q, ...)`, AWS SDK
//             `new ReceiveMessageCommand({ QueueUrl: u })`, Pub/Sub
//             `.topic(t).subscription(...)` / `.createSubscription(...)`.
//
// A client call needs its library in the file: Java, Kotlin, C# and Go check
// the receiver's declared type or the file's imports (`org.apache.kafka`,
// `org.springframework.kafka`, `org.springframework.amqp`, `Confluent.Kafka`,
// `RabbitMQ.Client`, `sarama`, `kafka-go`, `confluent-kafka-go`, `amqp`);
// Python and JavaScript the imports (`kafka`, `aiokafka`, `confluent_kafka`,
// `pika`, `boto3`, `pubsub`; `kafkajs`, `amqplib`, `amqp-connection-manager`,
// `@aws-sdk/client-sqs`, `@aws-sdk/client-sns`, `@google-cloud/pubsub`). A
// topic name is `[A-Za-z0-9._-]`, 1 to 249 characters.
//
// Every fact's source is the innermost enclosing function; outside any
// function it is, in JavaScript, js_syntax::reading_scope_id, in Python and C#
// the file (a script's or `Program.cs`'s top-level statement); in the other
// languages such a call records nothing. A test source (data_contracts.hpp
// is_test_source_path) records nothing: its producers and consumers stand in
// for other services.

#include "cgraph/language_config.hpp"

#include <tree_sitter/api.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// The raw relations Go server registration leaves for resolve_contracts.
inline constexpr std::string_view kGrpcServerRelation = "grpc_server";
inline constexpr std::string_view kGrpcHandlerRelation = "grpc_handler";

// True for a gRPC service every server registers for its own reasons
// (`Health`, `ServerReflection`): never recorded.
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
