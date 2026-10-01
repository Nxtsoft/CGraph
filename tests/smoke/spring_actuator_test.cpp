#include "cgraph/spring_actuator.hpp"

#include "cgraph/configured_extractors.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/pipeline.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view message) {
  std::cerr << "spring_actuator_test: " << message << '\n';
  return 1;
}

// The shape of a Spring Boot Kotlin service's module build file.
constexpr std::string_view kGradle = R"(plugins {
    kotlin("jvm") version "2.1.0"
    id("org.springframework.boot") version "3.4.0"
}

dependencies {
    implementation("org.springframework.boot:spring-boot-starter-web")
    implementation("org.springframework.boot:spring-boot-starter-actuator")
    testImplementation("org.springframework.boot:spring-boot-starter-test")
}
)";

struct Fixture {
  std::string path;
  std::string source;
};

cgraph::ExtractionResult build_facts(std::string_view relative_path, std::string_view source) {
  cgraph::ExtractionResult result;
  cgraph::append_spring_actuator_facts(
      {.source_file = "/repo/" + std::string(relative_path), .relative_path = std::string(relative_path), .source = source},
      result);
  return result;
}

// The routes a module with these files serves, as "<verb> <path>".
std::set<std::string> routes_of(const std::vector<Fixture>& files, std::vector<std::string>* handlers = nullptr) {
  std::vector<cgraph::RawRelation> facts;
  for (const auto& file : files) {
    const cgraph::ExtractionContext context{
        .source_file = "/repo/" + file.path, .relative_path = file.path, .source = file.source};
    const auto result = cgraph::is_spring_build_file(file.path) ? build_facts(file.path, file.source)
                                                                : cgraph::extract_spring_application_config(context);
    facts.insert(facts.end(), result.raw_relations.begin(), result.raw_relations.end());
  }
  std::set<std::string> routes;
  for (const auto& relation : cgraph::spring_actuator_routes(facts)) {
    routes.insert(relation.context);
    if (handlers != nullptr) {
      handlers->push_back(relation.source_id + " " + relation.context);
    }
  }
  return routes;
}

std::string join(const std::set<std::string>& routes) {
  std::string out;
  for (const auto& route : routes) {
    out += (out.empty() ? "" : ", ") + route;
  }
  return out;
}

int expect(const std::set<std::string>& actual, const std::set<std::string>& expected, std::string_view what) {
  if (actual != expected) {
    return fail(std::string(what) + ": got {" + join(actual) + "}, want {" + join(expected) + "}");
  }
  return 0;
}

// Only `application[-profile].{yml,yaml,properties}` under src/main/resources.
int test_detects_application_config_only() {
  for (const auto* yes : {"svc/src/main/resources/application.yml", "src/main/resources/application-prod.yaml",
                          "/abs/svc/src/main/resources/application-local.properties"}) {
    if (!cgraph::is_spring_application_config(yes) ||
        cgraph::detect_language(yes) != cgraph::DetectedLanguage::SpringConfig) {
      return fail(std::string("should detect ") + yes);
    }
  }
  for (const auto* no : {"src/test/resources/application.yml", "k8s/base/application.yml", "src/main/resources/application-.yml",
                         "src/main/resources/bootstrap.yml", "src/main/resources/config/application.yml",
                         "docker-compose.yml", "src/main/resources/applications.yml"}) {
    if (cgraph::is_spring_application_config(no)) {
      return fail(std::string("should not detect ") + no);
    }
  }
  return 0;
}

