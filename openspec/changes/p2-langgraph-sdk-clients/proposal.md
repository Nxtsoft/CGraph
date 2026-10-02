# LangGraph SDK client calls consume the Agent Server routes

## Why

Gap T15 of the cross-service plan (`~/.agents/artifacts/2026-09-29/cgraph-multiservice-plan/plan.md`, Phase 2). turing-webapp talks to the turing-agents Agent Server only through `@langchain/langgraph-sdk`'s `Client`: `lib/langgraph-client.ts` exports `createLangGraphClient(accessToken, service)` returning `new Client({ apiUrl, defaultHeaders })`, and the hooks call `client.runs.stream(threadId, 'luna', {...})`, `client.threads.create()`, `client.threads.getState(threadId)` and so on. None of those calls is a `fetch` or a `<receiver>.<verb>` call, so they produced no `CONSUMES` edge, and `POST /threads/{}/runs/stream`, which #144 serves from turing-agents' `langgraph.json`, stayed unlinked.

## What Changes

- **Extractor (`http_consumers.cpp`).** A table maps every REST method of `Client.assistants`, `.threads`, `.runs`, `.crons` and `.store` to the verb and path `@langchain/langgraph-sdk` 1.11.1 (turing-webapp's locked version) sends, read from `dist/client/*/index.js`. Methods that branch on an argument follow the SDK's own test (`threadId == null` for `runs.stream`/`runs.wait`, `=== null` for `runs.create`, `!= null` for `runs.joinStream`; `getState`'s checkpoint; `getSubgraphs`' namespace). A receiver is read as a client only when it provably is one, reusing the URL reader's machinery (`local_values`, `module_const_value`, `class_member_value`, `module_function`, `function_return_values`): `new Client(...)` with `Client` imported from the SDK, held in a local set once, a module constant or a `this.x` field, or returned by a same-file function. Such calls record `http_call` labelled `client.runs.stream`. A client returned by an imported function records `langgraph_call` naming that function, and a module function whose every return is `new Client(...)` records `langgraph_client`.
- **Contracts (`contracts.cpp`).** A `langgraph_call` is read as a client call (step 6) when its function resolves through the file's imports to a function with a `langgraph_client` fact; otherwise it is neither an edge nor a count.
- **Index key** `cgraph-index-v1:logic-12`: the facts changed.

## Non-goals

- A parameter typed `Client` (`function f(client: Client)`, turing-webapp `lib/hooks/use-luna-stream.ts` helpers) is not read as a client: a type says nothing about where the value came from. Neither is a `let` reassigned after its declaration (`let client = null; ... client = createLangGraphClient(token, 'ic')` in `lib/hooks/use-ic-stream.ts`).
- `threads.stream` (the v2 protocol, `POST /threads/{}/stream/events` and `/commands` through a transport adapter) and the `~ui` client are not mapped.
- Paths are the SDK's, whether or not the Agent Server serves them (`GET /threads/{}/stream`, `PATCH /threads/{}/state`, `POST /threads/prune`, `POST /runs/cancel`, the `crons` routes): a call the server does not serve mints an unserved endpoint, as any client call does.
