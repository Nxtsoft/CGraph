// Environment variable reads (env_contracts.cpp): `uses_contract env:<NAME>`
// facts from the code that reads each variable, through each language's walk.
#include "cgraph/env_contracts.hpp"

#include "cgraph/configured_extractors.hpp"
#include "cgraph/contracts.hpp"
#include "cgraph/detect.hpp"
#include "cgraph/javascript_extractor.hpp"
#include "cgraph/language_config.hpp"
#include "cgraph/normalize.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

// `<source id>|<context>` for every env fact, and a failure when a fact's
// source is no node of the file (resolve_contracts would drop it).
[[nodiscard]] std::set<std::string> env_facts(const cgraph::ExtractionResult& result, bool& orphan) {
  std::set<std::string> facts;
  for (const auto& relation : result.raw_relations) {
    if (relation.relation != "uses_contract") {
      continue;
    }
    facts.insert(relation.source_id + "|" + relation.context);
    const bool noded = std::ranges::any_of(result.fragment.nodes,
                                           [&](const cgraph::Node& node) { return node.id == relation.source_id; });
    if (!noded) {
      std::cerr << "fact with no node at its source: " << relation.source_id << '\n';
      orphan = true;
    }
  }
  return facts;
}

// The JavaScript and TypeScript sources, also replayed without the per-file
// binding cache to prove it changes nothing.
constexpr std::string_view kConfigTs = R"ts(
import { Value } from '@sinclair/typebox/value'
const API_BASE_URL = process.env.NEXT_PUBLIC_API_URL || 'http://localhost:8080';
function loadConfig() {
  const env = Value.Decode(envSchema, process.env)
  if (env.SENTRY_DSN) {}
  return env
}
const env = loadConfig()
export const config = {
  ml: { url: env.ML_BACKEND_BASE_URL || undefined, mode: env.mode },
  auth: { audience: env.AUTH0_AUDIENCE },
}
export function readAll(key: string) {
  const { REDIS_URL, PORT: port, LOG_LEVEL = 'info' } = process.env;
  const a = process.env['DATABASE_URL'];
  const b = process.env.DATABASE_URL;
  process.env.WRITTEN = '1';
  delete process.env.DELETED;
  const c = process.env[key];
  const d = import.meta.env.VITE_API;
  const e = Bun.env.BUN_ONLY;
  return [a, b, c, d, e, REDIS_URL, port, LOG_LEVEL];
}
export function shadowed(env: { NOT_ENV: string }) { return env.NOT_ENV; }
export function loose() {
  let mutable = process.env;
  return mutable.NOT_PROVEN;
}
export function viaParsed() {
  const parsed = schema.parse(process.env);
  const { PARSED_A } = parsed;
  return parsed.PARSED_B;
}
)ts";
constexpr std::string_view kEnvJs = R"js(
const url = process.env.API_URL;
const { DB_HOST } = process.env;
function client() { return process.env['CLIENT_KEY']; }
module.exports = { url, client };
)js";
constexpr std::string_view kShadowTs = R"ts(
function loadConfig() { return Value.Decode(envSchema, process.env) }
const env = loadConfig()
export function letShadow(p: any) { let env = p.cfg; return env.LET_SHADOWED }
export function forHead(list: any[]) { for (const env of list) { use(env.FOR_SHADOWED) } }
export function caught() { try { run() } catch (env) { return env.CATCH_SHADOWED } }
export function varHoisted(p: any) { if (p) { var env = p } return env.VAR_SHADOWED }
export function destructured(p: any) { const { env } = p; return env.DESTRUCTURED_SHADOWED }
export function classShadow() { class env { static CLASS_SHADOWED = 1 } return env.CLASS_SHADOWED }
export function reads() { return env.MODULE_READ }
)ts";
constexpr std::string_view kImportedTs = R"ts(
import { env } from './env'
export function a() { return env.IMPORTED_NAME }
)ts";
constexpr std::string_view kSwitchEnumTs = R"ts(
function loadConfig() { return Value.Decode(envSchema, process.env) }
const env = loadConfig()
export function switchLet(k: number, p: any) {
  switch (k) {
    case 1:
      let env = p.cfg
      return env.SWITCH_LET_SHADOWED
    default:
      return 0
  }
}
export function switchOtherCase(k: number, p: any) {
  switch (k) {
    case 1:
      const env = p.cfg
      break
    case 2:
      return env.SWITCH_CASE_SHADOWED
  }
}
export function inSwitch(k: number) { switch (k) { case 1: const cfg = 1; return env.IN_SWITCH_READ } }
export function afterSwitch(k: number) { switch (k) { case 1: { let env = 1 } } return env.AFTER_SWITCH_READ }
export function enumShadow() { enum env { ENUM_SHADOWED } return env.ENUM_SHADOWED }
export function constEnumShadow() { if (1) { const enum env { CONST_ENUM_SHADOWED } use(env.CONST_ENUM_SHADOWED) } }
)ts";

