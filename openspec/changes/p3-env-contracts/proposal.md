# Environment variable reads become env contract uses (Phase 3.4)

## Why

Five probe edges are base URLs and shared config values read from environment variables: T36 (`NEXT_PUBLIC_API_URL` in turing-webapp addresses turing-api), T37 (`ML_BACKEND_BASE_URL` in turing-api's typed config addresses ml-backend), M38 (`BACKEND_URL` in idp-front-end addresses idp), and T40 (`AUTH0_AUDIENCE`, read by both turing-webapp and turing-api). Code alone cannot join them: only a port or host matches, and `localhost:8080` appears in 7 of the 8 probe repos. The foundation (Phase 3.0) made the join declared: a manifest `env` entry or `--env NAME=service` says which member a variable addresses, and `env:<NAME>` crosses only then. No extractor emitted `env:` facts, so nothing could join.

## What Changes

- A new engine file `env_contracts.cpp` turns every literal environment variable read into a `uses_contract` fact `env:<NAME>` (case as written) from the reading symbol: the enclosing function, else a module-level variable the extractor already made a node for, else the file. One fact per reading symbol and name.
  - JavaScript / TypeScript: `process.env.X`, `process.env['X']`, `import.meta.env.X`, `Bun.env.X`, `const { X, Y: y, Z = d } = process.env`; and `e.X` / `const { X } = e` for an upper snake case X where `e` is a `const` holding an env object, a call that takes one as an argument (`Value.Decode(schema, process.env)`, `schema.parse(process.env)`), or a call of a same-file function whose every return is such a `const` (`const env = loadConfig()`). Writes (`=`, `+=`, `++`, `delete`) are not reads; a `let`, a shadowing parameter and a lower-case member prove nothing.
  - Python: `os.environ["X"]` (not as an assignment or `del` target), `os.environ.get("X")`, `os.getenv("X")`.
  - Go: `os.Getenv("X")`, `os.LookupEnv("X")`.
  - Kotlin / Java: `System.getenv("X")`; `@Value("${X}")` / `@Value("${X:default}")` for an upper snake case X.
  - Spring application config (`application*.yml|yaml|properties`): `${X}` / `${X:default}` placeholders in values, upper snake case, outside comments, from the config file's node.
- Hook-in calls only in the existing extractors: `js_extra_walk`, `python_extra_walk`, `kotlin_http_walk`, `go_extra_walk`, a new Java `extra_walk`, and the SpringConfig dispatch in `non_grammar_extractors.cpp`.
- Index version `logic-15` (the orchestrator re-bumps at merge).

## Design choices

- **Declared only.** Nothing in this change is a provider. The foundation's `env_provider_of` makes the declared member the provider (seam discover adds `SERVED_BY service:<member>`), so a declaration `NEXT_PUBLIC_API_URL=turing-api` is what makes webapp's reads meet turing-api. Undeclared names stay in their repo (`is_bridged_contract` is false for every `env:` id).
- **Typed env objects must be provable.** turing-api reads its config through `const env = Value.Decode(envSchema, process.env)` inside `loadConfig()` and `const env = loadConfig()` at module level. A schema decode of the env keeps the env's keys, so a `const` bound to a call taking `process.env` is an env object; one same-file function hop covers `loadConfig()`. Only upper snake case members count there, so a method or a derived field is never read as a variable.
- **Upper snake case where syntax does not say "env".** `${server.port}` and `@Value("${sentra.saml.allowed-domains}")` are Spring property keys, not environment variables; `${SAML_IDP_BASE_URL:...}` is the env-variable convention.
- **Reading symbol.** A module-level `const API = process.env.X || '...'` has no node (the extractor makes nodes for object, array, call and `new` initializers only), so its read belongs to the file; this change adds no nodes, so no existing node or edge can change.
- **M36 and M37 are not env reads.** passless-app's `const val DEV_API_BASE_URL = "https://dev-idp.cubeless.io"` and passless-cli's `const DefaultBaseURL = "https://dev-api.passless.net"` are host literals; the only link to idp is a host in a Spring default. Joining by host would over-link (the plan's reason for declaring), so they stay unlinked.

## Non-goals

- pydantic `BaseSettings` fields: the env name depends on `env_prefix`, aliases and `case_sensitive` configured elsewhere, so the field name alone proves nothing.
- Names that are not literals (`os.Getenv(EnvClientID)`, `process.env[key]`), `from os import environ` aliases, envsubst-style `${X}` in Kubernetes templates, `.env` files and docker-compose.
- Host-literal base URLs (M36, M37).
- Editing workspace manifests: the declarations are proposed in the PR.

## Impact

- `src/engine/env_contracts.cpp`, `src/engine/include/cgraph/env_contracts.hpp`, `tests/smoke/env_contracts_test.cpp`.
- One-line hooks in `javascript_extractor.cpp`, `python_extractor.cpp`, `configured_extractors.cpp`, `non_grammar_extractors.cpp`; `index_persistence.cpp` `logic-15`.
