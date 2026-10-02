#pragma once

#include "cgraph/extractor.hpp"
#include "cgraph/language_config.hpp"

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

// Spring Boot Actuator: web endpoints a framework serves, declared by the build
// file and the application config rather than by code.
//
// A Spring Boot application whose build depends on
// `org.springframework.boot:spring-boot-starter-actuator` serves management
// endpoints over HTTP (`GET /actuator/health`, ...). No handler exists in the
// repo's own source, so a client calling `$baseUrl/actuator/health` would
// otherwise meet no provider. Which endpoints are served is decided across
// files: the build file says the module is a web application with the actuator
// on its classpath, and `application[-<profile>].{yml,yaml,properties}` under the
// module's `src/main/resources` says which endpoints are exposed, where.
//
// Extraction records facts per file, so the incremental path stays correct:
//
//   build file    (`build.gradle.kts`, `build.gradle`, `pom.xml`) beside its own
//                 grammar's extraction: one `spring_actuator` node at the
//                 actuator dependency, `contains`-ed by the file, and an
//                 `actuator_app` fact (context: JSON module dir, web
//                 starters, whether the Prometheus registry and the cache
//                 module are dependencies, the Boot major version the plugin or
//                 parent declares). Maven `<profiles>`, `<pluginManagement>`
//                 alone, a `<packaging>pom</packaging>` aggregator and Gradle
//                 `apply false` do not make an application.
//   config file   its `file` node, one `actuator_exposure` node per document
//                 that sets `management.endpoints.web.exposure.include`, and an
//                 `actuator_config` fact per document holding the keys that
//                 decide the served paths (context: JSON).
//
// resolve_contracts asks spring_actuator_routes for the `file_route` facts these
// imply and mints the endpoints like any other route. An endpoint is handled by
// the exposure node of the document whose `include` won, else (the default
// exposure) by the build file's `spring_actuator` node. The rules follow the
// Spring Boot 4.1 reference (actuator/endpoints, actuator/monitoring):
//
//   * Only `health` is exposed over HTTP by default. `exposure.include` /
//     `exposure.exclude` are comma lists (or YAML lists) of endpoint ids, `*`
//     selects every endpoint, and exclude wins.
//   * Access: `management.endpoint.<id>.access` (none / read-only /
//     unrestricted), `management.endpoints.access.default`, capped by
//     `management.endpoints.access.max-permitted`; the pre-3.4 `enabled` /
//     `enabled-by-default` booleans read as unrestricted / none. `shutdown` and
//     `heapdump` default to none. Read-only keeps only the GET operations.
//   * Availability: `prometheus` needs `micrometer-registry-prometheus` (not
//     `-simpleclient`), `caches` the spring-boot-cache module (Boot 4.1
//     auto-configures it there); `spring.main.web-application-type: none` runs
//     no web server.
//   * Paths: `management.endpoints.web.base-path` (default `/actuator`, `/`
//     meaning the root) plus `path-mapping.<id>`. Where they sit follows
//     ManagementPortType.get (Boot 4.1.1): a negative `management.server.port`
//     serves nothing; the main server when it is unset, is 8080 with no
//     `server.port`, or is not 0 and equals `server.port`, under
//     `server.servlet.context-path` (servlet) or `spring.webflux.base-path`
//     (reactive); otherwise a separate server under `management.server.base-path`.
//     A non-integer port fails startup. The discovery page answers
//     `GET <base-path>` unless the base path is the root or
//     `management.endpoints.web.discovery.enabled` is false.
//   * Health also answers `GET <health>/{*path}`, and each configured group
//     (`management.endpoint.health.group.<name>`) at `GET <health>/<name>`, plus
//     `liveness` and `readiness` when the probes are on: by default on Boot 4
//     (AvailabilityProbesAutoConfiguration, `matchIfMissing = true`) unless
//     `management.endpoint.health.probes.enabled` is false; on Boot 3 only when
//     it is true; off when the build names no Boot version.
//   * Profiles: the base documents (no profile) are one configuration; each
//     profile named by a file (`application-prod.yml`) or a document
//     (`spring.config.activate.on-profile`) is the base overlaid by that
//     profile's documents. `.properties` wins over YAML at one location, a
//     profile file over the base, a later document over an earlier one. The
//     endpoints served are the union over these configurations: any of them is a
//     way the code can run.
//
// Conservatively NOT modeled (no endpoint rather than a wrong one): endpoints
// whose availability needs a bean or dependency other than the Prometheus
// registry and the cache module (flyway, liquibase, quartz, sessions, integrationgraph,
// httpexchanges, auditevents, startup, logfile, sbom, custom `@Endpoint`s);
// Jersey-only applications; `spring.mvc.servlet.path` other than `/` (a
// configuration setting it serves nothing); a value whose `${...}` placeholder
// has no default (that configuration serves nothing); version-catalog
// dependencies (`libs.spring.boot.starter.actuator`) and dependencies declared in
// `subprojects`/`allprojects` blocks; profile expressions (`prod & !cloud`) and
// combinations of several active profiles; `spring.profiles.active`/groups;
// config outside `src/main/resources` (`config/`, environment variables, command
// line); a YAML document whose anchor, tag, merge key or flow mapping may hold a
// deciding key (the configurations it may apply to serve nothing); flow
// sequences nested deeper than 8; health component paths and additional paths
// (`/livez`); Boot 3 probes enabled by Kubernetes detection; Boot 3's unrestricted
// `heapdump` default; ports, which the graph's endpoint ids do not carry.
namespace cgraph {

// `application.yml`, `application-<profile>.properties`, ... directly under a
// `src/main/resources` directory.
[[nodiscard]] bool is_spring_application_config(const std::filesystem::path& path);

// `build.gradle.kts`, `build.gradle` or `pom.xml`.
[[nodiscard]] bool is_spring_build_file(std::string_view relative_path);

// Appends the actuator node and `actuator_app` fact to a build file's own
// extraction when the build declares a Spring Boot web application with the
// actuator starter. Adds the file node when the build file's extractor made none.
void append_spring_actuator_facts(const ExtractionContext& context, ExtractionResult& result);

[[nodiscard]] ExtractionResult extract_spring_application_config(const ExtractionContext& context);

// The text of a YAML line before its `#` comment (at the start or after
// whitespace, outside a quoted scalar).
[[nodiscard]] std::string_view strip_yaml_comment(std::string_view line);

// The first `${...}` placeholder at or after `from` in a config value: `open`
// is npos when there is none, `close` (the first `}` after it) npos when it is
// never closed, `inner` the text between them (`NAME` or `NAME:default`; a
// nested default leaves the inner placeholder unclosed in it).
struct SpringPlaceholder {
  std::size_t open = std::string_view::npos;
  std::size_t close = std::string_view::npos;
  std::string_view inner;
};
[[nodiscard]] SpringPlaceholder find_spring_placeholder(std::string_view value, std::size_t from = 0);

// The `file_route` facts the `actuator_app` and `actuator_config` facts imply,
// deterministic in order: modules by directory, the base configuration first.
[[nodiscard]] std::vector<RawRelation> spring_actuator_routes(std::span<const RawRelation> raw_relations);

}  // namespace cgraph
