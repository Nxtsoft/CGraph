# A wrapper's callers send their method where the wrapper takes their options

## Why

Issue #147. A wrapper whose first parameter is the path records an `http_wrapper` fact of the form `"<fixed METHOD or empty> <prefix>"`. The fact does not say which parameter's options the wrapper forwards, whether its own method is readable, or whether it picks between two verbs. `resolve_contracts` then took a caller's method from the caller's *second* argument, which is often not the options, and fell back to GET.

turing-agents `src/luna_agent/library-tool.ts:38` defines `apiFetch<T>(path, authHeaders, init?)` and spreads `init`, the third argument. So `apiFetch(\`/api/v1/composites/${id}/ingredient\`, authHeaders, { method: "POST" })` (`:307`) came out as `GET`, `:327`'s PATCH came out as GET, and the POSTs at `:165` and `:217` were missing. The same gap made a wrapper that forwards no options take a caller's `{ method: 'POST' }`, and a wrapper whose own options are unreadable (`fetch(url, decorate(init))`) default every caller to GET.

The positional wrappers #145 introduced had the same problem: their callers' options were read right after the path, wherever the wrapper actually takes them.

## What Changes

- **Extractor (`http_consumers.cpp`).** The `http_wrapper` method gains three markers: `~<index>` for the parameter whose options the wrapper spreads into its own (`{ ...init }`, `fetch(url, init)`), a trailing `?` when the wrapper's own method is unreadable, and `POST|DELETE` for a method choice. A call to a non-primitive function no longer reads its second argument as options. Its `http_call_args` descriptors already carry every argument, and an `O` descriptor now also carries a choice (`OPOST|DELETE`).
- **Contracts (`contracts.cpp`).** A wrapper with any of the new markers is read from `http_call_args` (step 6b), like the positional ones. Its method is the wrapper's own (from its method parameter, fixed verb or choices), overridden by the caller's options at the recorded index when they name a method. Options the caller's file cannot read, or a method the wrapper's file cannot read that no caller overrides, are counted in `calls_unresolved`. A wrapper with no marker (Kotlin, Go and TypeScript wrappers that forward no options) sends its own method whatever a caller passes.
- **Index key** `cgraph-index-v1:logic-11`: the fact format changed.

## Non-goals

- Options built by a call inside the wrapper (`decorate(init)`) are not followed into that function; callers stay unresolved.
- Kotlin and Go wrappers record no options index; their requests take no options object.
