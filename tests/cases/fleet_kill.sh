#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify `fleet kill` stops live sessions for a single project or the entire
# fleet while leaving session history stored.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start serve_idle.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
for p in a b; do
	mkdir -p $p/.git
	(cd $p && "$FYAI_BIN" init >/dev/null 2>&1 &&
		"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
done
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'
mock_watch

# pid_of PROJECT: print PID of PROJECT's live session, or empty if none.
pid_of() {
	"$FYAI_BIN" fleet list --output json | "$PYTHON" -c '
import json, sys
for r in json.load(sys.stdin):
    if r["project"] == sys.argv[1] and r.get("pid"):
        print(r["pid"])' "$1"
}

run_fyai fleet start --branch fleet/work a Work
assert_status 0
run_fyai fleet start --branch fleet/work b Work
assert_status 0
wait_requests 2
pa="$(pid_of a)"
pb="$(pid_of b)"
[ -n "$pa" ] && [ -n "$pb" ] || fail "the members do not run: '$pa' '$pb'"

# Targeted kill stops only project `a`.
run_fyai fleet kill a
assert_status 0
assert_stdout_contains "a:fleet/work"
"$PYTHON" "$TESTS_DIR/wait_exit.py" "$pa" "$((30 * FYAI_TIMEOUT_SCALE))" ||
	fail "the session of a did not end"
[ -z "$(pid_of a)" ] || fail "a is still listed live"
[ "$(pid_of b)" = "$pb" ] || fail "b did not stay"

# Fleet-wide kill stops remaining live sessions.
run_fyai fleet kill
assert_status 0
assert_stdout_contains "b:fleet/work"
"$PYTHON" "$TESTS_DIR/wait_exit.py" "$pb" "$((30 * FYAI_TIMEOUT_SCALE))" ||
	fail "the session of b did not end"
run_fyai fleet kill
assert_status 0
assert_stdout_contains "no live sessions"
run_fyai fleet list a
assert_stdout_contains "stored"
pass
