#!/usr/bin/env sh
# Claude Code PreToolUse hook for Edit|Write|MultiEdit. Before a file is edited,
# names the other services on the far side of every HTTP endpoint the file
# serves or calls (from the cgraph.workspace.json above its repository), as
# additionalContext. Prints nothing when there is nothing to say; never blocks
# the edit. CGRAPH_HOOK_WAIT_MS bounds the wait for cold daemons (default 3000).
set -eu

CGRAPH_CLIENT="${CGRAPH_CLIENT:-cgraph-client}"

if [ "${CGRAPH_DAEMON:-}" != "" ]; then
  exec "${CGRAPH_CLIENT}" --daemon "${CGRAPH_DAEMON}" pre-edit
fi

exec "${CGRAPH_CLIENT}" pre-edit
