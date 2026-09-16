# Tasks

## 1. Design view
- [x] 1.1 `report_test.cpp::test_design_view`: four entry kinds ranked by reach; non-entries (leaf,
      cycle members, called functions, test-root callers); flow children by reach with the branch
      cap and `more`, three hops deep; layers by shortest distance with modules; reached/unreached
      counts and samples; `hops` bounds the drawing not the reach; include_tests admits the test
      caller; scope; shedding drops the last entry; json, markdown and mermaid shapes; a shared
      callee drawn once in mermaid.
- [x] 1.2 `report_test.cpp::test_design_envelope`: json/mermaid/markdown envelopes, svg refused,
      hops 0 rejected; `test_daemon_envelope` no longer expects a reserved view.
- [x] 1.3 `FlowNode`/`DesignEntry`/`DesignLayer`/`DesignReport`, `build_design_report`,
      `shed_to_budget`, `design_report_json`, `render_design_mermaid`, `render_design_markdown`,
      `render_design_report`, `report_response` dispatch; `ReportRequest.hops`.

## 2. Exports and surfaces
- [x] 2.1 `write_exports` writes `design.mmd` + `design.md`, stops writing `call-flow.html`;
      `export_call_flow_html` removed; `pipeline_test.cpp` and `export_json_test.cpp` updated.
- [x] 2.2 CLI `cgraph report design` + `--hops`; MCP `graph_report` + `hops`; host contract, skill,
      READMEs.

## 3. Verification
- [x] 3.1 `ctest --preset default`: 78/78 pass (this Mac, Debug preset).
- [x] 3.2 Measured on CGraph's own tree: 60 entry points (10 main, 50 root), `src/cli/main.cpp:887`
      leading at reach 483; layers 0-10; 635 reached, 38 unreached; 70 ms; `design.md` (703 lines) and
      `design.mmd` (757) written by the one-shot build, `call-flow.html` gone. Also found: the one-shot
      build crosses `kDrlThreshold` (2,000 nodes) and spends 12-20 s in DrL layout -- recorded in the
      proposal as a follow-up, not caused by the view.
- [x] 3.3 Measured on `frontend` with `scope: "app"`: 199 page components in 124 files, 18 route
      handlers, 172 roots; `ProjectDetailPage` (17 hooks) leads; 0.16 s; JSX use is not a call edge, so
      components rendered only in JSX are roots or unreached (707); without scope, Playwright trace
      bundles lead the list (detector follow-up).
- [ ] 3.4 Obtain non-author review and merge.
