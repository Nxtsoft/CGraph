## MODIFIED Requirements

### Requirement: Call resolution is measurable from a committed artifact
`BuildStats` SHALL report, per build, `raw_calls_total` and a partition of it: `resolved_same_file`, `resolved_project_unique`, `resolved_member_method`, `dropped_unknown`, `dropped_ambiguous`, `dropped_self`, `dropped_scope_mismatch`, `dropped_library_member`, and `dropped_closure_guess`. The partition SHALL sum to `raw_calls_total`, and every field SHALL be serialized to `stats.json`. `resolved_overload_first` and `resolved_qualifier_unchecked` SHALL also be reported, each a subset of the resolved fields.

#### Scenario: The resolution rate is readable without instrumenting a build
- **WHEN** `cgraph --root PATH --out DIR` completes
- **THEN** `DIR/stats.json` contains every field
- **AND** the nine partition fields sum to `raw_calls_total`

### Requirement: An HTTP route's inline handler is a function node and a call scope

In JavaScript and TypeScript, the last function-valued argument of a call whose callee is `<receiver>.<verb>` with `<verb>` one of `get`, `post`, `put`, `patch`, `delete`, `head`, `options`, `all`, and whose first argument is a string or template literal, SHALL be extracted as a `function` node whose source location is the handler's own extent, contained by the enclosing scope, and SHALL be the caller of every call in its body. Its label SHALL be `<root>.<verb> <path>`, where `<path>` is the literal's text without quotes and `<root>` is the identifier at the base of the callee's fluent chain, or, when the chain is rooted in a constructor or other non-identifier, the name of the variable the chain is assigned to; when neither exists the label is `<verb> <path>`. Its id SHALL be `make_id(<project-relative source path> + ":" + label)`, subject to the existing id-collision guard. Function-valued arguments before the handler SHALL remain anonymous. Every other nested anonymous function SHALL remain a boundary as before (no node, no function scope; its calls are attributed as "Calls inside closures keep a caller" requires), and a module-level const holding such a chain SHALL still be extracted as a `variable` node.

#### Scenario: An Elysia chain yields one handler per route
- **GIVEN** `const notebookRoutes = new Elysia({prefix: '/notebooks'}).use(auth).get('/', async ({dbUser}) => { return listNotebooks(dbUser); }, {detail}).post('/:id/notes', async (c) => { return createNote(c); });`
- **THEN** function nodes `notebookRoutes.get /` and `notebookRoutes.post /:id/notes` exist, each
  spanning its arrow, and raw calls `listNotebooks` and `createNote` name them as callers
- **AND** the `variable` node `notebookRoutes` is still emitted

#### Scenario: A chain on an identifier is rooted at that identifier
- **GIVEN** `app.get('/health', (req, res) => res.send(ping()));`
- **THEN** a function node `app.get /health` exists and is the caller of `ping`

#### Scenario: Middleware before the handler stays anonymous
- **GIVEN** `app.post(`/users`, authenticate, (req, res) => { save(req.body); });`
- **THEN** exactly one function node, `app.post /users`, is emitted and it is the caller of `save`

#### Scenario: Non-route callbacks stay boundaries
- **GIVEN** `app.use('/static', (req, res, next) => next());`, `router.route('/x').get((req, res) => res.end());`, `list.map(x => transform(x));`, `describe('suite', () => { run(); });`
- **THEN** no function node is emitted for any of those arrows
- **AND** their calls `next`, `transform` and `run` are attributed to the file as closure calls (written as `const items = list.map(x => transform(x));`, `transform` is attributed to the `variable` node `items`)

## ADDED Requirements

### Requirement: Calls inside closures keep a caller
In JavaScript and TypeScript, a call inside an anonymous function that is not itself a call scope (an arrow that is neither a module-level `const f = () => {}` nor an HTTP route's inline handler) SHALL be attributed to the nearest enclosing function or method. With none, it SHALL be attributed to the enclosing class for a class-field arrow, else to the module-level variable whose initializer holds the closure when the extractor made a node for it, else to the file. Such an arrow SHALL still emit no node and SHALL NOT be a function scope for any other extraction. A call attributed this way SHALL resolve only on evidence: a declaration in the caller's own file, an `imports` / `imports_from` edge from the caller's file, or a receiver naming the owning class. A project-wide unique label without import evidence and a unique method for an unknown receiver SHALL NOT bind it; each such refusal SHALL be counted as `dropped_closure_guess`. Calls outside closures SHALL resolve as before.

#### Scenario: Effect closures in a module-level service object
- **GIVEN** `export const ServiceLive = Layer.succeed(Tag, { merge: (id) => Effect.tryPromise({ try: async () => { await assertAccess(id); }, catch: (e) => toError(e) }) })`, `assertAccess` imported and `toError` declared in the same file
- **THEN** `ServiceLive` CALLS `assertAccess` and `toError`, and no node is emitted for `merge`, `try` or `catch`

#### Scenario: Test callbacks
- **GIVEN** a test file with `describe('x', () => { it('y', async () => { expect(await assertAccess('a')).toBe('a'); }); })` and `assertAccess` imported
- **THEN** the test file's node CALLS `assertAccess`

#### Scenario: Closures inside a function and a class field
- **GIVEN** `function load(ids) { return Promise.all(ids.map((id) => assertAccess(id))).then(() => audit('load')); }` and `class Handler { handle = (id) => assertAccess(id); }`
- **THEN** `load` CALLS `assertAccess` and `audit`, and `Handler` CALLS `assertAccess`

#### Scenario: Names a closure is handed are not guessed
- **GIVEN** closures calling a parameter `reject(...)`, a parameter `onDone()`, a destructured prop `onOpenChange(false)` and `cache.delete(key)` on a `Map`, and another file, not imported, declaring functions `reject`, `onDone`, `onOpenChange` and a class method `delete`
- **THEN** none of those calls produces a CALLS edge, and `dropped_closure_guess` is 4
