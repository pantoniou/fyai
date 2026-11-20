#!/bin/bash
# SPDX-License-Identifier: MIT
# The model of a root session works with the fleet through the `fleet` tool:
# it finds a session by the words of its conversation, reads it, and starts a
# session with a task, which the root keeps. A session of a project with no
# project below it has no such tool.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_tool.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
mock_watch

# A session of a project with nothing below it: no fleet tool.
(cd a && "$FYAI_BIN" "The build failure is in parser.c" 8<&-) \
	>"$TEST_DIR/a.out" 2>&1 || fail "the run in the project failed: $(cat "$TEST_DIR/a.out")"
wait_requests 1
assert_request 0 'not any(t["function"]["name"] == "fleet" for t in r["body"].get("tools", []))'

# Nobody answers a question in a run of a prompt: the root asks for none.
"$FYAI_BIN" config set fleet/approve never >/dev/null 2>&1 ||
	fail "cannot set fleet/approve"
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

# The root session: search, read, start.
"$FYAI_BIN" -b main "Search the fleet" 8<&- >"$TEST_DIR/root.out" 2>&1 ||
	fail "the root run failed: $(cat "$TEST_DIR/root.out")"
grep -q "Started it." "$TEST_DIR/root.out" ||
	fail "the root run did not end: $(cat "$TEST_DIR/root.out")"
wait_requests 6
assert_request 1 'any(t["function"]["name"] == "fleet" for t in r["body"].get("tools", []))'
# The search found the session by a word of its conversation, in any case.
assert_request 2 'json.loads(r["body"]["messages"][-1]["content"])["found"][0]["name"] == "a:main"'
# The read gave its messages.
assert_request 3 '"parser.c" in r["body"]["messages"][-1]["content"] and "Noted the parser." in r["body"]["messages"][-1]["content"]'
"$PYTHON" - "$TEST_DIR/requests.jsonl" <<'PY' || fail "the started session did not run its task"
import json, sys
reqs = [json.loads(l) for l in open(sys.argv[1])]
member = [r for r in reqs if any(m.get("role") == "user" and m.get("content") == "Fix the parser now"
                                 for m in r["body"]["messages"])]
assert len(member) == 1, len(member)
PY

"$FYAI_BIN" fleet tasks --output json >"$TEST_DIR/tasks.json"
"$PYTHON" - "$TEST_DIR/tasks.json" <<'PY' || fail "the root keeps no task: $(cat "$TEST_DIR/tasks.json")"
import json, sys
rows = json.load(open(sys.argv[1]))
assert [(r['id'], r['session'], r['name'], r['state']) for r in rows] == \
    [(1, 'main', 'a:fleet/fix-the-parser-now', 'open')], rows
PY
pass
