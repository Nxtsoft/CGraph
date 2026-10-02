#pragma once

// Environment variable reads (contracts.hpp: `uses_contract` facts of kind
// `env`). Every read of a variable whose name the code spells literally is a
// `uses_contract` fact, context `env:<NAME>` (case as written), from the code
// that reads it: the enclosing function, else the module-level variable whose
// initializer holds the read when the extractor made a node for it, else the
// file. One fact per reading symbol and name.
//
// A name alone never says which service a variable addresses (every service
// reads `NODE_ENV`, and `localhost:8080` is half the fleet), so these facts
// join repositories only where a workspace manifest or a seam command declares
// the variable's service (contract_declarations.hpp `env`). Nothing here is a
// provider: the declared service is.
//
// What counts as a read:
//
//   JavaScript / TypeScript
//     `process.env.X`, `process.env['X']`, `import.meta.env.X`, `Bun.env.X`;
//     `const { X, Y: y, Z = d } = process.env`;
//     `e.X` (and `const { X } = e`) for an X in upper snake case where `e` is a
//     `const` whose value is one of those env objects, a call taking one as an
//     argument (`Value.Decode(schema, process.env)`, `schema.parse(process.env)`:
//     a schema-validated env keeps its keys), or a call of a function declared
//     in the same file whose every return is such a `const` (one hop:
//     `const env = loadConfig()`). An assignment target or `delete` is a write.
//   Python   `os.environ["X"]` (not as an assignment or `del` target),
//            `os.environ.get("X")`, `os.getenv("X")`.
//   Go       `os.Getenv("X")`, `os.LookupEnv("X")`.
//   Kotlin / Java
//            `System.getenv("X")`; `@Value("${X}")` / `@Value("${X:default}")`
//            for an X in upper snake case (a dotted key is a Spring property,
//            not an environment variable).
//   Spring application config (`application*.yml|yaml|properties`)
//            `${X}` / `${X:default}` placeholders in values, X in upper snake
//            case, outside comments.
//
// Not read: a name that is not a literal (`os.Getenv(EnvClientID)`,
// `process.env[key]`), pydantic `BaseSettings` fields (their env names depend
// on `env_prefix`, aliases and case sensitivity configured elsewhere), and
// envsubst-style `${X}` in other YAML (Kubernetes templates are not read by
// the code that runs them).

#include "cgraph/extractor.hpp"

#include <tree_sitter/api.h>

#include <string>
#include <string_view>
#include <vector>

namespace cgraph {

// One handler per grammar, called from that language's extra_walk on every
// node. `fragment` is the file's fragment so far: a module-level variable is
// the reading symbol only when it is already a node there.
void javascript_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                          const Fragment& fragment, std::vector<RawRelation>& out);
void python_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                      std::vector<RawRelation>& out);
void go_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                  std::vector<RawRelation>& out);
void kotlin_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                      std::vector<RawRelation>& out);
void java_env_reads(const TSNode& node, const ExtractionContext& context, const std::string& function_scope_id,
                    std::vector<RawRelation>& out);

// `${X}` placeholders in a Spring application config file, from its file node.
void append_spring_config_env_reads(const ExtractionContext& context, ExtractionResult& result);

// `[A-Za-z_][A-Za-z0-9_]*`: a name a shell can export.
[[nodiscard]] bool is_env_variable_name(std::string_view name);
// `[A-Z][A-Z0-9_]*`: the upper snake case that marks an environment variable
// where the syntax alone does not (`env.X`, `${X}`).
[[nodiscard]] bool is_env_shaped_name(std::string_view name);

}  // namespace cgraph
