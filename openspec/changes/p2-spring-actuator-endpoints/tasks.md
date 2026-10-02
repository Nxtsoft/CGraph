## 1. Facts

- [x] 1.1 `detect_language` recognizes `application[-profile].{yml,yaml,properties}` under `src/main/resources` (`spring-config`).
- [x] 1.2 `append_spring_actuator_facts`: Gradle (Kotlin, Groovy) and Maven builds; plugin/parent, web starter, runtime actuator dependency, Prometheus registry.
- [x] 1.3 `extract_spring_application_config`: YAML subset and properties, relaxed keys, placeholders with defaults, per-document facts, `actuator_exposure` nodes.

## 2. Resolution

- [x] 2.1 `spring_actuator_routes`: exposure, access, base path, path mapping, ports, context path, discovery, health groups and probes, profile configurations and precedence.
- [x] 2.2 `resolve_contracts` mints the derived `file_route` facts; `resolve_raw_relations` skips the new facts.
- [x] 2.3 Index version `logic-12`.

## 3. Verification

- [x] 3.1 `spring_actuator_test.cpp`, `detect_test.cpp`; fail before (wiring reverted) and pass after.
- [x] 3.2 Full suite; probe graphs: `CONSUMES` and served endpoints diffed against bin-v0.7.1; seam scoring for both systems.
- [x] 3.3 README.

## 4. Review round 1 (PR #154)

- [x] 4.1 `caches` needs `spring-boot-starter-cache` / `spring-boot-cache` (Boot 4.1 `CachesEndpointAutoConfiguration` lives in spring-boot-cache).
- [x] 4.2 Probes follow the declared Boot version: Boot 4 on unless `probes.enabled` is false, Boot 3 only when true, unknown off.
- [x] 4.3 Flow sequences nest at most 8 deep; `cgraph_fuzz_spring_config` fuzzer.
- [x] 4.4 Only `micrometer-registry-prometheus` itself backs `prometheus`.
- [x] 4.5 Management port mirrors `ManagementPortType.get` (any negative disables; 0 is DIFFERENT; non-integer fails startup).
- [x] 4.6 An anchor, tag, merge key or flow mapping that may hold a deciding key makes the document unreadable; its configurations serve nothing.
- [x] 4.7 Maven `<profiles>`, `<pluginManagement>`-only, `<packaging>pom</packaging>` and Gradle `apply false` are not applications.
- [x] 4.8 Facts serialize with the replace error handler (non-UTF-8 bytes).
- [x] 4.9 `spring.main.web-application-type: none` serves nothing.
- [x] 4.10 List appends and Gradle block lookups are linear.
