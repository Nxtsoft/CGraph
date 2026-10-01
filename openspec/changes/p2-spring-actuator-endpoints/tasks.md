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
