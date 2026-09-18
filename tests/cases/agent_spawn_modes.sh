#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify both ways to start a sub-agent child.
#
# The default child executes fyai again and reads its fork point from the
# arena. `agent/spawn: fork` keeps the forked image. Both must start the
# sub-agent from the conversation of the parent.
set -eu
. "$(dirname "$0")/../harness.sh"

# Print "exec" when the sub-agent child started as a new process image, else
# "fork". Only an executed child records an "exec" line.
spawn_kind() {
	"$PYTHON" - "$1" <<'PY'
import re
import sys

spawned = set()
executed = set()
unpinned = False
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r" spawn: \S*agent:\S*, pid (\d+)", line)
    if m:
        spawned.add(m.group(1))
    m = re.match(r"\S+ (\d+) exec: ", line)
    if m:
        executed.add(m.group(1))
    if "is not published; using the published head" in line:
        unpinned = True
if not spawned:
    raise SystemExit("no sub-agent was spawned")
if unpinned:
    raise SystemExit("the fork head was not found in the ref log")
print("exec" if spawned <= executed else "fork")
PY
}

for mode in exec fork; do
	fyai_test_setup
	mock_start agent_fork.json
	export FYAI_TRACE="$TEST_DIR/trace-$mode.log"

	run_fyai --set display/stream=false --set tools=true --set api=responses \
		 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
		 --set 'agent/personas/explore/description=Fork model test.' \
		 --set 'agent/personas/explore/system_prompt=You are the fork tester.' \
		 --set 'agent/personas/explore/model=mock-model-mini' \
		 --set "agent/spawn=$mode" \
		 "remember a pass phrase"
	assert_status 0

	run_fyai --set display/stream=false --set tools=true --set api=responses \
		 --set api_url="$MOCK_URL/v1/responses" -m mock-model \
		 "ask a sub-agent to continue"
	assert_status 0

	# The sub-agent request carries the persona and the turn of the parent.
	assert_any_request \
	    "'fork tester' in json.dumps(r['body'].get('input','')) and \
	     'MAGENTA-KESTREL' in json.dumps(r['body'].get('input',''))"
	# A command-line key reaches the child.
	assert_request 1 'r["auth"] == "Bearer test-key"'

	kind="$(spawn_kind "$FYAI_TRACE")" || fail "$mode: $kind"
	[ "$kind" = "$mode" ] || fail "agent/spawn=$mode started a $kind child"

	run_fyai branch --all
	assert_status 0
	assert_stdout_contains "main/agent:explore"

	mock_stop 4
	unset FYAI_TRACE
done
pass
