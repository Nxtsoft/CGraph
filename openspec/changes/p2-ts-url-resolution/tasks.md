## 1. Extractor

- [x] 1.1 Locals read through their value at the host position; per-branch `let` values; `new URL`, `.toString()`, `.href`.
- [x] 1.2 Same-file builders and query helpers; class-field prefixes; path-fallback fields unresolvable.
- [x] 1.3 Axios `baseURL`, same-file class-method and forwarding wrappers, slash-stripping wrappers.
- [x] 1.4 Positional wrapper facts (`@j[=DEFAULT] <prefix> #i`) and `http_call_args` for calls into them; ternary method choices.
- [x] 1.5 Destructured parameters keep their position and shadow outer locals.

## 2. Contracts

- [x] 2.1 Positional wrappers are filled from `http_call_args`; a method parameter's default applies when a call leaves it out.
- [x] 2.2 A relative path joins only a prefix ending in `/`; otherwise counted unresolved.

## 3. Verification

- [x] 3.1 Extractor and contracts tests fail against the code before this change and pass after.
- [x] 3.2 Full default suite green (one hardware segfault in `cgraph_pack_context_parity_test` passed 3 of 3 reruns).
- [x] 3.3 Probe: Turing 12 -> 15, ModSquad 4 -> 7 linked; zero previously matched CONSUMES edges lost in turing-webapp, turing-api, turing-agents, idp-front-end; 20 of 20 sampled new links correct.

## 4. Review round 1 (#145)

- [x] 4.1 HTTP consumer reading moved to `http_consumers.cpp` with `http_consumers_test.cpp`; CONSUMES identical before and after the move on the four consumer repos.
- [x] 4.2 Method through wrappers follows spread order, in consumer and composed-wrapper calls.
- [x] 4.3 Wrapper shapes cached per function for the file being extracted.
- [x] 4.4 Only locals set once are read; self-referencing, reassigned, loop-header and `catch` bindings are unresolved.
- [x] 4.5 Destructured parameter position tested.
- [x] 4.6 Full suite 85 of 85; probe links unchanged (Turing 15, ModSquad 7), zero matched edges lost.