// The build declares a Boot web application with the actuator: one node at the
// dependency line. Anything less serves no HTTP actuator endpoint.
int test_build_file_declares_the_actuator() {
  const auto result = build_facts("svc/build.gradle.kts", kGradle);
  const auto node = std::ranges::find_if(result.fragment.nodes, [](const auto& n) { return n.kind == "spring_actuator"; });
  if (node == result.fragment.nodes.end() || !node->source_location || node->source_location->start_line != 8 ||
      result.raw_relations.size() != 1 || result.raw_relations.front().relation != "actuator_app") {
    return fail("a Boot web app with the actuator yields a spring_actuator node at line 8 and one actuator_app fact");
  }
  const auto replace = [](std::string text, std::string_view from, std::string_view to) {
    text.replace(text.find(from), from.size(), to);
    return text;
  };
  const std::string gradle(kGradle);
  const std::vector<std::pair<std::string, std::string>> none = {
      {"no web starter (JMX only)", replace(gradle, "spring-boot-starter-web\"", "spring-boot-starter-json\"")},
      {"commented out", replace(gradle, "    implementation(\"org.springframework.boot:spring-boot-starter-actuator\")",
                                "    // implementation(\"org.springframework.boot:spring-boot-starter-actuator\")")},
      {"test classpath only", replace(gradle, "implementation(\"org.springframework.boot:spring-boot-starter-actuator\")",
                                      "testImplementation(\"org.springframework.boot:spring-boot-starter-actuator\")")},
      {"not a Boot application", replace(gradle, "id(\"org.springframework.boot\")", "id(\"java-library\")")},
      {"declared for subprojects", replace(gradle, "dependencies {", "subprojects {\ndependencies {") + "}\n"},
  };
  for (const auto& [why, source] : none) {
    if (!build_facts("svc/build.gradle.kts", source).raw_relations.empty()) {
      return fail("no actuator fact when " + why);
    }
  }
  const auto groovy = build_facts("build.gradle",
                                  "plugins { id 'org.springframework.boot' version '3.3.0' }\n"
                                  "dependencies {\n"
                                  "  implementation 'org.springframework.boot:spring-boot-starter-webflux'\n"
                                  "  implementation group: 'org.springframework.boot', name: 'spring-boot-starter-actuator'\n"
                                  "}\n");
  if (groovy.raw_relations.size() != 1 || groovy.raw_relations.front().context.find("\"reactive\":true") == std::string::npos) {
    return fail("Groovy DSL map notation with webflux is a reactive actuator app");
  }
  const std::string pom =
      "<project><parent><groupId>org.springframework.boot</groupId><artifactId>spring-boot-starter-parent</artifactId></parent>\n"
      "<dependencies>\n"
      "<dependency><groupId>org.springframework.boot</groupId><artifactId>spring-boot-starter-web</artifactId></dependency>\n"
      "<dependency><groupId>org.springframework.boot</groupId><artifactId>spring-boot-starter-actuator</artifactId>SCOPE</dependency>\n"
      "</dependencies></project>\n";
  const auto maven = build_facts("pom.xml", replace(pom, "SCOPE", ""));
  if (maven.raw_relations.size() != 1 ||
      std::ranges::none_of(maven.fragment.nodes, [](const auto& n) { return n.kind == "file" && n.id == "pom_xml"; })) {
    return fail("a Maven Boot web app with the actuator yields the fact and its file node");
  }
  if (!build_facts("pom.xml", replace(pom, "SCOPE", "<scope>test</scope>")).raw_relations.empty()) {
    return fail("a test-scoped actuator serves nothing");
  }
  return 0;
}

// No config: only health, at /actuator, plus the discovery page, handled by the
// build file's actuator node.
int test_default_exposure_is_health() {
  std::vector<std::string> handlers;
  if (const int failed = expect(routes_of({{"svc/build.gradle.kts", std::string(kGradle)}}, &handlers),
                                {"get /actuator", "get /actuator/health", "get /actuator/health/{*path}"}, "default exposure")) {
    return failed;
  }
  for (const auto& handler : handlers) {
    if (!handler.starts_with("svc_build_gradle_kts_spring_actuator ")) {
      return fail("default routes are handled by the build file's actuator node: " + handler);
    }
  }
  // Config for another module, or the root, does not apply.
  return expect(routes_of({{"svc/build.gradle.kts", std::string(kGradle)},
                           {"other/src/main/resources/application.yml",
                            "management:\n  endpoints:\n    web:\n      exposure:\n        include: env\n"}}),
                {"get /actuator", "get /actuator/health", "get /actuator/health/{*path}"}, "another module's config");
}

