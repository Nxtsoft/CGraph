## ADDED Requirements

### Requirement: Spring request mappings become endpoints
In Kotlin and Java, a method annotated `@GetMapping`, `@PostMapping`, `@PutMapping`, `@DeleteMapping` or `@PatchMapping`, or `@RequestMapping` with a `method` argument, SHALL mint one endpoint per (verb, path). Each endpoint SHALL be `handled_by` the method, and its path SHALL be the nearest enclosing class's `@RequestMapping` path joined with the method's path. A mapping with no path SHALL serve the class path itself. Paths SHALL be read only from string literals, given positionally or as `value =` or `path =`, singly or as an array. Only a method whose nearest enclosing type is a concrete class SHALL mint. A method of an interface (including a `@FeignClient`, which calls the route rather than serving it), an object or companion object, or a top-level function SHALL mint nothing. The first Spring HTTP mapping on a method decides; other `*Mapping` annotations such as `@MessageMapping` SHALL be ignored. Several positional path arguments SHALL each mint. A path that is not exactly one plain string literal (a constant, a template, a concatenation, a raw string), and a method-level `@RequestMapping` without `method`, SHALL mint no endpoint.

#### Scenario: A Kotlin controller under a class prefix
- **GIVEN** `@RequestMapping("/api/v1/users")` on a class whose methods carry `@GetMapping`, `@GetMapping("/{id}", produces = [...])` and `@RequestMapping(value = ["/sync", "/resync"], method = [RequestMethod.PUT])`
- **THEN** endpoints `GET /api/v1/users`, `GET /api/v1/users/{id}`, `PUT /api/v1/users/sync` and `PUT /api/v1/users/resync` exist, each `handled_by` its method, and `GET /api/v1/users/{id}` has id `endpoint:GET /api/v1/users/{}`

#### Scenario: A Java controller with named arguments
- **GIVEN** `@RequestMapping(path = "/api/orders")` on a class with `@GetMapping(value = {"", "/all"})` and `@RequestMapping(value = "/legacy", method = RequestMethod.POST)`
- **THEN** endpoints `GET /api/orders`, `GET /api/orders/all` and `POST /api/orders/legacy` exist

#### Scenario: Unreadable mappings mint nothing
- **GIVEN** `@GetMapping(ApiPaths.EXPORT)` and a method-level `@RequestMapping("/any")` with no `method`
- **THEN** neither mints an endpoint

#### Scenario: A client consumes a Spring endpoint
- **GIVEN** a Kotlin `@GetMapping` serving `/api/v1/users` and `fetch('/api/v1/users')` in a TypeScript function
- **THEN** the TypeScript function has a `CONSUMES` edge to that endpoint

#### Scenario: Interfaces and non-class scopes serve nothing
- **GIVEN** a `@FeignClient` interface and a plain interface whose methods carry `@GetMapping`, a Java interface under `@RequestMapping("/api/v2")`, a `companion object`, a nested `object`, and a top-level function, each with a mapping
- **THEN** none of them mints an endpoint

#### Scenario: Shapes that still mint
- **GIVEN** `@GetMapping("/one", "/two")`, `@MessageMapping("/ws")` above `@GetMapping("/after-message")`, `@GetMapping(value = arrayOf("/arr"))` and a fully qualified `@org.springframework.web.bind.annotation.PostMapping("/fq")` under `@RequestMapping("/v")`, plus a Java record controller
- **THEN** `GET /v/one`, `GET /v/two`, `GET /v/after-message`, `GET /v/arr` and `POST /v/fq` exist, while `@GetMapping("/a" + "/b")` and a raw-string path mint nothing
