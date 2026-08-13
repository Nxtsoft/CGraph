#pragma once

#include <string_view>

namespace cgraph {

struct BuildInfo {
  std::string_view name;
  std::string_view version;   // semver of the engine surface
  std::string_view revision;  // source revision the binary was configured from
};

[[nodiscard]] BuildInfo build_info() noexcept;

}  // namespace cgraph
