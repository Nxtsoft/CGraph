#pragma once

#include <nlohmann/json.hpp>

namespace cgraph {

// Builds two isolated in-memory source snapshots and validates a supplied unified
// diff. Never edits either root or publishes into a resident daemon. Exceptions
// reject the whole operation; callers must not emit a partial success response.
[[nodiscard]] nlohmann::json change_context(const nlohmann::json& parameters);

}  // namespace cgraph
