#include "cgraph/dedup.hpp"

#include <iostream>

int main() {
  if (cgraph::shannon_entropy("aaaaaa") >= 2.5) {
    return 1;
  }
  if (cgraph::jaro_winkler_similarity("service", "service") != 1.0) {
    return 1;
  }
  if (cgraph::jaro_winkler_similarity("payment_service", "payment_services") < 0.92) {
    return 1;
  }

  cgraph::GraphSnapshot graph;
  graph.nodes.push_back(cgraph::Node{.id = "a", .label = "PaymentService", .source_file = "a.cpp", .kind = "class"});
  graph.nodes.push_back(cgraph::Node{.id = "b", .label = "Payment Service", .source_file = "b.cpp", .kind = "class"});
  graph.nodes.push_back(cgraph::Node{.id = "c", .label = "aaaaaa", .source_file = "c.cpp", .kind = "class"});
  graph.nodes.push_back(cgraph::Node{
      .id = "d",
      .label = "PaymentServic",
      .source_file = "d.cpp",
      .kind = "class",
      .properties = {{"community", "payments"}},
  });
  graph.nodes.push_back(cgraph::Node{
      .id = "e",
      .label = "PaymentServce",
      .source_file = "e.cpp",
      .kind = "class",
      .properties = {{"community", "payments"}},
  });
  graph.edges.push_back(cgraph::Edge{.source = "a", .target = "b", .relation = "USES"});
  graph.edges.push_back(cgraph::Edge{.source = "d", .target = "e", .relation = "USES"});

  cgraph::semantic_dedup(graph);

  if (graph.nodes.size() != 2) {
    std::cerr << "expected 2 nodes after dedup, got " << graph.nodes.size() << '\n';
    for (const auto& node : graph.nodes) {
      std::cerr << node.id << ":" << node.label << '\n';
    }
    return 1;
  }
  for (const auto& edge : graph.edges) {
    if (edge.source == "b" || edge.target == "b" || edge.source == "d" || edge.target == "d" ||
        edge.source == "e" || edge.target == "e") {
      std::cerr << "edge still references duplicate id: " << edge.source << " -> " << edge.target << '\n';
      return 1;
    }
  }
  bool saw_low_entropy = false;
  for (const auto& node : graph.nodes) {
    if (node.id == "c") {
      saw_low_entropy = true;
    }
  }
  if (!saw_low_entropy) {
    std::cerr << "low entropy node was incorrectly deduplicated\n";
    return 1;
  }

  // Regression: a component and its props interface in the same file share a
  // prefix ("MessageBubble" / "MessageBubbleProps"). Jaro-Winkler's prefix
  // bonus scores them high, but they are distinct symbols — merging them
  // deletes the function node and mis-attributes its calls to the type. A
  // prefix-extension pair must never merge. Likewise a name shared across two
  // files ("Helper" in two modules) is two symbols, not one.
  cgraph::GraphSnapshot guard;
  guard.nodes.push_back(cgraph::Node{.id = "mb", .label = "MessageBubbleProps", .source_file = "mb.tsx", .kind = "type"});
  guard.nodes.push_back(cgraph::Node{.id = "mf", .label = "MessageBubble", .source_file = "mb.tsx", .kind = "function"});
  guard.nodes.push_back(cgraph::Node{.id = "h1", .label = "HelperWidget", .source_file = "one.tsx", .kind = "function"});
  guard.nodes.push_back(cgraph::Node{.id = "h2", .label = "HelperWidget", .source_file = "two.tsx", .kind = "function"});
  // Two same-label declarations in ONE file are a genuine duplicate and merge.
  guard.nodes.push_back(cgraph::Node{.id = "d1", .label = "ConfigLoader", .source_file = "dup.tsx", .kind = "function"});
  guard.nodes.push_back(cgraph::Node{.id = "d2", .label = "ConfigLoader", .source_file = "dup.tsx", .kind = "function"});
  // A node that names a concrete declaration site is a concrete symbol, and a
  // similar label is not evidence that two of them are one thing. These four
  // pairs each score above the 0.92 threshold and are each two different
  // functions; all were measured merging away on this repo once C/C++ labels
  // became bare names, which removed the accidental protection a long
  // signature-bearing label used to provide.
  const auto at = [](std::string id, std::string label, std::string file, std::uint32_t line) {
    return cgraph::Node{.id = std::move(id),
                        .label = std::move(label),
                        .source_file = std::move(file),
                        .source_location = cgraph::SourceLocation{.start_line = line, .end_line = line},
                        .kind = "function"};
  };
  guard.nodes.push_back(at("s1", "validate_semantic_fragment_json", "validation.cpp", 9));
  guard.nodes.push_back(at("s2", "validate_semantic_fragment_file", "validation.cpp", 15));
  guard.nodes.push_back(at("s3", "drainer_installed", "drainer.cpp", 92));
  guard.nodes.push_back(at("s4", "drainer_uninstall", "drainer.cpp", 105));
  guard.nodes.push_back(at("s5", "supervisor_spec", "supervisor.cpp", 30));
  guard.nodes.push_back(at("s6", "supervisor_sync", "supervisor.cpp", 161));
  // Cross-file counts too: a declaration site is a file AND a line.
  guard.nodes.push_back(at("s7", "query_zero_hits", "stats.hpp", 144));
  guard.nodes.push_back(at("s8", "query_zero_hit_rate", "stats.cpp", 90));
  // A genuine double-extraction of one symbol shares its site and still merges.
  guard.nodes.push_back(at("s9", "same_site_twice", "twice.cpp", 12));
  guard.nodes.push_back(at("s10", "same_site_twice", "twice.cpp", 12));
  // Three overloads can share a LINE, so the site is line AND column.
  guard.nodes.push_back(cgraph::Node{.id = "c1", .label = "three_up", .source_file = "cols.cpp",
                                     .source_location = cgraph::SourceLocation{.start_line = 7, .start_column = 0},
                                     .kind = "function"});
  guard.nodes.push_back(cgraph::Node{.id = "c2", .label = "three_up", .source_file = "cols.cpp",
                                     .source_location = cgraph::SourceLocation{.start_line = 7, .start_column = 30},
                                     .kind = "function"});
  guard.nodes.push_back(cgraph::Node{.id = "c3", .label = "three_up", .source_file = "cols.cpp",
                                     .source_location = cgraph::SourceLocation{.start_line = 7, .start_column = 62},
                                     .kind = "function"});
  // A `concept` carries NO source_file and NO source_location, so the exact pass
  // skips it entirely and the fuzzy pass is its only merge path. Two concepts with
  // one label are one idea and must still merge. (`document` and `media` are NOT
  // like this -- they do carry a source_file -- so they are governed by the
  // from-a-file branch instead.)
  guard.nodes.push_back(cgraph::Node{.id = "k1", .label = "OpenSpec Change Lifecycle", .kind = "concept"});
  guard.nodes.push_back(cgraph::Node{.id = "k2", .label = "OpenSpec Change Lifecycle", .kind = "concept"});
  // ...but a site-less concept must never absorb a sited code symbol, because
  // unite() keeps the lower index and enrichment nodes are appended last, so the
  // concept would be the one deleted.
  guard.nodes.push_back(at("k3", "PaymentProcessor", "pay.cpp", 40));
  guard.nodes.push_back(cgraph::Node{.id = "k4", .label = "PaymentProcessor", .kind = "concept"});

  // Two sibling files with near-identical path-tail labels are DISTINCT files
  // and must never merge (a file's identity is its path). Merging one away would
  // destroy every import/contains edge that referenced it.
  guard.nodes.push_back(cgraph::Node{.id = "f1", .label = "viewers/compiq-viewer.tsx", .source_file = "viewers/compiq-viewer.tsx", .kind = "file"});
  guard.nodes.push_back(cgraph::Node{.id = "f2", .label = "viewers/compiq-viewer-states.tsx", .source_file = "viewers/compiq-viewer-states.tsx", .kind = "file"});

  // An enrichment document's identity is the file it was written from. A change's
  // proposal and its tasks are two different documents about one subject: labels
  // similar enough to cross the fuzzy threshold, a source_file each, and no
  // source_location -- so neither the identical-label guard nor the
  // declaration-site guard can fire. Merging them deletes real content (measured
  // on this repo's enriched graph: 44 documents and 144 edges per build).
  // The community property mirrors the graphs this bites in production: dedup
  // re-runs over already-published graphs whose nodes carry communities, which
  // buckets the pair as candidates at the lower 0.88 threshold.
  guard.nodes.push_back(cgraph::Node{.id = "doc1",
                                     .label = "persist-incremental-index/proposal.md",
                                     .source_file = "openspec/changes/persist-incremental-index/proposal.md",
                                     .kind = "document",
                                     .properties = {{"community", "openspec-docs"}}});
  guard.nodes.push_back(cgraph::Node{.id = "doc2",
                                     .label = "persist-incremental-index/tasks.md",
                                     .source_file = "openspec/changes/persist-incremental-index/tasks.md",
                                     .kind = "document",
                                     .properties = {{"community", "openspec-docs"}}});
  // Two records of ONE document (same source file) are still a duplicate and merge.
  guard.nodes.push_back(cgraph::Node{.id = "doc3",
                                     .label = "Host Skill Contract",
                                     .source_file = "docs/host-skill-contract.md",
                                     .kind = "document"});
  guard.nodes.push_back(cgraph::Node{.id = "doc4",
                                     .label = "Host Skill Contract",
                                     .source_file = "docs/host-skill-contract.md",
                                     .kind = "document"});
  // media carries the same shape (source_file, no source_location) and follows
  // the same file-scoped identity rule.
  guard.nodes.push_back(cgraph::Node{.id = "med1",
                                     .label = "Semantic Ingest Pipeline Proposal",
                                     .source_file = "docs/media/semantic-ingest-pipeline.svg",
                                     .kind = "media"});
  guard.nodes.push_back(cgraph::Node{.id = "med2",
                                     .label = "Semantic Ingest Pipeline Tasks",
                                     .source_file = "docs/media/semantic-ingest-flow.svg",
                                     .kind = "media"});
  // The guard fires when EITHER node is an enrichment kind: a cross-file
  // document-into-code-symbol merge deletes the document just the same, so
  // "either" must not decay to "both".
  guard.nodes.push_back(cgraph::Node{.id = "dsym",
                                     .label = "incremental_update_flow",
                                     .source_file = "src/engine/incremental_update.cpp",
                                     .source_location = cgraph::SourceLocation{.start_line = 12, .end_line = 12},
                                     .kind = "function",
                                     .properties = {{"community", "openspec-docs"}}});
  guard.nodes.push_back(cgraph::Node{.id = "doc5",
                                     .label = "incremental_update_notes",
                                     .source_file = "docs/incremental-update-notes.md",
                                     .kind = "document",
                                     .properties = {{"community", "openspec-docs"}}});
  // `kind` is verbatim host input with no validation; a cased variant must not
  // bypass the guard.
  guard.nodes.push_back(cgraph::Node{.id = "doc6",
                                     .label = "plan-candidate-code-links/proposal.md",
                                     .source_file = "openspec/changes/plan-candidate-code-links/proposal.md",
                                     .kind = "Document",
                                     .properties = {{"community", "openspec-docs"}}});
  guard.nodes.push_back(cgraph::Node{.id = "doc7",
                                     .label = "plan-candidate-code-links/design.md",
                                     .source_file = "openspec/changes/plan-candidate-code-links/design.md",
                                     .kind = "Document",
                                     .properties = {{"community", "openspec-docs"}}});
  // An edge into each at-risk document: rewrite must keep resolving after dedup.
  guard.edges.push_back(cgraph::Edge{.source = "doc1", .target = "doc2", .relation = "REFERENCES"});

  cgraph::semantic_dedup(guard);

  bool saw_props = false;
  bool saw_func = false;
  std::size_t helper_widgets = 0;
  std::size_t config_loaders = 0;
  std::size_t file_nodes = 0;
  for (const auto& node : guard.nodes) {
    saw_props = saw_props || node.label == "MessageBubbleProps";
    saw_func = saw_func || node.label == "MessageBubble";
    helper_widgets += node.label == "HelperWidget" ? 1 : 0;
    config_loaders += node.label == "ConfigLoader" ? 1 : 0;
    file_nodes += node.kind == "file" ? 1 : 0;
  }
  if (file_nodes != 2) {
    std::cerr << "sibling file nodes were incorrectly merged\n";
    return 1;
  }
  if (!saw_props || !saw_func) {
    std::cerr << "prefix-extension pair was incorrectly merged\n";
    return 1;
  }
  if (helper_widgets != 2) {
    std::cerr << "cross-file identical labels were incorrectly merged\n";
    return 1;
  }
  // Every distinct declaration site survives; the one shared site collapses.
  for (const char* label : {"validate_semantic_fragment_json", "validate_semantic_fragment_file",
                            "drainer_installed", "drainer_uninstall", "supervisor_spec",
                            "supervisor_sync", "query_zero_hits", "query_zero_hit_rate"}) {
    std::size_t count = 0;
    for (const auto& node : guard.nodes) {
      count += node.label == label ? 1 : 0;
    }
    if (count != 1) {
      std::cerr << "distinct declaration site was fuzzy-merged away: " << label << " (" << count << ")\n";
      return 1;
    }
  }
  {
    std::size_t same_site = 0;
    for (const auto& node : guard.nodes) {
      same_site += node.label == "same_site_twice" ? 1 : 0;
    }
    if (same_site != 1) {
      std::cerr << "a genuine duplicate at one site failed to merge\n";
      return 1;
    }
  }
  {
    // Three overloads sharing a line survive, because the site carries the column.
    std::size_t three_up = 0;
    std::size_t concepts = 0;
    std::size_t payment_total = 0;
    std::size_t payment_concepts = 0;
    for (const auto& node : guard.nodes) {
      three_up += node.label == "three_up" ? 1 : 0;
      concepts += node.label == "OpenSpec Change Lifecycle" ? 1 : 0;
      if (node.label == "PaymentProcessor") {
        ++payment_total;
        payment_concepts += node.kind == "concept" ? 1 : 0;
      }
    }
    if (three_up != 3) {
      std::cerr << "overloads sharing a line were merged (site must be line AND column): " << three_up << "\n";
      return 1;
    }
    if (concepts != 1) {
      std::cerr << "site-less enrichment concepts failed to merge: " << concepts << "\n";
      return 1;
    }
    if (payment_total != 2 || payment_concepts != 1) {
      std::cerr << "a site-less concept merged with a sited code symbol\n";
      return 1;
    }
  }
  if (config_loaders != 1) {
    std::cerr << "same-file duplicate labels were not merged\n";
    return 1;
  }
  {
    // A change's proposal and tasks stay two documents; two records of one
    // document still merge; media follows the document rule.
    std::size_t change_docs = 0;
    std::size_t contract_docs = 0;
    std::size_t media_nodes = 0;
    for (const auto& node : guard.nodes) {
      change_docs += node.label == "persist-incremental-index/proposal.md" ||
                             node.label == "persist-incremental-index/tasks.md"
                         ? 1
                         : 0;
      contract_docs += node.label == "Host Skill Contract" ? 1 : 0;
      media_nodes += node.kind == "media" ? 1 : 0;
    }
    if (change_docs != 2) {
      std::cerr << "similar documents from different files were merged: " << change_docs << "\n";
      return 1;
    }
    if (media_nodes != 2) {
      std::cerr << "similar media from different files were merged: " << media_nodes << "\n";
      return 1;
    }
    if (contract_docs != 1) {
      std::cerr << "two records of one document failed to merge: " << contract_docs << "\n";
      return 1;
    }
    std::size_t either_scope = 0;
    std::size_t cased_docs = 0;
    for (const auto& node : guard.nodes) {
      either_scope += node.label == "incremental_update_notes" || node.label == "incremental_update_flow" ? 1 : 0;
      cased_docs += node.label == "plan-candidate-code-links/proposal.md" ||
                            node.label == "plan-candidate-code-links/design.md"
                        ? 1
                        : 0;
    }
    if (either_scope != 2) {
      std::cerr << "a cross-file document merged into a code symbol: " << either_scope << "\n";
      return 1;
    }
    if (cased_docs != 2) {
      std::cerr << "a cased enrichment kind bypassed the file-scoped guard: " << cased_docs << "\n";
      return 1;
    }
    for (const auto& edge : guard.edges) {
      if (edge.relation != "REFERENCES") {
        continue;
      }
      if (edge.source != "doc1" || edge.target != "doc2") {
        std::cerr << "document edge no longer resolves to both documents: " << edge.source << " -> "
                  << edge.target << "\n";
        return 1;
      }
    }
  }

  // Escape-path B: a non-compliant host omits source_file on two similar-labeled
  // document nodes. They share a community (so the fuzzy pass buckets them as
  // candidates at the 0.88 threshold) but carry no file to scope identity to.
  // With no source_file the pair's source_files compare equal (""=="") -- the
  // file-scoped guard must still keep them apart, or one document is silently
  // deleted. A document that dropped its file is not label-mergeable with
  // another document.
  {
    cgraph::GraphSnapshot g;
    g.nodes.push_back(cgraph::Node{.id = "b1",
                                   .label = "orphan-change/proposal.md",
                                   .kind = "document",
                                   .properties = {{"community", "openspec-docs"}}});
    g.nodes.push_back(cgraph::Node{.id = "b2",
                                   .label = "orphan-change/tasks.md",
                                   .kind = "document",
                                   .properties = {{"community", "openspec-docs"}}});
    cgraph::semantic_dedup(g);
    std::size_t sourceless_docs = 0;
    for (const auto& node : g.nodes) {
      sourceless_docs += (node.label == "orphan-change/proposal.md" ||
                          node.label == "orphan-change/tasks.md")
                             ? 1
                             : 0;
    }
    if (sourceless_docs != 2) {
      std::cerr << "source_file-less documents were fuzzy-merged (escape-path B): " << sourceless_docs
                << "\n";
      return 1;
    }
  }

  // No over-block: two documents that share the SAME real source_file but have
  // different (similar) labels must still merge -- the guard only keeps apart
  // pairs lacking a shared non-empty file. This path is otherwise untested and
  // is what the source_file.empty() clause protects.
  {
    cgraph::GraphSnapshot g;
    g.nodes.push_back(cgraph::Node{.id = "s1",
                                   .label = "reproducible-ci-dependencies-notes-one",
                                   .source_file = "openspec/changes/reproducible-ci/notes.md",
                                   .kind = "document",
                                   .properties = {{"community", "openspec-docs"}}});
    g.nodes.push_back(cgraph::Node{.id = "s2",
                                   .label = "reproducible-ci-dependencies-notes-two",
                                   .source_file = "openspec/changes/reproducible-ci/notes.md",
                                   .kind = "document",
                                   .properties = {{"community", "openspec-docs"}}});
    cgraph::semantic_dedup(g);
    std::size_t same_file_docs = 0;
    for (const auto& node : g.nodes) {
      same_file_docs += (node.label == "reproducible-ci-dependencies-notes-one" ||
                         node.label == "reproducible-ci-dependencies-notes-two")
                            ? 1
                            : 0;
    }
    if (same_file_docs != 1) {
      std::cerr << "two documents sharing one real source_file failed to merge: " << same_file_docs
                << "\n";
      return 1;
    }
  }
  return 0;
}
