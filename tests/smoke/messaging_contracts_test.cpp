// gRPC method and message topic facts (messaging_contracts.cpp), read through
// every extractor that calls it: a server method or a producer records
// `provides_contract grpc:<S>/<M>` / `topic:<name>`, a client call or a
// consumer records `uses_contract`. Go servers leave grpc_server/grpc_handler
// facts that grpc_server_contract_facts joins per directory. Facts are compared
// as "<provides|uses>|<label of the source node>|<contract>".
#include "cgraph/configured_extractors.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/messaging_contracts.hpp"
#include "cgraph/python_extractor.hpp"

#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

std::set<std::string> facts(const cgraph::ExtractionResult& result,
                            const std::vector<cgraph::RawRelation>& extra = {}) {
  std::unordered_map<std::string, std::string> labels;
  for (const auto& node : result.fragment.nodes) {
    labels.emplace(node.id, node.label);
  }
  std::set<std::string> out;
  const auto add = [&](const cgraph::RawRelation& relation) {
    if ((relation.relation == "provides_contract" || relation.relation == "uses_contract") &&
        (relation.context.starts_with("grpc:") || relation.context.starts_with("topic:"))) {
      const auto label = labels.find(relation.source_id);
      out.insert(std::string(relation.relation == "provides_contract" ? "provides" : "uses") + "|" +
                 (label == labels.end() ? "<no node: " + relation.source_id + ">" : label->second) + "|" +
                 relation.context);
    }
  };
  for (const auto& relation : result.raw_relations) add(relation);
  for (const auto& relation : extra) add(relation);
  return out;
}

bool expect(std::string_view what, const std::set<std::string>& actual, const std::set<std::string>& expected) {
  if (actual == expected) {
    return true;
  }
  std::cerr << what << ": unexpected messaging facts\n";
  for (const auto& fact : actual) std::cerr << "  actual:   " << fact << '\n';
  for (const auto& fact : expected) std::cerr << "  expected: " << fact << '\n';
  return false;
}

cgraph::ExtractionResult configured(cgraph::DetectedLanguage language, const std::string& file, std::string_view source) {
  return *cgraph::extract_configured_language(language, {.source_file = file, .relative_path = file, .source = source});
}

}  // namespace

