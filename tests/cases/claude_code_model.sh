#!/bin/bash
# SPDX-License-Identifier: MIT
# The model selects the Claude Code CLI: `model claude-code/MODEL` switches to
# it and stores the grammar with the model, a bare model keeps it, the CLI
# gets the model, and a model that a catalogue provider pins leaves it, also
# when the grammar was set by hand.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
fake_claude_install claude_code_model.json
"$FYAI_BIN" --set "claude_code/command=$FAKE_CLAUDE" >/dev/null 2>&1 ||
	fail "configure the project"

stored() {
	run_fyai config get "$1"
	assert_status 0
	[ "$(tail -n 1 "$TEST_DIR/stdout")" = "$2" ] ||
		fail "$1 is $(tail -n 1 "$TEST_DIR/stdout"), not $2"
}

# The model that the last CLI was started with.
cli_model() {
	"$PYTHON" - "$FAKE_CLAUDE_LOG" "$1" <<'PYEOF' || fail "the CLI did not get the model $1"
import json, sys
argv = [json.loads(l) for l in open(sys.argv[1]) if '"event": "start"' in l][-1]["argv"]
assert argv[argv.index("--model") + 1] == sys.argv[2], argv
PYEOF
}

run_fyai model claude-code/sonnet
assert_status 0
assert_stdout_contains "provider claude-code, api claude-code"
stored api claude-code
stored model "'claude-code/sonnet'"
run_fyai "hello"
assert_status 0
assert_stdout_contains "answered"
cli_model sonnet

run_fyai model opus
assert_status 0
stored model "'claude-code/opus'"
run_fyai "hello"
assert_status 0
cli_model opus

run_fyai model openai/gpt-5.4-mini
assert_status 0
assert_stdout_contains "provider openai, api responses"
stored api responses
stored model "'openai/gpt-5.4-mini'"

"$FYAI_BIN" --set api=claude-code >/dev/null 2>&1 || fail "select the CLI by hand"
run_fyai model openai/gpt-5.4-mini
assert_status 0
assert_stdout_contains "provider openai, api responses"
stored api responses
