#!/usr/bin/env sh
# Claude Code PreToolUse hook for Edit|Write|MultiEdit. Before a file is edited,
# names the files most likely to change with the edited lines and, inside a
# cgraph.workspace.json, the other services on the far side of every HTTP
# endpoint the file serves or calls, as additionalContext. Prints nothing when
# there is nothing to say; never blocks the edit. Asks only daemons already
# running, never starts one, and gives up silently after CGRAPH_HOOK_WAIT_MS
# for the whole hook (default 250).
set -eu

CGRAPH_CLIENT="${CGRAPH_CLIENT:-cgraph-client}"

if [ "${CGRAPH_DAEMON:-}" != "" ]; then
  exec "${CGRAPH_CLIENT}" --daemon "${CGRAPH_DAEMON}" pre-edit
fi

exec "${CGRAPH_CLIENT}" pre-edit