int main() {
  bool ok = true;

  // Ids and bridging: both kinds cross repositories by name.
  ok &= cgraph::contract_id("grpc", "CartService/GetCart") == std::optional<std::string>("grpc:CartService/GetCart");
  ok &= cgraph::contract_id("topic", "orders") == std::optional<std::string>("topic:orders");
  ok &= cgraph::contract_kind_of("grpc:CartService/GetCart") == "grpc";
  ok &= cgraph::contract_kind_of("topic:orders") == "topic";
  ok &= cgraph::is_bridged_contract("grpc:CartService/GetCart") && cgraph::is_bridged_contract("topic:orders");
  ok &= cgraph::grpc_method_name("getCart") == "GetCart";
  ok &= cgraph::topic_name_from_address("https://sqs.us-east-1.amazonaws.com/123456789012/orders-queue") ==
        "orders-queue";
  ok &= cgraph::topic_name_from_address("arn:aws:sns:us-east-1:123456789012:order-events") == "order-events";
  ok &= cgraph::is_topic_name("payment-orders") && !cgraph::is_topic_name("${kafka.topic}") &&
        !cgraph::is_topic_name("");
  if (!ok) {
    std::cerr << "contract id / name helpers wrong\n";
  }

  // go-redis (`redis.UniversalClient`, `redis.NewClusterClient`) has the
  // client's shape but its package is no generated stub: nothing.
  // Go client: inline constructor, a struct field typed with the client, a
  // field and a local assigned one. Lowercase methods and a zero-argument
  // `New...Client()` are not RPCs; Health is standard. A generated client is
  // an interface: a `*pkg.XClient` field or `pkg.XClient{}` literal is another
  // library's struct, and a call that does not take a context first and then
  // only grpc call options is not a generated client's.
  {
    const auto result = configured(cgraph::DetectedLanguage::Go, "checkout/main.go", R"go(
package main

import (
	pb "example.com/demo/genproto"
	"github.com/redis/go-redis/v9"
)

type checkout struct {
	rdb           redis.UniversalClient
	cartSvcClient pb.CartServiceClient
	other         *grpc.ClientConn
	ilm           *esclient.ILMClient
	http          *tracing.HTTPClient
}

func (cs *checkout) setup(c *grpc.ClientConn) {
	cs.cartSvcClient = pb.NewCartServiceClient(c)
}

func (cs *checkout) placeOrder(ctx context.Context) {
	cart, _ := cs.cartSvcClient.GetCart(ctx, &pb.GetCartRequest{})
	pb.NewPaymentServiceClient(cs.other).Charge(ctx, nil)
	cl := pb.NewProductCatalogServiceClient(cs.other)
	cl.GetProduct(ctx, nil)
	healthpb.NewHealthClient(cs.other).Check(ctx, nil)
	flags := openfeature.NewDefaultClient()
	flags.BooleanValue(ctx, "x", false, nil)
	cs.cartSvcClient.toString()
	cs.ilm.DeletePolicy(ctx, "p")
	tracing.NewHTTPClient(tracer).GetJSON(ctx, "/customer", url, &out)
	cl.ListProducts(ctx, nil, grpc.WaitForReady(true))
	cl.Search(name, nil)
	cs.rdb.Del(ctx, "k")
	cluster := redis.NewClusterClient(&redis.ClusterOptions{})
	cluster.Get(ctx, "k")
}
)go");
    ok &= expect("go client", facts(result),
                 {
                     "uses|placeOrder|grpc:CartService/GetCart",
                     "uses|placeOrder|grpc:PaymentService/Charge",
                     "uses|placeOrder|grpc:ProductCatalogService/GetProduct",
                     "uses|placeOrder|grpc:ProductCatalogService/ListProducts",
                 });
  }

  // A type registered for two services (Foo, Bar) provides nothing.
  // Go server: the registered type's unary handlers in the same directory,
  // whose messages come from the registration's package. The health handler
  // (healthpb messages), a helper with another shape and a handler of an
  // unregistered type provide nothing; a streaming handler names itself.
  {
    const auto main_go = configured(cgraph::DetectedLanguage::Go, "catalog/main.go", R"go(
package main

func run() {
	svc := &productCatalog{}
	pb.RegisterProductCatalogServiceServer(srv, svc)
	healthpb.RegisterHealthServer(srv, svc)
	pb.RegisterAdServiceServer(srv, new(adServer))
	multi := &multiServer{}
	pb.RegisterFooServer(srv, multi)
	pb.RegisterBarServer(srv, multi)
}
)go");
    const auto catalog_go = configured(cgraph::DetectedLanguage::Go, "catalog/catalog.go", R"go(
package main

func (p *productCatalog) ListProducts(context.Context, *pb.Empty) (*pb.ListProductsResponse, error) { return nil, nil }
func (p *productCatalog) GetProduct(ctx context.Context, req *pb.GetProductRequest) (*pb.Product, error) { return nil, nil }
func (p *productCatalog) Check(ctx context.Context, req *healthpb.HealthCheckRequest) (*healthpb.HealthCheckResponse, error) { return nil, nil }
func (p *productCatalog) parse(ctx context.Context, n int) ([]*pb.Product, error) { return nil, nil }
func (a *adServer) GetAds(ctx context.Context, req *pb.AdRequest) (*pb.AdResponse, error) { return nil, nil }
func (o *other) GetCart(ctx context.Context, req *pb.GetCartRequest) (*pb.Cart, error) { return nil, nil }
func (p *productCatalog) Watch(req *pb.WatchRequest, stream pb.ProductCatalogService_WatchServer) error { return nil }
func (m *multiServer) Ping(ctx context.Context, req *pb.PingRequest) (*pb.PingResponse, error) { return nil, nil }
)go");
    const auto elsewhere = configured(cgraph::DetectedLanguage::Go, "other/ads.go", R"go(
package other

func (a *adServer) RemoveAd(ctx context.Context, req *pb.AdRequest) (*pb.AdResponse, error) { return nil, nil }
)go");
    std::vector<cgraph::RawRelation> all = main_go.raw_relations;
    all.insert(all.end(), catalog_go.raw_relations.begin(), catalog_go.raw_relations.end());
    all.insert(all.end(), elsewhere.raw_relations.begin(), elsewhere.raw_relations.end());
    ok &= expect("go server", facts(catalog_go, cgraph::grpc_server_contract_facts(all)),
                 {
                     "provides|ListProducts|grpc:ProductCatalogService/ListProducts",
                     "provides|GetProduct|grpc:ProductCatalogService/GetProduct",
                     "provides|GetAds|grpc:AdService/GetAds",
                     "provides|Watch|grpc:ProductCatalogService/Watch",
                 });
  }

  // Go topics: sarama and kafka-go messages, a reader config, an amqp publish
  // on the default exchange (one to a named exchange is a routing key, not a
  // queue), sarama consumers. A const names the topic; a `Topic` of another
  // library's struct is nothing.
  {
    const auto result = configured(cgraph::DetectedLanguage::Go, "svc/kafka.go", R"go(
package svc

import (
	"github.com/IBM/sarama"
	"github.com/segmentio/kafka-go"
	amqp "github.com/rabbitmq/amqp091-go"
)

const ordersTopic = "orders"

func produce(p sarama.AsyncProducer) {
	p.Input() <- &sarama.ProducerMessage{Topic: ordersTopic, Value: nil}
	w := kafka.Writer{Topic: "payments"}
	other := mylib.Message{Topic: "nope"}
}

func consume(c sarama.Consumer, g sarama.ConsumerGroup, ch *amqp.Channel) {
	r := kafka.NewReader(kafka.ReaderConfig{Topic: "stock", GroupID: "g"})
	c.ConsumePartition("audit", 0, 0)
	g.Consume(ctx, []string{"refunds", ordersTopic}, handler)
	ch.Publish("", "tasks", false, false, msg)
	ch.Publish("events", "tasks.created", false, false, msg)
	ch.Consume("emails", "", true, false, false, false, nil)
}
)go");
    ok &= expect("go topics", facts(result),
                 {
                     "provides|produce|topic:orders",
                     "provides|produce|topic:payments",
                     "uses|consume|topic:stock",
                     "uses|consume|topic:audit",
                     "uses|consume|topic:refunds",
                     "uses|consume|topic:orders",
                     "provides|consume|topic:tasks",
                     "uses|consume|topic:emails",
                 });
  }

  // Java: an ImplBase server (RPC methods take a StreamObserver), stubs typed
  // and constructed, Spring Kafka, Rabbit and Kafka Streams.
  {
    const auto result = configured(cgraph::DetectedLanguage::Java, "src/main/java/demo/AdService.java", R"java(
package demo;

import org.springframework.kafka.core.KafkaTemplate;
import org.springframework.amqp.rabbit.core.RabbitTemplate;
import demo.ProductCatalogServiceGrpc.ProductCatalogServiceBlockingStub;

class AdServiceImpl extends demo.AdServiceGrpc.AdServiceImplBase {
  private final ProductCatalogServiceBlockingStub productCatalog;
  private CartServiceGrpc.CartServiceStub cart;
  private KafkaTemplate<Long, Order> template;
  private RabbitTemplate rabbit;
  static final String QUEUE = "shipping-task";

  @Override
  public void getAds(AdRequest req, StreamObserver<AdResponse> responseObserver) {
    productCatalog.listProducts(Empty.getDefaultInstance());
    CurrencyServiceGrpc.newBlockingStub(channel).withDeadlineAfter(1, TimeUnit.SECONDS).convert(req);
  }

  private List<Ad> pickAds(int n) {
    template.send("orders", 1L, order);
    rabbit.convertAndSend(QUEUE, shipment);
    rabbit.convertAndSend("exchange", "key", shipment);
    mailer.send("not-a-topic", 1L);
    return null;
  }

  @KafkaListener(id = "orders", topics = "orders", groupId = "payment")
  public void onOrder(Order o) {}

  @RabbitListener(queues = {"a-queue", QUEUE})
  public void onShipment(Shipment s) {}

  public KStream<Long, Order> stream(StreamsBuilder builder) {
    return builder.stream("payment-orders", Consumed.with(Serdes.Long(), serde));
  }

  void listen(SimpleMessageListenerContainer container) {
    container.setQueueNames(QUEUE);
  }
}
)java");
    ok &= expect("java", facts(result),
                 {
                     "provides|getAds|grpc:AdService/GetAds",
                     "uses|getAds|grpc:ProductCatalogService/ListProducts",
                     "uses|getAds|grpc:CurrencyService/Convert",
                     "provides|pickAds|topic:orders",
                     "provides|pickAds|topic:shipping-task",
                     "uses|onOrder|topic:orders",
                     "uses|onShipment|topic:a-queue",
                     "uses|onShipment|topic:shipping-task",
                     "uses|stream|topic:payment-orders",
                     "uses|listen|topic:shipping-task",
                 });
  }

  // Kotlin: a consumer subscribing to an env read with a literal default, a
  // ProducerRecord; without the Kafka import nothing.
  {
    const auto result = configured(cgraph::DetectedLanguage::Kotlin, "frauddetection/main.kt", R"kt(
import org.apache.kafka.clients.consumer.KafkaConsumer

val topic: String = System.getenv("KAFKA_TOPIC") ?: "orders"

fun main() {
    val consumer = KafkaConsumer<String, ByteArray>(props).apply {
        subscribe(listOf(topic))
    }
    producer.send(ProducerRecord("fraud-alerts", key, value))
}
)kt");
    ok &= expect("kotlin", facts(result), {"uses|main|topic:orders", "provides|main|topic:fraud-alerts"});
    const auto plain = configured(cgraph::DetectedLanguage::Kotlin, "other/main.kt", R"kt(
fun main() { bus.subscribe(listOf("orders")) }
)kt");
    ok &= expect("kotlin without kafka", facts(plain), {});
  }

  // C#: a generated base's overrides, clients (also through a using alias of
  // the generated client type), Confluent.Kafka.
  {
    const auto result = configured(cgraph::DetectedLanguage::CSharp, "src/services/CartService.cs", R"cs(
using Confluent.Kafka;
using RabbitMQ.Client;
using GrpcBasketClient = eShop.Basket.API.Grpc.Basket.BasketClient;

public class BasketService(GrpcBasketClient basketClient)
{
    public async Task Delete() { await basketClient.DeleteBasketAsync(new DeleteBasketRequest()); }
}

public class CartService : Oteldemo.CartService.CartServiceBase
{
    private static readonly string TopicName = Environment.GetEnvironmentVariable("KAFKA_TOPIC") ?? "orders";
    private readonly CurrencyService.CurrencyServiceClient _currency;

    public override async Task<Empty> AddItem(AddItemRequest request, ServerCallContext context)
    {
        var client = new PaymentService.PaymentServiceClient(channel);
        await client.ChargeAsync(new ChargeRequest());
        _currency.Convert(req);
        return new Empty();
    }

    public async Task Helper()
    {
        _consumer.Subscribe(TopicName);
        producer.Produce("audit", msg);
        await channel.BasicPublishAsync(exchange: string.Empty, routingKey: "hello", body: body);
        await channel.BasicConsumeAsync("task_queue", autoAck: false, consumer: consumer);
        var http = new HttpClient();
        await http.GetAsync("x");
    }
}
)cs");
    ok &= expect("csharp", facts(result),
                 {
                     "uses|Delete|grpc:Basket/DeleteBasket",
                     "provides|AddItem|grpc:CartService/AddItem",
                     "uses|AddItem|grpc:PaymentService/Charge",
                     "uses|AddItem|grpc:CurrencyService/Convert",
                     "uses|Helper|topic:orders",
                     "provides|Helper|topic:audit",
                     "provides|Helper|topic:hello",
                     "uses|Helper|topic:task_queue",
                 });
  }

  // C++: a generated Service's overrides; a helper and the Health service
  // provide nothing.
  {
    const auto result = configured(cgraph::DetectedLanguage::Cpp, "src/server.cpp", R"cpp(
class CurrencyService final : public oteldemo::CurrencyService::Service
{
  Status Convert(ServerContext* context, const CurrencyConversionRequest* request, Money* response) override
  {
    return Status::OK;
  }
  Status helper(int x) { return Status::OK; }
};
class HealthServer final : public grpc::health::v1::Health::Service
{
  Status Check(ServerContext* context, const HealthCheckRequest* request, HealthCheckResponse* response) override
  {
    return Status::OK;
  }
};
)cpp");
    ok &= expect("cpp", facts(result), {"provides|Convert|grpc:CurrencyService/Convert"});
  }

  // Python: a servicer and a same-file subclass of one, a stub bound at module
  // level and used in a method, `.future`, kafka-python, pika.
  {
    const std::string file = "recommendation_server.py";
    const auto result = cgraph::extract_python({.source_file = file, .relative_path = file, .source = R"py(
import grpc
import demo_pb2_grpc
from kafka import KafkaProducer, KafkaConsumer
import pika

ORDERS = "orders"

class BaseEmailService(demo_pb2_grpc.EmailServiceServicer):
    def Check(self, request, context):
        return None

class EmailService(BaseEmailService):
    def SendOrderConfirmation(self, request, context):
        return None
    def send_email(client, address, content):
        return None

class RecommendationService(demo_pb2_grpc.RecommendationServiceServicer):
    def ListRecommendations(self, request, context):
        cat = product_catalog_stub.ListProducts(None)
        self.cart.GetCart.future(None)
        return cat

    def _private(self, request, context):
        return None

def publish(channel):
    producer = KafkaProducer(bootstrap_servers="k")
    producer.send(ORDERS, b"x")
    channel.basic_publish(exchange="", routing_key="tasks", body=b"x")
    channel.basic_publish(exchange="events", routing_key="tasks.created", body=b"x")
    sock.send("not-a-topic")

def forward(ORDERS):
    relay = KafkaProducer(bootstrap_servers="k")
    relay.send(ORDERS, b"x")

def consume(channel):
    consumer = KafkaConsumer("payments", "refunds", group_id="g")
    channel.basic_consume(queue="emails", on_message_callback=cb)

if __name__ == "__main__":
    product_catalog_stub = demo_pb2_grpc.ProductCatalogServiceStub(channel)
    channel.basic_publish(exchange="", routing_key="hello", body=b"x")
    channel.basic_consume(queue="amq.rabbitmq.reply-to", on_message_callback=cb)

class Holder:
    def __init__(self, channel):
        self.cart = demo_pb2_grpc.CartServiceStub(channel)
)py"});
    ok &= expect("python", facts(result),
                 {
                     "provides|Check|grpc:EmailService/Check",
                     "provides|SendOrderConfirmation|grpc:EmailService/SendOrderConfirmation",
                     "provides|ListRecommendations|grpc:RecommendationService/ListRecommendations",
                     "uses|ListRecommendations|grpc:ProductCatalogService/ListProducts",
                     "uses|ListRecommendations|grpc:CartService/GetCart",
                     "provides|publish|topic:orders",
                     "provides|publish|topic:tasks",
                     "uses|consume|topic:payments",
                     "uses|consume|topic:refunds",
                     "uses|consume|topic:emails",
                     "provides|recommendation_server.py|topic:hello",
                 });
    const std::string test_file = "tests/test_server.py";
    const auto test = cgraph::extract_python({.source_file = test_file, .relative_path = test_file, .source = R"py(
import demo_pb2_grpc
def test_it(channel):
    demo_pb2_grpc.CartServiceStub(channel).GetCart(None)
)py"});
    ok &= expect("python test file", facts(test), {});
  }

  // TypeScript: generated grpc-js clients, a server's addService, kafkajs. A
  // `*Client` in a file without the grpc import is not a stub.
  {
    const std::string file = "gateways/rpc/Cart.gateway.ts";
    const auto result = cgraph::extract_typescript({.source_file = file, .relative_path = file, .source = R"ts(
import { ChannelCredentials } from '@grpc/grpc-js';
import { CartServiceClient } from '../../protos/demo';
import { Kafka } from 'kafkajs';

const client = new CartServiceClient(CART_ADDR, ChannelCredentials.createInsecure());

export function getCart(userId: string) {
  client.getCart({ userId }, cb);
  client.close();
}

export function serve(server: any, shopProto: any) {
  server.addService(shopProto.CurrencyService.service, { getSupportedCurrencies, convert: handler });
}

import { S3Client } from '@aws-sdk/client-s3';
const s3 = new S3Client({});
export function put(cmd: any) { return s3.send(cmd); }

export async function produce(producer: any, consumer: any) {
  await producer.send({ topic: 'orders', messages: [] });
  await consumer.subscribe({ topics: ['payments', 'refunds'] });
}
)ts"});
    ok &= expect("typescript", facts(result),
                 {
                     "uses|getCart|grpc:CartService/GetCart",
                     "provides|serve|grpc:CurrencyService/GetSupportedCurrencies",
                     "provides|serve|grpc:CurrencyService/Convert",
                     "provides|produce|topic:orders",
                     "uses|produce|topic:payments",
                     "uses|produce|topic:refunds",
                 });
    const std::string sdk = "lib/s3.ts";
    const auto plain = cgraph::extract_typescript({.source_file = sdk, .relative_path = sdk, .source = R"ts(
import { S3Client } from '@aws-sdk/client-s3';
const s3 = new S3Client({});
export function put() { return s3.send(cmd); }
)ts"});
    ok &= expect("typescript without grpc", facts(plain), {});
  }

  // One name in one C# file bound to a gRPC client and to an HttpClient is
  // ambiguous: neither class's call records a fact.
  {
    const auto result = configured(cgraph::DetectedLanguage::CSharp, "src/Clients.cs", R"cs(
public class A
{
    private readonly CurrencyService.CurrencyServiceClient _client;
    public void Run() { _client.Convert(req); }
}
public class B
{
    private readonly HttpClient _client;
    public async Task Fetch() { await _client.GetAsync(url); }
}
)cs");
    ok &= expect("csharp ambiguous name", facts(result), {});
  }

  // The per-file index belongs to one extraction: the same path, the same
  // size and the same first and last bytes, with another topic in the middle,
  // gives the second topic.
  {
    const std::string padding(200, '#');
    const auto source = [&](std::string_view topic) {
      return "# " + padding + "\nfrom kafka import KafkaProducer\nTOPIC = \"" + std::string(topic) +
             "\"\ndef publish():\n    producer = KafkaProducer()\n    producer.send(TOPIC, b\"x\")\n# " + padding + "\n";
    };
    const std::string file = "svc/publisher.py";
    // One buffer edited in place: the same address and size, as a daemon's
    // re-read of an edited file can hand the extractor.
    auto buffer = source("orders-v1");
    const auto first = cgraph::extract_python({.source_file = file, .relative_path = file, .source = buffer});
    buffer.replace(buffer.find("orders-v1"), 9, "orders-v2");
    const auto second = cgraph::extract_python({.source_file = file, .relative_path = file, .source = buffer});
    ok &= expect("first extraction", facts(first), {"provides|publish|topic:orders-v1"});
    ok &= expect("second extraction", facts(second), {"provides|publish|topic:orders-v2"});
  }

  // A file whose tree is far deeper than a worker stack allows a recursion to
  // go (a generated 20000-term expression) is read without one.
  {
    std::string deep = "public class Gen {\n  private readonly CartService.CartServiceClient _cart;\n"
                       "  private readonly int x = 1";
    for (int term = 0; term < 20000; ++term) deep += " + 1";
    deep += ";\n  public void F() { _cart.GetCart(req); }\n}\n";
    const auto result = configured(cgraph::DetectedLanguage::CSharp, "src/Gen.cs", deep);
    ok &= expect("deep file", facts(result), {"uses|F|grpc:CartService/GetCart"});
  }

  if (!ok) {
    return 1;
  }
  std::cout << "messaging_contracts_test: ok\n";
  return 0;
}
