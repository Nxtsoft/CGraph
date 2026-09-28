#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cgraph {

[[nodiscard]] std::string make_id(std::string_view input);

// The id input of a relative import's stub, before make_id. `joined` is the
// importing file's project-relative directory joined with the import's
// relative path; `kind` is "module" or "symbol". make_id reduces every run of
// punctuation to one separator, so "./util", "../util" and a bare "util" would
// all normalize alike. The distinction therefore lives in the namespace, ahead
// of the path, where no path can reach: "import-relative-<kind>:" for a path
// inside the root, "import-relative-up-<N>-<kind>:" for one that climbs N
// levels above it, versus the "import-<kind>:" namespace bare packages and
// absolute Python modules use. For stub ids only: resolve_imports matches
// `import_path`, which stays resolved against the absolute path.
[[nodiscard]] std::string relative_import_stub(std::string_view kind, const std::filesystem::path& joined);

}  // namespace cgraph
