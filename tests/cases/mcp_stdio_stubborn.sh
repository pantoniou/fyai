#!/bin/bash
# SPDX-License-Identifier: MIT
# A stdio MCP server that ignores EOF and SIGTERM must still be shut down, and
# must not hold the invocation open.
#
# mcp_stdio.sh covers the polite path, where the server exits as soon as its
# stdin closes. This drives the escalation instead: close the pipes (ignored),
# SIGTERM (ignored), then SIGKILL. The client waits on the child through the
# event loop at each stage, so the whole sequence is bounded by its timeouts
# rather than by a fixed sleep-poll budget.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start mcp_stdio.json

start=$(date +%s)
run_fyai --set api=chat-completions --set display/stream=false \
	--set mcp/enabled=true \
	--set "mcp/servers/local={transport: stdio, command: '$PYTHON', args: ['$TESTS_DIR/mock/mock_mcp_stdio_stubborn.py', '$TEST_DIR'], cwd: '$TEST_DIR'}" \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "use local echo"
elapsed=$(( $(date +%s) - start ))

assert_status 0
assert_stdout_contains "Stdio MCP completed."

# The call itself succeeded, so the shutdown - not the protocol - is what this
# case is about: it must terminate, and within the escalation budget rather
# than hanging on a server that never leaves.
[ "$elapsed" -lt 30 ] || fail "shutdown took ${elapsed}s; escalation did not terminate the server"

# The server and its descendants must stop before the invocation exits. The
# server has this run's scratch directory in its arguments, so the search finds
# server and never another run's: two suites on one machine used to see each
# other's children and report a leak neither produced. The bracket prevents the
# pattern from matching the command line of whatever shell is running this.
leaked=$(pgrep -f "mock_mcp_stdio[_]stubborn.py $TEST_DIR" || true)
if [ -n "$leaked" ]; then
	kill -KILL $leaked 2>/dev/null || true
	fail "stdio server outlived fyai"
fi

mock_stop 2
pass
