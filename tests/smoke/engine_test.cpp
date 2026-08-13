// Build identity: the stamped revision is what lets an operator (or an agent)
// tell a stale installed binary from the checkout it was built from -- a
// pre-fix daemon once served an outdated graph for days with nothing on any
// surface to say so.

#include "cgraph/engine.hpp"

#include <string>

int main() {
  const auto info = cgraph::build_info();
  if (info.name.empty() || info.version.empty() || info.revision.empty()) {
    return 1;
  }
  // "unknown" is the documented fallback outside a git checkout; a git build
  // (every dev and CI build) must carry a real describe string.
  if (info.revision == "unknown") {
    return 1;
  }
  return 0;
}
