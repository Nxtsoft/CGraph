## MODIFIED Requirements

### Requirement: Environment variable reads become env contract uses
Extraction SHALL record a `uses_contract` fact with context `env:<NAME>` (the name as written) for every read of an environment variable whose name the code spells as a literal, from the reading symbol: the enclosing function, else the module-level variable whose initializer holds the read when the extractor made a node for it, else the file; at most one fact per reading symbol and name. Reads SHALL be: in JavaScript and TypeScript `process.env.X`, `process.env['X']`, `import.meta.env.X`, `Bun.env.X` and `const { X } = process.env` (renamed and defaulted keys included), and `e.X` or `const { X } = e` for an upper snake case `X` where `e` is a `const` bound to an env object, to a call taking an env object as an argument, or to a call of a same-file function whose every return is such a `const`; in Python `os.environ["X"]`, `os.environ.get("X")` and `os.getenv("X")`; in Go `os.Getenv("X")` and `os.LookupEnv("X")`; in Kotlin and Java `System.getenv("X")` and `@Value` placeholders `${X}` / `${X:default}`; in Spring application config files `${X}` / `${X:default}` placeholders outside comments, from the file's node. In placeholders and typed env objects only upper snake case names (`[A-Z][A-Z0-9_]*`) SHALL count. Assignments, compound assignments, increments and deletions of a variable SHALL NOT be reads, nor SHALL a computed name, nor a member of a name that any binding nearer than the env `const` shadows (a `let` or `var`, a parameter, a for-head, a `catch` parameter, a class, an enum, an import, a destructured `const`; a `let`, `const` or `var` directly in any case of a `switch` binds for the whole switch). Spring YAML comments and placeholders SHALL be read by the Spring config reader's own comment and placeholder rules. Extraction SHALL record no provider for an env variable: a declaration names it.

#### Scenario: Typed config reads through a schema-decoded env
- **GIVEN** `function loadConfig() { const env = Value.Decode(envSchema, process.env); return env }`, `const env = loadConfig()` and `export const config = { ml: { url: env.ML_BACKEND_BASE_URL, mode: env.mode } }`
- **THEN** `config` uses `env:ML_BACKEND_BASE_URL` and nothing uses `env:mode`

#### Scenario: A nearer binding shadows a typed env const
- **GIVEN** a module-level `const env = loadConfig()` that is an env object, and functions reading `env.X` where `env` is rebound by `let env = p.cfg`, `for (const env of list)`, `catch (env)`, a hoisted `var env`, `const { env } = p` or a local `class env`
- **THEN** none of those reads is a fact, and a function reading the module `env.MODULE_READ` uses `env:MODULE_READ`

#### Scenario: A switch case declaration and a local enum shadow a typed env const
- **GIVEN** a module-level `const env = loadConfig()` that is an env object, a function with `switch (k) { case 1: let env = p.cfg; return env.X }`, a function declaring `const env = p.cfg` in one case and reading `env.Y` in another case of the same switch, and a function with `enum env { Z }` reading `env.Z`
- **THEN** none of those reads is a fact, while `env.W` read inside a switch that binds no `env`, or after a switch whose inner braced block binds one, uses `env:W`

#### Scenario: Direct, destructured and subscript reads, and writes
- **GIVEN** a function with `const { REDIS_URL, PORT: port } = process.env`, `process.env['DATABASE_URL']`, `process.env.DATABASE_URL`, `process.env.WRITTEN = '1'`, `delete process.env.DELETED` and `process.env[key]`
- **THEN** the function uses `env:REDIS_URL`, `env:PORT` and `env:DATABASE_URL` once each, and nothing uses `env:WRITTEN` or `env:DELETED`

#### Scenario: A module constant without a node reads from the file
- **GIVEN** `const API_BASE_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080'` at module level
- **THEN** the file uses `env:NEXT_PUBLIC_API_URL`

#### Scenario: Other languages
- **GIVEN** Python `os.environ["OPENAI_API_KEY"]` and `os.environ["WRITTEN"] = "1"`, Go `os.Getenv("PASSLESS_CONTRACT_BASE_URL")` and `os.Getenv(EnvClientID)`, Kotlin `System.getenv("SPRING_PROFILES_ACTIVE")` and `@Value("\${sentra.saml.allowed-domains:}")`, Spring YAML `base-url: ${SAML_IDP_BASE_URL:https://x}  # was ${OLD_URL}` and `message: Can't reach ${API_URL} # was ${OLD_URL}`
- **THEN** facts exist for `OPENAI_API_KEY`, `PASSLESS_CONTRACT_BASE_URL`, `SPRING_PROFILES_ACTIVE`, `SAML_IDP_BASE_URL` and `API_URL` only

#### Scenario: Only a declaration makes an env read cross repositories
- **GIVEN** turing-webapp reading `NEXT_PUBLIC_API_URL` and a seam declaring `--env NEXT_PUBLIC_API_URL=turing-api`
- **THEN** `env:NEXT_PUBLIC_API_URL` is `SERVED_BY service:turing-api` and consumed by turing-webapp; without the declaration no `env:` node enters the seam