[[nodiscard]] bool expect(std::string_view what, const std::set<std::string>& facts,
                          const std::set<std::string>& expected) {
  if (facts == expected) {
    return true;
  }
  std::cerr << what << ": got\n";
  for (const auto& fact : facts) std::cerr << "  " << fact << '\n';
  std::cerr << "expected\n";
  for (const auto& fact : expected) std::cerr << "  " << fact << '\n';
  return false;
}

[[nodiscard]] std::string id(std::string_view file, std::string_view symbol = {}) {
  return cgraph::make_id(symbol.empty() ? std::string(file) : std::string(file) + ":" + std::string(symbol));
}

}  // namespace

int main() {
  bool orphan = false;
  // Every language reports before the exit, so a failure shows them all.
  bool failed = false;

  // TypeScript: direct reads, destructuring, subscripts and typed env objects.
  // A module-level const the extractor made no node for reads from the file; an
  // object-valued one is the reader. Writes, computed names, a `let`, a
  // shadowing parameter and a lower-case member of a typed object read nothing.
  {
    const auto result = cgraph::extract_typescript({.source_file = "src/config.ts", .relative_path = "src/config.ts", .source = kConfigTs});
    const auto file = id("src/config.ts");
    const std::set<std::string> expected{
        file + "|env:NEXT_PUBLIC_API_URL",
        id("src/config.ts", "loadConfig") + "|env:SENTRY_DSN",
        id("src/config.ts", "config") + "|env:ML_BACKEND_BASE_URL",
        id("src/config.ts", "config") + "|env:AUTH0_AUDIENCE",
        id("src/config.ts", "readAll") + "|env:REDIS_URL",
        id("src/config.ts", "readAll") + "|env:PORT",
        id("src/config.ts", "readAll") + "|env:LOG_LEVEL",
        id("src/config.ts", "readAll") + "|env:DATABASE_URL",
        id("src/config.ts", "readAll") + "|env:VITE_API",
        id("src/config.ts", "readAll") + "|env:BUN_ONLY",
        id("src/config.ts", "viaParsed") + "|env:PARSED_A",
        id("src/config.ts", "viaParsed") + "|env:PARSED_B",
    };
    if (!expect("typescript", env_facts(result, orphan), expected)) {
      failed = true;
    }

    // resolve_contracts mints one env node per name, unserved (no repo provides
    // an env variable), with CONSUMES from each reader.
    cgraph::GraphSnapshot graph;
    graph.nodes = result.fragment.nodes;
    graph.edges = result.fragment.edges;
    cgraph::resolve_contracts(graph, result.raw_relations);
    const auto node = std::ranges::find_if(graph.nodes, [](const cgraph::Node& n) { return n.id == "env:DATABASE_URL"; });
    const bool consumed = std::ranges::any_of(graph.edges, [&](const cgraph::Edge& edge) {
      return edge.source == id("src/config.ts", "readAll") && edge.target == "env:DATABASE_URL" && edge.relation == "CONSUMES";
    });
    if (node == graph.nodes.end() || node->kind != "env" || node->properties.at("served") != "false" || !consumed ||
        cgraph::is_bridged_contract("env:DATABASE_URL")) {
      std::cerr << "env:DATABASE_URL was not minted as an unserved, unbridged env contract consumed by readAll\n";
      failed = true;
    }
  }

  // Python: os.environ[...] reads (not writes or deletes), .get and getenv.
  {
    const auto result = cgraph::extract_python({.source_file = "app/settings.py", .relative_path = "app/settings.py", .source = R"py(
import os
ENV = os.environ.get("ENV", "local")

def database_url():
    os.environ["WRITTEN"] = "1"
    del os.environ["DELETED"]
    key = os.environ["OPENAI_API_KEY"]
    other = os.getenv('ML_BACKEND_URL')
    dynamic = os.environ.get(name)
    formatted = os.environ.get(f"X_{name}")
    return os.environ.get("ENV")
)py"});
    const std::set<std::string> expected{
        id("app/settings.py") + "|env:ENV",
        id("app/settings.py", "database_url") + "|env:OPENAI_API_KEY",
        id("app/settings.py", "database_url") + "|env:ML_BACKEND_URL",
        id("app/settings.py", "database_url") + "|env:ENV",
    };
    if (!expect("python", env_facts(result, orphan), expected)) {
      failed = true;
    }
  }

  // Go: os.Getenv / os.LookupEnv with a literal name; a constant is not followed.
  {
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Go, {.source_file = "contract/spec.go", .relative_path = "contract/spec.go", .source = R"go(
package contract

import "os"

func BaseURL() string {
	if v := os.Getenv("PASSLESS_CONTRACT_BASE_URL"); v != "" {
		return v
	}
	_, ok := os.LookupEnv(`RAW_NAME`)
	_ = os.Getenv(EnvClientID)
	_ = ok
	return DefaultBaseURL
}
)go"});
    const std::set<std::string> expected{
        id("contract/spec.go", "BaseURL") + "|env:PASSLESS_CONTRACT_BASE_URL",
        id("contract/spec.go", "BaseURL") + "|env:RAW_NAME",
    };
    if (!result || !expect("go", env_facts(*result, orphan), expected)) {
      failed = true;
    }
  }

  // Kotlin: System.getenv with a literal (not a template) and @Value("${X}") for
  // an env-shaped X; a dotted Spring property key is no env variable.
  {
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Kotlin, {.source_file = "src/Keys.kt", .relative_path = "src/Keys.kt", .source = R"kt(
package keys

class Keys(
    @Value("\${ENCRYPTION_KEY:}") private val key: String,
    @Value("\${sentra.saml.allowed-domains:}") private val domains: String,
) {
    fun load(): String {
        val profile = System.getenv("SPRING_PROFILES_ACTIVE") ?: "default"
        val templated = System.getenv("X_$profile")
        return profile
    }
}
)kt"});
    const std::set<std::string> expected{
        id("src/Keys.kt") + "|env:ENCRYPTION_KEY",
        id("src/Keys.kt", "load") + "|env:SPRING_PROFILES_ACTIVE",
    };
    if (!result || !expect("kotlin", env_facts(*result, orphan), expected)) {
      failed = true;
    }
  }

  // Java: System.getenv and @Value.
  {
    const auto result = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::Java, {.source_file = "src/Cfg.java", .relative_path = "src/Cfg.java", .source = R"java(
class Cfg {
  @Value("${JWT_SECRET}") private String secret;
  String region() { return System.getenv("AWS_REGION"); }
  String other() { helper(); return Other.getenv("NOT_SYSTEM"); }
}
)java"});
    const std::set<std::string> expected{
        id("src/Cfg.java") + "|env:JWT_SECRET",
        id("src/Cfg.java", "region") + "|env:AWS_REGION",
    };
    if (!result || !expect("java", env_facts(*result, orphan), expected)) {
      failed = true;
    }
  }

  // Spring application config: `${X}` / `${X:default}` placeholders outside
  // comments, from the config file's node; dotted keys are properties.
  {
    const auto yml = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::SpringConfig,
        {.source_file = "/r/idp-core/src/main/resources/application-dev.yml",
         .relative_path = "idp-core/src/main/resources/application-dev.yml", .source = R"yml(
# base-url: ${COMMENTED_OUT}
saml:
  idp:
    base-url: ${SAML_IDP_BASE_URL:https://dev-api.example.net}  # was ${OLD_URL}
spring:
  datasource:
    url: "${DB_URL:jdbc:${DB_HOST:localhost}}"
    other: ${server.port}
message: Can't reach ${API_URL} # was ${OLD_URL}
)yml"});
    const auto yml_file = id("idp-core/src/main/resources/application-dev.yml");
    const std::set<std::string> expected_yml{
        yml_file + "|env:SAML_IDP_BASE_URL",
        yml_file + "|env:DB_URL",
        yml_file + "|env:DB_HOST",
        yml_file + "|env:API_URL",
    };
    if (!yml || !expect("spring yaml", env_facts(*yml, orphan), expected_yml)) {
      failed = true;
    }
    const auto properties = cgraph::extract_configured_language(
        cgraph::DetectedLanguage::SpringConfig,
        {.source_file = "/r/src/main/resources/application.properties",
         .relative_path = "src/main/resources/application.properties", .source = R"props(
! legacy ${BANG_COMMENT}
# ${HASH_COMMENT}
server.port=${PORT:8080}
)props"});
    const std::set<std::string> expected_properties{id("src/main/resources/application.properties") + "|env:PORT"};
    if (!properties || !expect("spring properties", env_facts(*properties, orphan), expected_properties)) {
      failed = true;
    }
  }

  // Plain JavaScript reads the same way.
  {
    const auto result = cgraph::extract_javascript({.source_file = "lib/env.js", .relative_path = "lib/env.js", .source = kEnvJs});
    const std::set<std::string> expected{
        id("lib/env.js") + "|env:API_URL",
        id("lib/env.js") + "|env:DB_HOST",
        id("lib/env.js", "client") + "|env:CLIENT_KEY",
    };
    if (!expect("javascript", env_facts(result, orphan), expected)) {
      failed = true;
    }
  }

  // Any other binding of the name between a read and a module-level typed env
  // `const` shadows it: `let`, `var` (hoisted from a nested block), a for-head,
  // a `catch` parameter, a class, a destructured `const`, an import.
  {
    const auto result = cgraph::extract_typescript({.source_file = "src/shadow.ts", .relative_path = "src/shadow.ts", .source = kShadowTs});
    const std::set<std::string> expected{id("src/shadow.ts", "reads") + "|env:MODULE_READ"};
    if (!expect("typescript shadowing", env_facts(result, orphan), expected)) {
      failed = true;
    }
    const auto imported = cgraph::extract_typescript({.source_file = "src/imported.ts", .relative_path = "src/imported.ts", .source = kImportedTs});
    if (!expect("typescript import", env_facts(imported, orphan), {})) {
      failed = true;
    }
  }

  // A `let` / `const` directly in a `switch` case (no braces: one scope for
  // the whole switch) and a local TS `enum` shadow the module env `const` too.
  {
    const auto result = cgraph::extract_typescript(
        {.source_file = "src/switch.ts", .relative_path = "src/switch.ts", .source = kSwitchEnumTs});
    const std::set<std::string> expected{
        id("src/switch.ts", "inSwitch") + "|env:IN_SWITCH_READ",
        id("src/switch.ts", "afterSwitch") + "|env:AFTER_SWITCH_READ",
    };
    if (!expect("typescript switch and enum shadowing", env_facts(result, orphan), expected)) {
      failed = true;
    }
  }

  // extract_* hold an EnvContractsFileScope, so each scope's bindings are read
  // once per file; without one every lookup reads them afresh. Both give the
  // same env facts on every JavaScript / TypeScript case above.
  {
    struct Case {
      cgraph::DetectedLanguage language;
      std::string_view path;
      std::string_view source;
    };
    for (const auto& [language, path, source] : {
             Case{cgraph::DetectedLanguage::TypeScript, "src/config.ts", kConfigTs},
             Case{cgraph::DetectedLanguage::JavaScript, "lib/env.js", kEnvJs},
             Case{cgraph::DetectedLanguage::TypeScript, "src/shadow.ts", kShadowTs},
             Case{cgraph::DetectedLanguage::TypeScript, "src/imported.ts", kImportedTs},
             Case{cgraph::DetectedLanguage::TypeScript, "src/switch.ts", kSwitchEnumTs},
             Case{cgraph::DetectedLanguage::Tsx, "src/switch.tsx", kSwitchEnumTs},
         }) {
      const cgraph::ExtractionContext context{
          .source_file = std::string(path), .relative_path = std::string(path), .source = source};
      const auto cached = language == cgraph::DetectedLanguage::JavaScript ? cgraph::extract_javascript(context)
                          : language == cgraph::DetectedLanguage::Tsx      ? cgraph::extract_tsx(context)
                                                                           : cgraph::extract_typescript(context);
      auto config = language == cgraph::DetectedLanguage::JavaScript ? cgraph::javascript_language_config()
                    : language == cgraph::DetectedLanguage::Tsx      ? cgraph::tsx_language_config()
                                                                     : cgraph::typescript_language_config();
      const TSLanguage* grammar = cgraph::tree_sitter_language_for(language);
      cgraph::intern_node_symbols(config, grammar);
      const auto uncached = cgraph::extract_with_config(grammar, config, context);
      const auto env_only = [&](const cgraph::ExtractionResult& result) {
        auto facts = env_facts(result, orphan);
        std::erase_if(facts, [](const std::string& fact) { return fact.find("|env:") == std::string::npos; });
        return facts;
      };
      const auto cached_facts = env_only(cached);
      if (cached_facts.empty() && path != "src/imported.ts") {
        std::cerr << path << ": no env facts to compare\n";
        failed = true;
      }
      if (!expect(std::string(path) + " cached vs uncached", cached_facts, env_only(uncached))) {
        failed = true;
      }
    }
  }

  if (orphan || failed) {
    return 1;
  }
  if (!cgraph::is_env_shaped_name("ML_BACKEND_BASE_URL") || cgraph::is_env_shaped_name("server.port") ||
      cgraph::is_env_shaped_name("mode") || !cgraph::is_env_variable_name("mode") ||
      cgraph::is_env_variable_name("X Y") || cgraph::is_env_variable_name("1X")) {
    std::cerr << "env name predicates\n";
    return 1;
  }
  return 0;
}
