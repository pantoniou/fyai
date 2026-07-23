#!/bin/bash
# SPDX-License-Identifier: MIT
# A signal terminating an asynchronous shell worker must preserve output
# already captured and produce a valid failed tool result.
#
# The shell command signals fyai itself ("kill -INT $PPID", the capturing
# parent) rather than the case script doing it on a timer. That is deliberate:
# a wall-clock kill only lands inside the capture window when startup and the
# first mock request happen to be fast enough, so the timed version passed
# against the buggy code roughly half the time. Signalling from inside the
# command puts the delivery squarely in the middle of the capture, every run.
#
# The signal targets the worker that captures the shell, exercising the same
# missing-final-JSON path as user cancellation.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_capture_interrupt.json

cat > catalog.yaml <<EOF
models:
- name: foo
  capabilities: []
providers:
- name: mockprov
  root_url: $MOCK_URL
  endpoints:
  - protocol: chat_completions
    endpoint: /v1/chat/completions
  models:
  - canonical_id: foo
    provider_model_id: bar
EOF
run_fyai catalog import catalog.yaml
assert_status 0

export MOCKPROV_API_KEY=mock-secret

set +e
"$FYAI_BIN" --color off --set display/markdown=false --set display/stream=false \
	-i --set tools=true --set builtin_shell=true -m foo \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" <<'EOF' &
run the shell command
EOF
FPID=$!

# The command signals fyai on its own; nothing to time from here.
# The whole run must still finish promptly - a hang here is the regression,
# the old path blocked in waitpid() with the capture already abandoned.
for i in $(seq 1 100); do
	kill -0 "$FPID" 2>/dev/null || break
	sleep 0.1
done
kill -0 "$FPID" 2>/dev/null && { kill -KILL "$FPID" 2>/dev/null; fail "fyai hung after SIGINT during shell capture"; }

wait "$FPID"
FYAI_STATUS=$?
set -e

# The shell tool streams its output live to stderr, alongside the REPL banner.
# Output written before the signal survives...
grep -qxF "early-marker" "$TEST_DIR/stderr" || \
	{ cat "$TEST_DIR/stderr" >&2; fail "output before the signal was lost"; }
# The terminated worker must not continue to its late output.
if grep -qxF "late-marker" "$TEST_DIR/stderr"; then
	fail "terminated shell worker produced late output"
fi

# Collection synthesizes a valid failed result when the worker cannot emit
# its normal final JSON.
assert_request 1 \
	'any(m.get("role") == "tool" and "interrupted" in '\
'm.get("content", "") for m in r["body"]["messages"])'

pass
