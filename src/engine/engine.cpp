#include "cgraph/engine.hpp"

// Configure-time source revision (git describe --always --dirty --tags),
// injected by src/engine/CMakeLists.txt; "unknown" outside a git checkout.
#ifndef CGRAPH_GIT_DESCRIBE
#define CGRAPH_GIT_DESCRIBE "unknown"
#endif

namespace cgraph {

BuildInfo build_info() noexcept {
  return BuildInfo{
      .name = "cgraph-native",
      .version = "0.1.0",
      .revision = CGRAPH_GIT_DESCRIBE,
  };
}

}  // namespace cgraph