// include / exclude / `*`, access, base path, path mapping, ports, context path.
int test_exposure_rules() {
  const auto with = [](std::string config, std::string build = std::string(kGradle)) {
    return routes_of({{"svc/build.gradle.kts", std::move(build)}, {"svc/src/main/resources/application.yml", std::move(config)}});
  };
  int failures = 0;
  failures += expect(with("management:\n  endpoints:\n    web:\n      exposure:\n        include: health,info,metrics\n"),
                     {"get /actuator", "get /actuator/health", "get /actuator/health/{*path}", "get /actuator/info",
                      "get /actuator/metrics", "get /actuator/metrics/{requiredMetricName}"},
                     "include list");
  failures += expect(with("management.endpoints.web.exposure:\n  include:\n    - info\n    - env\n  exclude: env\n"),
                     {"get /actuator", "get /actuator/info"}, "YAML list include, exclude wins, dotted key");
  // `*` exposes every built-in endpoint whose access allows it: shutdown and
  // heapdump default to none, prometheus needs its registry, custom ids are not
  // known endpoints.
  const auto star = with("management.endpoints.web.exposure.include: \"*\"\n");
  if (!star.contains("get /actuator/beans") || !star.contains("post /actuator/loggers/{name}") ||
      !star.contains("delete /actuator/caches/{cache}") || star.contains("post /actuator/shutdown") ||
      star.contains("get /actuator/heapdump") || star.contains("get /actuator/prometheus")) {
    failures += fail("`*` exposes built-ins except shutdown/heapdump and prometheus without its registry: " + join(star));
  }
  std::string with_registry(kGradle);
  with_registry.replace(with_registry.find("    testImplementation"), 0,
                        "    runtimeOnly(\"io.micrometer:micrometer-registry-prometheus\")\n");
  failures += expect(with("management:\n  endpoints:\n    web:\n      exposure:\n        include: prometheus,shutdown\n"
                          "  endpoint:\n    shutdown:\n      access: unrestricted\n",
                          with_registry),
                     {"get /actuator", "get /actuator/prometheus", "post /actuator/shutdown"},
                     "prometheus with its registry, shutdown granted access");
  failures += expect(with("management:\n  endpoints:\n    web:\n      exposure:\n        include: loggers,env\n"
                          "    access:\n      default: read-only\n  endpoint:\n    env:\n      enabled: false\n"),
                     {"get /actuator", "get /actuator/loggers", "get /actuator/loggers/{name}"},
                     "read-only default keeps GET only; legacy enabled:false is none");
  failures += expect(with("management:\n  endpoints:\n    web:\n      basePath: /manage/\n      path-mapping:\n        health: healthcheck\n"),
                     {"get /manage", "get /manage/healthcheck", "get /manage/healthcheck/{*path}"},
                     "relaxed basePath, trailing slash dropped, path mapping");
  failures += expect(with("management.endpoints.web.base-path: /\n"), {"get /health", "get /health/{*path}"},
                     "root base path serves no discovery page");
  failures += expect(with("management.endpoints.web.base-path: actuator\n"), {}, "a base path without / does not start");
  failures += expect(with("management:\n  server:\n    port: -1\n"), {}, "management port -1 serves nothing");
  failures += expect(with("server:\n  servlet:\n    context-path: /idp\n"),
                     {"get /idp/actuator", "get /idp/actuator/health", "get /idp/actuator/health/{*path}"},
                     "context path on the main port");
  failures += expect(with("server:\n  servlet:\n    context-path: /idp\nmanagement:\n  server:\n    port: 9090\n    base-path: /mgmt\n"),
                     {"get /mgmt/actuator", "get /mgmt/actuator/health", "get /mgmt/actuator/health/{*path}"},
                     "management port: management base path, not the context path");
  failures += expect(with("management:\n  endpoints:\n    web:\n      discovery:\n        enabled: false\n"
                          "  endpoint:\n    health:\n      probes:\n        enabled: true\n      group:\n"
                          "        custom-group:\n          include: db\n"),
                     {"get /actuator/health", "get /actuator/health/{*path}", "get /actuator/health/custom-group",
                      "get /actuator/health/liveness", "get /actuator/health/readiness"},
                     "probes and configured groups; discovery disabled");
  failures += expect(with("spring:\n  mvc:\n    servlet:\n      path: /api\n"), {}, "a dispatcher servlet path is not modeled");
  failures += expect(with("management.endpoints.web.exposure.include: ${EXPOSE:health,info}\n"),
                     {"get /actuator", "get /actuator/health", "get /actuator/health/{*path}", "get /actuator/info"},
                     "a placeholder runs with its default");
  failures += expect(with("management.endpoints.web.exposure.include: ${EXPOSE}\n"), {},
                     "a placeholder without a default is unknowable");
  return failures;
}

// Profiles: the base and each profile overlay are configurations; the routes
// are their union. Properties beat YAML at one location; a profile file beats
// the base; a later document beats an earlier one.
int test_profiles_and_precedence() {
  int failures = 0;
  std::vector<std::string> handlers;
  const auto profiled = routes_of(
      {{"svc/build.gradle.kts", std::string(kGradle)},
       {"svc/src/main/resources/application.yml",
        "management:\n  endpoints:\n    web:\n      exposure:\n        include: health,info\n"
        "---\nspring:\n  config:\n    activate:\n      on-profile: debug\nmanagement.endpoints.web.exposure.include: env\n"
        "---\nspring.config.activate.on-profile: \"!prod\"\nmanagement.endpoints.web.exposure.include: beans\n"},
       {"svc/src/main/resources/application-prod.yml", "management.endpoints.web.base-path: /ops\n"}},
      &handlers);
  failures += expect(profiled,
                     {"get /actuator", "get /actuator/health", "get /actuator/health/{*path}", "get /actuator/info",
                      "get /actuator/env", "get /actuator/env/{toMatch}", "get /ops", "get /ops/health",
                      "get /ops/health/{*path}", "get /ops/info"},
                     "base, on-profile document and profile file; a profile expression applies to none");
  const auto handled = [&](std::string_view entry) { return std::ranges::find(handlers, entry) != handlers.end(); };
  if (!handled("svc_src_main_resources_application_yml_actuator_exposure_0 get /actuator/health") ||
      !handled("svc_src_main_resources_application_yml_actuator_exposure_1 get /actuator/env") ||
      !handled("svc_src_main_resources_application_yml_actuator_exposure_0 get /ops/info")) {
    return fail("each route is handled by the exposure node of the document whose include won");
  }
  failures += expect(routes_of({{"svc/build.gradle.kts", std::string(kGradle)},
                                {"svc/src/main/resources/application.yml", "management.endpoints.web.exposure.include: env\n"},
                                {"svc/src/main/resources/application.properties",
                                 "# comment\nmanagement.endpoints.web.exposure.include[0]=info\n"}}),
                     {"get /actuator", "get /actuator/info"}, ".properties wins over YAML");
  failures += expect(routes_of({{"svc/build.gradle.kts", std::string(kGradle)},
                                {"svc/src/main/resources/application.properties",
                                 "management.endpoints.web.exposure.include=info\n#---\n"
                                 "spring.config.activate.on-profile=ops\nmanagement.endpoints.web.base-path=/ops\n"}}),
                     {"get /actuator", "get /actuator/info", "get /ops", "get /ops/info"}, "properties documents");
  return failures;
}

