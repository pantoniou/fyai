#!/bin/bash
# SPDX-License-Identifier: MIT
# A sandboxed tool sub-execution sanitizes the environment (secrets in the
# parent env never reach the tool) and, when Landlock is available, confines the
# tool - without breaking ordinary shell operations. Environment sanitization is
# location- and kernel-independent, so this case is robust where Landlock is
# absent (the FS confinement itself is exercised directly in the unit path).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

export MY_SECRET=topsecret-xyz
export OPENAI_API_KEY=sk-should-not-leak

# The tool's environment is stripped of everything but a safe allow-list, so
# neither an arbitrary secret nor a provider key is visible to the command.
run_fyai --set sandbox=true tool shell '{"command": "echo v=[$MY_SECRET] k=[$OPENAI_API_KEY]"}'
assert_status 0
assert_stdout_contains "v=[] k=[]"
assert_stdout_not_contains "topsecret-xyz"

# PATH is preserved so tools still resolve.
run_fyai --set sandbox=true tool shell '{"command": "test -n \"$PATH\" && echo path-ok"}'
assert_stdout_contains "path-ok"

# The sandbox does not break /dev/null redirection or basic commands.
run_fyai --set sandbox=true tool shell '{"command": "echo hi > /dev/null && echo devnull-ok"}'
assert_stdout_contains "devnull-ok"

# A file in the project stays readable under the sandbox.
run_fyai --set sandbox=false tool write_file '{"path": "keep.txt", "content": "visible\n"}'
run_fyai --set sandbox=true tool shell '{"command": "cat keep.txt"}'
assert_stdout_contains "visible"

# Landlock denies apply only on Linux.
if [ "$(uname -s)" = Linux ]; then
	SECRET_DIR="$TEST_DIR/scratch-secret"
	mkdir -p "$SECRET_DIR"
	printf 'classified\n' >"$SECRET_DIR/s.txt"
	DENY_CONFIG="$TEST_DIR/deny-sandbox.yaml"
	printf 'sandbox: { enabled: true, deny: [%s] }\n' "$SECRET_DIR" >"$DENY_CONFIG"
	run_fyai --config "$DENY_CONFIG" tool shell \
		'{"command": "cat '"$SECRET_DIR"'/s.txt || echo deny-ok"}'
	assert_stdout_contains "deny-ok"
	assert_stdout_not_contains "classified"
fi

# A misspelled allow mode must fail before the command runs. An explicit
# config file is validated at ingestion, so the schema rejects it there.
BAD_CONFIG="$TEST_DIR/bad-sandbox.yaml"
printf '%s\n' 'sandbox: { enabled: true, allow: [{ path: /tmp, mode: readonly }] }' \
	>"$BAD_CONFIG"
run_fyai --config "$BAD_CONFIG" tool shell '{"command": "echo must-not-run"}'
assert_status_nonzero
assert_stderr_contains "schema validation failed"
assert_stdout_not_contains "must-not-run"

pass
