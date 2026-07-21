#!/bin/bash
# SPDX-License-Identifier: MIT
# A signal arriving while a shell tool is being captured must not lose the
# tool's output.
#
# The interactive REPL installs SIGINT/SIGWINCH handlers without SA_RESTART
# (src/fyai_signal.c), so a signal delivered mid-capture interrupts the wait.
# The old select() loop treated any -1 as fatal: EINTR abandoned the capture
# and dropped into a blocking waitpid(), losing everything the command had
# already written and everything it wrote afterwards. The event loop reports
# EINTR as a spurious wakeup instead, so the capture simply resumes.
#
# The shell command signals fyai itself ("kill -INT $PPID", the capturing
# parent) rather than the case script doing it on a timer. That is deliberate:
# a wall-clock kill only lands inside the capture window when startup and the
# first mock request happen to be fast enough, so the timed version passed
# against the buggy code roughly half the time. Signalling from inside the
# command puts the delivery squarely in the middle of the capture, every run.
#
# The signal goes to fyai alone, not to the process group, so the shell child
# keeps running - which is what makes this discriminating: the capture has to
# survive the interruption and still collect the output produced after it.
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
# ...and so does output written after it, proving the capture resumed rather
# than being abandoned at the first EINTR.
grep -qxF "late-marker" "$TEST_DIR/stderr" || \
	{ cat "$TEST_DIR/stderr" >&2; fail "capture abandoned on EINTR: post-signal output lost"; }

# The interrupt still took effect - it aborts the follow-up model call, it does
# not silently vanish.
assert_stderr_contains "interrupted"

pass
