// fingerprint.cpp: rename-insensitive function fingerprints for `report clones`
// -- identifier/literal normalization, shingle winnowing, Jaccard similarity,
// determinism, the token count, and the extraction hook that fills
// Fragment::fingerprints for every function node.
#include "cgraph/fingerprint.hpp"

#include "cgraph/javascript_extractor.hpp"
#include "cgraph/python_extractor.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& what) {
  std::cerr << "fingerprint_test: " << what << '\n';
  return 1;
}

const cgraph::FunctionFingerprint* fingerprint_of(const cgraph::ExtractionResult& result, const std::string& label) {
  for (const auto& node : result.fragment.nodes) {
    if (node.kind == "function" && node.label == label) {
      const auto it = result.fragment.fingerprints.find(node.id);
      return it == result.fragment.fingerprints.end() ? nullptr : &it->second;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  // The three write_file copies from tests/smoke, as TypeScript: identifiers,
  // string literals and numbers differ, the token stream does not.
  const auto ts = cgraph::extract_typescript({.source_file = "a.ts", .source = R"ts(
function writeFile(path: string, contents: string) {
  const dir = dirname(path);
  mkdir(dir, { recursive: true });
  const out = open(path, "w");
  out.write(contents);
  out.close();
  return 0;
}
function persist(target: string, body: string) {
  const folder = dirname(target);
  mkdir(folder, { recursive: false });
  const handle = open(target, 'wb');
  handle.write(body);
  handle.close();
  return 42;
}
function edited(path: string, contents: string) {
  const dir = dirname(path);
  mkdir(dir, { recursive: true });
  const out = open(path, "w");
  out.write(contents);
  out.flush();
  out.write(contents);
  out.close();
  return 0;
}
function unrelated(xs: number[]) {
  let total = 0;
  for (const x of xs) {
    if (x > 0) {
      total += x * x;
    }
  }
  return total / xs.length;
}
function tiny() { return 1; }
)ts"});
  const auto* write = fingerprint_of(ts, "writeFile");
  const auto* persist = fingerprint_of(ts, "persist");
  const auto* edited = fingerprint_of(ts, "edited");
  const auto* unrelated = fingerprint_of(ts, "unrelated");
  const auto* tiny = fingerprint_of(ts, "tiny");
  if (write == nullptr || persist == nullptr || edited == nullptr || unrelated == nullptr || tiny == nullptr) {
    return fail("every function node gets a fingerprint through extraction");
  }
  if (write->tokens == 0 || write->shingles.empty() || write->tokens != persist->tokens) {
    return fail("a body has tokens and shingles; renaming does not change the token count");
  }
  if (write->shingles != persist->shingles || cgraph::fingerprint_similarity(*write, *persist) != 1.0) {
    return fail("identifiers, string literals and numbers are normalized away: the copies are identical");
  }
  const auto edited_similarity = cgraph::fingerprint_similarity(*write, *edited);
  if (edited_similarity >= 1.0 || edited_similarity < 0.5) {
    return fail("two inserted statements lower the similarity without erasing it (" + std::to_string(edited_similarity) + ")");
  }
  if (cgraph::fingerprint_similarity(*write, *unrelated) > 0.2) {
    return fail("an unrelated body shares almost nothing (" + std::to_string(cgraph::fingerprint_similarity(*write, *unrelated)) + ")");
  }
  if (tiny->tokens >= 30 || tiny->shingles.empty()) {
    return fail("a one-line body is below the default token floor but still fingerprinted");
  }
  if (!std::is_sorted(write->shingles.begin(), write->shingles.end()) ||
      std::adjacent_find(write->shingles.begin(), write->shingles.end()) != write->shingles.end()) {
    return fail("shingle sets are sorted and unique so Jaccard is a merge walk");
  }
  // Winnowing keeps a bounded fraction of the shingles: never more than there are
  // shingle positions, and at least one per window.
  const auto positions = write->tokens >= cgraph::kShingleSize ? write->tokens - cgraph::kShingleSize + 1 : 1;
  if (write->shingles.size() > positions || write->shingles.size() * cgraph::kWinnowWindow < positions) {
    return fail("winnowing selects at most one hash per position and at least one per window");
  }

  // Determinism: the same source fingerprints identically across runs and files.
  const auto again = cgraph::extract_typescript({.source_file = "b.ts", .source = R"ts(
function writeFile(path: string, contents: string) {
  const dir = dirname(path);
  mkdir(dir, { recursive: true });
  const out = open(path, "w");
  out.write(contents);
  out.close();
  return 0;
}
)ts"});
  const auto* repeat = fingerprint_of(again, "writeFile");
  if (repeat == nullptr || repeat->shingles != write->shingles || repeat->tokens != write->tokens) {
    return fail("fingerprints are deterministic");
  }

  // Comments never count; a Python body (no separate body field beyond `block`)
  // normalizes the same way, and the same algorithm with a different grammar
  // still equates renamed copies.
  const auto py = cgraph::extract_python({.source_file = "m.py", .source = R"py(
def write_file(path, contents):
    # a comment that must not matter
    directory = dirname(path)
    makedirs(directory, exist_ok=True)
    with open(path, "w") as handle:
        handle.write(contents)
    return 0

def persist(target, body):
    folder = dirname(target)
    makedirs(folder, exist_ok=False)
    with open(target, 'wb') as fh:
        fh.write(body)
    return 1
)py"});
  const auto* py_write = fingerprint_of(py, "write_file");
  const auto* py_persist = fingerprint_of(py, "persist");
  if (py_write == nullptr || py_persist == nullptr || py_write->shingles != py_persist->shingles ||
      py_write->tokens != py_persist->tokens) {
    return fail("Python copies with different names, literals and a comment fingerprint identically");
  }
  if (cgraph::fingerprint_similarity(*write, *py_write) == 1.0) {
    return fail("different languages with different syntax do not collide by accident");
  }
  cgraph::FunctionFingerprint empty;
  if (cgraph::fingerprint_similarity(empty, empty) != 0.0) {
    return fail("two empty fingerprints are not similar");
  }
  return 0;
}
