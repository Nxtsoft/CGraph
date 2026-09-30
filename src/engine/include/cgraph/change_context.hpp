#pragma once

#include "cgraph/workspace.hpp"

#include <nlohmann/json.hpp>

namespace cgraph {

// The other services of the workspace the target root belongs to, and a way to
// ask their daemons. With it, change_context adds a `cross_service` section:
// the consumers (in other repos) of every endpoint the change serves, and the
// providers of every endpoint it consumes.
struct CrossServiceAsk {
  const EnclosingWorkspace* enclosing = nullptr;
  RepoAsk ask;
};

// Builds two isolated in-memory source snapshots and validates a supplied unified
// diff. Never edits either root or publishes into a resident daemon. Exceptions
// reject the whole operation; callers must not emit a partial success response.
[[nodiscard]] nlohmann::json change_context(const nlohmann::json& parameters,
                                            const CrossServiceAsk* cross_service = nullptr);

}  // namespace cgraph
