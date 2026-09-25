#include "cgraph/change_context.hpp"
#include "cgraph/daemon_ops.hpp"
#include "cgraph/mcp_server.hpp"
#include "cgraph/pipeline.hpp"
#include "cgraph/snapshot_source_reader.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
const fs::path fixtures = CGRAPH_CHANGE_FIXTURE_DIR;
const fs::path output = CGRAPH_CHANGE_TEST_OUTPUT_DIR;
void require(bool condition, const std::string& reason) {
  if (!condition) throw std::runtime_error(reason);
}
Json parameters(const std::string& name, int budget = 20000) {
  const auto root = fixtures / name;
  return {{"base_root", (root / "base").string()}, {"target_root", (root / "target").string()},
          {"diff_path", (root / "change.diff").string()}, {"budget", budget}, {"max_depth", 4}};
}
bool contains(const Json& rows, const std::string& field, const std::string& value) {
  for (const auto& row : rows) if (row.value(field, std::string{}) == value) return true;
  return false;
}
void rejects(const Json& params, const std::string& reason) {
  try { (void)cgraph::change_context(params); }
  catch (const std::exception& error) {
    require(std::string(error.what()).find(reason) != std::string::npos,
            "wrong rejection: " + std::string(error.what()));
    return;
  }
  throw std::runtime_error("expected rejection: " + reason);
}
void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
}
int main() {
  try {
    fs::create_directories(output);
    auto deletion = cgraph::change_context(parameters("deletion"));
    write(output / "deletion.json", deletion.dump(2));
    require(deletion["changes"][0]["status"] == "deleted", "deleted file mapping missing");
    bool caller = false, transitive = false, deleted_source = false;
    for (const auto& item : deletion["impacts"]) {
      require(item["path"] != "unrelated.py", "same-named unrelated symbol became a dependent");
      if (item["snapshot"] != "base") continue;
      if (item["label"] == "caller") caller = true;
      if (item["label"] == "outer") {
        transitive = true;
        require(item["witness"].size() >= 2, "transitive witness chain missing");
        for (const auto& edge : item["witness"]) {
          require(edge["snapshot"] == "base", "witness mixes snapshots");
          require(edge["source"].contains("source_sha256") && edge["target"].contains("source_sha256"),
                  "witness lacks source identity");
        }
      }
    }
    for (const auto& item : deletion["context"]) {
      if (item["snapshot"] == "base" && item["label"] == "exported" &&
          item.value("snippet", "").find("def exported") != std::string::npos) deleted_source = true;
    }
    require(caller && transitive && deleted_source, "deleted export lost caller, transitive witness, or base source");
    require(deletion["tokens_used"] == cgraph::serialized_context_tokens(deletion), "serialized token count inaccurate");
    require(deletion["tokens_used"].get<int>() <= 20000, "response exceeds budget");

    const auto rename = cgraph::change_context(parameters("rename"));
    require(contains(rename["context"], "label", "old_name") && contains(rename["context"], "label", "new_name"),
            "function rename must preserve both source versions");
    require(contains(rename["changes"][0]["symbol_changes"], "pairing", "unresolved"),
            "rename must disclose uncertain pairing rather than guess by name proximity");
    const auto moved = cgraph::change_context(parameters("file_rename"));
    require(moved["changes"][0]["status"] == "renamed", "explicit file rename missing");
    require(contains(moved["changes"][0]["symbol_changes"], "status", "moved"), "renamed file symbol pairing absent");

    const auto interface = cgraph::change_context(parameters("interface"));
    write(output / "interface.json", interface.dump(2));
    require(contains(interface["context"], "label", "Service"), "interface source missing");
    require(contains(interface["context"], "label", "Worker"), "implementer source missing");
    require(!interface["changes"][0]["base_symbols"].empty() && !interface["changes"][0]["target_symbols"].empty(),
            "interface old/new mapping missing");
    const auto barrel = cgraph::change_context(parameters("barrel"));
    write(output / "barrel.json", barrel.dump(2));
    require(barrel["changes"][0]["old_path"] == "index.ts", "barrel mapping missing");
    bool old_export = false, new_export = false;
    for (const auto& item : barrel["context"]) {
      old_export |= item["snapshot"] == "base" && item["path"] == "old.ts";
      new_export |= item["snapshot"] == "target" && item["path"] == "new.ts";
    }
    require(old_export && new_export, "redirected barrel must retain old/new export evidence");

    for (const int budget : {2000, 4000, 6000}) {
      const auto bounded = cgraph::change_context(parameters("deletion", budget));
      require(bounded["tokens_used"].get<int>() <= budget, "full envelope exceeds budget");
      require(bounded["tokens_used"] == cgraph::serialized_context_tokens(bounded), "budget count not fixed point");
    }
    rejects(parameters("deletion", 1), "budget");
    auto pinned = parameters("deletion");
    pinned["expected_base_content_root"] = std::string(64, '0');
    rejects(pinned, "expected_base_content_root");
    pinned["expected_base_content_root"] = deletion["base"]["content_root"];
    pinned["expected_target_content_root"] = deletion["target"]["content_root"];
    (void)cgraph::change_context(pinned);

    const auto bad_diff = output / "bad.diff";
    auto bad = parameters("deletion"); bad["diff_path"] = bad_diff.string();
    write(bad_diff, "--- a/api.py\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-def wrong():\n-    return 1\n");
    rejects(bad, "old hunk content");
    write(bad_diff, "--- a/../api.py\n+++ /dev/null\n@@ -1,1 +0,0 @@\n-x\n");
    rejects(bad, "escapes");
    write(bad_diff, "--- a/api.py\n+++ /dev/null\n@@ bad @@\n-x\n");
    rejects(bad, "malformed hunk");


    const auto insertion = cgraph::change_context(parameters("insertion"));
    bool base_caller = false;
    for (const auto& item : insertion["impacts"])
      base_caller |= item["snapshot"] == "base" && item["label"] == "caller";
    require(base_caller, "pure insertion boundary lost the existing base caller");
    const auto owner_change = cgraph::change_context(parameters("owner_change"));
    bool saw_owner_run = false;
    for (const auto& item : owner_change["changes"][0]["symbol_changes"]) {
      if (!item["base"].is_null() && item["base"]["label"] == "run") {
        saw_owner_run = true;
        require(item["pairing"] == "unresolved", "same-name methods in different owners were paired");
      }
    }
    require(saw_owner_run, "owner-change mapping dropped the removed method");
    const auto owner_fields = cgraph::change_context(parameters("owner_fields"));
    bool saw_field = false;
    for (const auto& item : owner_fields["changes"][0]["symbol_changes"]) {
      if (!item["base"].is_null() && item["base"]["label"] == "x") {
        saw_field = true;
        require(item["pairing"] == "unresolved", "C++ field with a different defines owner was paired");
      }
    }
    require(saw_field, "C++ field-owner fixture did not map x");
    const auto mixed = cgraph::change_context(parameters("mixed"));
    require(mixed["changes"][0]["base_symbols"].size() >= 2,
            "mixed import and body edits lost the file seed");

    const auto partial_root = output / "partial";
    fs::create_directories(partial_root);
    fs::copy(fixtures / "rename" / "base", partial_root / "base",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::copy(fixtures / "rename" / "target", partial_root / "target",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    write(partial_root / "base" / "omitted.py", "def omitted(): return 1\n");
    write(partial_root / "target" / "omitted.py", "def omitted(): return 2\n");
    auto partial_params = parameters("rename");
    partial_params["base_root"] = (partial_root / "base").string();
    partial_params["target_root"] = (partial_root / "target").string();
    const auto partial = cgraph::change_context(partial_params);
    require(partial["uncertainty"]["unassessed_source_paths"] == Json::array({"omitted.py"}),
            "omitted source difference is not explicitly listed");
    require(!partial["uncertainty"]["all_detected_differences_supplied"].get<bool>(), "partial diff reported complete");

    const auto copy_root = output / "copy";
    write(copy_root / "base" / "old.py", "def stable():\n    return 3\n");
    write(copy_root / "target" / "old.py", "def stable():\n    return 3\n");
    write(copy_root / "target" / "new.py", "def stable():\n    return 3\n");
    auto copy_params = parameters("file_rename");
    copy_params["base_root"] = (copy_root / "base").string();
    copy_params["target_root"] = (copy_root / "target").string();
    rejects(copy_params, "rename source must disappear");

    // Real source mutation after a snapshot, no mock reader or service.
    const auto stale_root = output / "stale";
    write(stale_root / "source.py", "def value():\n    return 1\n");
    const auto stale = cgraph::run_one_shot(stale_root);
    write(stale_root / "source.py", "def value():\n    return 2\n");
    cgraph::SnapshotSourceReader stale_reader(stale.graph.source_hashes, true);
    bool rejected_stale = false;
    try { (void)stale_reader.read_verified_source(fs::canonical(stale_root / "source.py").string()); }
    catch (const cgraph::SourceSnapshotMismatch&) { rejected_stale = true; }
    require(rejected_stale, "changed source passed pinned verification");

    // symbols_only: every symbol change survives at a budget that sheds every
    // impact otherwise, and no impact or context evidence is produced at all.
    {
      auto starved = parameters("deletion", 2000);
      const auto shed = cgraph::change_context(starved);
      require(shed["omitted"]["impacts"].get<int>() > 0, "budget 2000 must shed impacts for the contrast to hold");
      starved["symbols_only"] = true;
      const auto symbols = cgraph::change_context(starved);
      require(symbols["symbols_only"] == true, "symbols_only not echoed");
      require(symbols["impacts"].empty() && symbols["context"].empty(), "symbols_only produced impacts or context");
      require(symbols["omitted"]["impacts"] == 0 && symbols["omitted"]["context"] == 0 && symbols["truncated"] == false,
              "symbols_only must not report shedding");
      require(symbols["changes"] == deletion["changes"], "symbols_only changed the symbol_changes classification");
      require(symbols["tokens_used"] == cgraph::serialized_context_tokens(symbols), "symbols_only token count not fixed point");
      auto symbols_only_mcp = Json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
                                   {"params", {{"name", "graph_change_context"}, {"arguments", starved}}}};
      const auto via_mcp = cgraph::handle_mcp_request(symbols_only_mcp, {});
      const auto mcp_body = Json::parse(via_mcp["result"]["content"][0]["text"].get<std::string>());
      require(mcp_body["symbols_only"] == true && mcp_body["changes"] == deletion["changes"],
              "MCP symbols_only differs from the engine");
    }

    // MCP executes the same real engine path without any daemon forwarder.
    auto request = Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                        {"params", {{"name", "graph_change_context"}, {"arguments", parameters("deletion")}}}};
    const auto mcp = cgraph::handle_mcp_request(request, {});
    require(!mcp.contains("error") && !mcp["result"].value("isError", false), "MCP real operation failed");
    const auto body = Json::parse(mcp["result"]["content"][0]["text"].get<std::string>());
    require(body["base"]["content_root"] == deletion["base"]["content_root"], "MCP snapshot differs from CLI engine");
    request["params"]["arguments"]["budget"] = 1;
    const auto failure = cgraph::handle_mcp_request(request, {});
    require(failure["result"]["isError"] == true, "MCP failure must be an explicit tool error");
    std::cout << "change-context: deletion, transitive witnesses, ambiguous names, rename, interface, barrel, budgets, pins, malformed diff, stale source and MCP passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
