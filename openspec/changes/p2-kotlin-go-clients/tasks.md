## 1. Extraction

- [x] 1.1 `ClientUrl`: literal / host / segment parameter / wrapper tail / call-built reduction, absolute vs base-relative.
- [x] 1.2 Kotlin `kotlin_http_walk`: Ktor verb calls on client-named receivers, `request` unresolved, wrapper definitions and bare-name wrapper calls.
- [x] 1.3 Go `go_http_walk`: net/http entry points, method-then-URL client calls, wrapper definitions and wrapper calls by field name.

## 2. Tests

- [x] 2.1 Kotlin and Go fact tests and the Spring end-to-end test in `configured_extractors_test.cpp`; fail on origin/main (exit 9).

## 3. Measurement and docs

- [x] 3.1 Probe: M15-M21, M24-M27 link; no lost links; precision sample of 20.
- [x] 3.2 README and SKILL.md describe Kotlin and Go consumers.