// End to end: a Kotlin Ktor client in the same repo consumes the served
// endpoint, handled by the exposure config. A repo that only mentions the
// actuator (deployment YAML, docs) serves nothing.
int test_pipeline_serves_and_does_not_over_link() {
  const auto root = std::filesystem::temp_directory_path() / "cgraph_spring_actuator_test";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "svc" / "src" / "main" / "resources");
  std::filesystem::create_directories(root / "svc" / "src" / "main" / "kotlin");
  std::ofstream(root / "svc" / "build.gradle.kts") << kGradle;
  std::ofstream(root / "svc" / "src" / "main" / "resources" / "application.yml")
      << "server:\n  port: 8080\n\nmanagement:\n  endpoints:\n    web:\n      exposure:\n        include: health,info\n";
  std::ofstream(root / "svc" / "src" / "main" / "kotlin" / "ApiClient.kt")
      << "import io.ktor.client.HttpClient\n"
         "class ApiClient(private val baseUrl: String) {\n"
         "    private val client = HttpClient()\n"
         "    suspend fun health(): String = client.get(\"$baseUrl/actuator/health\").body()\n"
         "}\n";
  const auto result = cgraph::run_one_shot(root);
  const auto& graph = result.graph;
  const auto node = [&](std::string_view id) {
    return std::ranges::find_if(graph.nodes, [&](const cgraph::Node& n) { return n.id == id; });
  };
  const auto has = [&](std::string_view source, std::string_view target, std::string_view relation) {
    return std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
      return edge.source == source && edge.target == target && edge.relation == relation;
    });
  };
  const auto health = node("endpoint:GET /actuator/health");
  const std::string exposure = "svc_src_main_resources_application_yml_actuator_exposure_0";
  if (health == graph.nodes.end() || health->properties.contains("served") || node(exposure) == graph.nodes.end()) {
    std::filesystem::remove_all(root);
    return fail("pipeline did not mint a served endpoint:GET /actuator/health and the exposure node");
  }
  if (!has(health->id, exposure, "handled_by") || !health->source_location || health->source_location->start_line != 8 ||
      !has("svc_src_main_resources_application_yml", health->id, "contains")) {
    std::filesystem::remove_all(root);
    return fail("the endpoint is handled_by the exposure node at the include line, contained by the config file");
  }
  const bool consumed = std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
    return edge.target == health->id && edge.relation == "CONSUMES";
  });
  if (!consumed) {
    std::filesystem::remove_all(root);
    return fail("the Ktor client call consumes the served endpoint");
  }

  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "k8s");
  std::ofstream(root / "k8s" / "deployment.yaml") << "livenessProbe:\n  httpGet:\n    path: /actuator/health\n";
  std::ofstream(root / "README.md") << "Add spring-boot-starter-actuator and call /actuator/health.\n";
  std::ofstream(root / "build.gradle.kts") << "dependencies {\n    implementation(\"com.example:lib:1.0\")\n}\n";
  const auto docs_only = cgraph::run_one_shot(root);
  std::filesystem::remove_all(root);
  if (std::ranges::any_of(docs_only.graph.nodes, [](const cgraph::Node& n) {
        return n.kind == "endpoint" || n.kind == "spring_actuator" || n.kind == "actuator_exposure";
      })) {
    return fail("a repo that only mentions the actuator serves no endpoint");
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += test_detects_application_config_only();
  failures += test_build_file_declares_the_actuator();
  failures += test_default_exposure_is_health();
  failures += test_exposure_rules();
  failures += test_profiles_and_precedence();
  failures += test_pipeline_serves_and_does_not_over_link();
  return failures == 0 ? 0 : 1;
}
