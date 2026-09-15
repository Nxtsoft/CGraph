## ADDED Requirements

### Requirement: An HTTP route's inline handler is a function node and a call scope

In JavaScript and TypeScript, the last function-valued argument of a call whose callee is `<receiver>.<verb>` with `<verb>` one of `get`, `post`, `put`, `patch`, `delete`, `head`, `options`, `all`, and whose first argument is a string or template literal, SHALL be extracted as a `function` node whose source location is the handler's own extent, contained by the enclosing scope, and SHALL be the caller of every call in its body. Its label SHALL be `<root>.<verb> <path>`, where `<path>` is the literal's text without quotes and `<root>` is the identifier at the base of the callee's fluent chain, or, when the chain is rooted in a constructor or other non-identifier, the name of the variable the chain is assigned to; when neither exists the label is `<verb> <path>`. Its id SHALL be `make_id(source_file + ":" + label)`, subject to the existing id-collision guard. Function-valued arguments before the handler SHALL remain anonymous. Every other nested anonymous function SHALL remain a boundary as before, and a module-level const holding such a chain SHALL still be extracted as a `variable` node.

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
- **THEN** no function node is emitted for any of those arrows and their calls are dropped
