# Spring request mappings become endpoints

## Why

CGraph mints endpoints from JavaScript and TypeScript route registrations and from Next.js route files. It mints none for Spring MVC, the dominant JVM web framework, whose routes live in annotations. The area 9 measurement (`~/.agents/artifacts/2026-09-25/cgraph-context-engine-research/briefs/area9-config-routing/results.md`) found 372 annotated routes in idp-core, a Kotlin Spring service, and 0 endpoint nodes in its graph. 362 of the 372 are independently listed in the service's own springdoc OpenAPI export. So `impact` on a controller method never reaches the route it serves or the clients that call it.

## What Changes

- The Kotlin and Java extractors gain a relation handler. For each method annotated `@GetMapping`, `@PostMapping`, `@PutMapping`, `@DeleteMapping` or `@PatchMapping`, or `@RequestMapping` with a `method`, it emits one `file_route` fact per (verb, path). The path is the nearest enclosing class's `@RequestMapping` prefix joined with the method's own path. These facts reuse the absolute-path branch `resolve_contracts` already has for Next.js route files, so endpoint minting, `contains` and `handled_by` edges, and `CONSUMES` linking from clients are unchanged.
- Paths are read from string literals only, in positional, `value =` and `path =` forms, including arrays: Kotlin `[...]` or `arrayOf(...)`, Java `{...}`. A constant, a string template, or a method-level `@RequestMapping` without a `method` mints nothing, because a wrong endpoint is worse than none.

## Contract that tests verify

- A Kotlin controller with a class prefix mints one endpoint per mapped method and path, each `handled_by` its method. `@GetMapping` with no path serves the prefix itself. Extra arguments such as `produces` are ignored.
- `@RequestMapping(value = ["/a", "/b"], method = [RequestMethod.PUT])` mints `PUT` endpoints for both paths.
- A Java controller with `@RequestMapping(path = ...)`, `value = {"", "/all"}` and `method = RequestMethod.POST` mints the same way.
- A class with no `@RequestMapping` uses the method path as is.
- A constant path and a method-less method-level `@RequestMapping` mint nothing.
- A TypeScript `fetch` of the path gets a `CONSUMES` edge to the Kotlin endpoint.

## Non-goals

- Resolving path constants (`@GetMapping(ApiPaths.USERS)`); idp-core uses none.
- Mappings declared on interfaces and inherited by controllers.
- Spring WebFlux router functions and Ktor routing DSLs.
- Controller detection: the mapping annotation alone decides.

## Impact

- `src/engine/configured_extractors.cpp`: `kotlin_relation_handler` and `java_relation_handler`, plus a text parser for mapping annotations shared by both.
- `tests/smoke/contracts_test.cpp` (the pipeline to endpoints) and `tests/smoke/configured_extractors_test.cpp` (the emitted facts).
- `openspec/specs/contract-discovery`: one added requirement.
- `graph.json` gains endpoint nodes for JVM repositories that use Spring. JavaScript and TypeScript output is unchanged.
