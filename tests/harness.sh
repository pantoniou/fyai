# SPDX-License-Identifier: MIT
# harness.sh - shared plumbing for the fyai functional test cases.
#
# Every case script sources this file. Contract (set by CMake/add_test env or
# by the caller):
#   FYAI_BIN   - path to the fyai binary under test        (or argv[1])
#   PYTHON     - python3 interpreter                        (default python3)
#   TESTS_DIR  - the source tests/ directory                (or derived)
#
# Each case runs in its own scratch directory with HOME/XDG_* redirected into
# it, so nothing touches the real ~/.fyai, ~/.config or the network.

set -u

FYAI_BIN="${FYAI_BIN:-${1:-}}"
PYTHON="${PYTHON:-python3}"
TESTS_DIR="${TESTS_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
SCENARIOS_DIR="$TESTS_DIR/scenarios"
MOCK_PROVIDER="$TESTS_DIR/mock/mock_provider.py"
FYAI_VALGRIND="${FYAI_VALGRIND:-}"

[ -x "$FYAI_BIN" ] || { echo "harness: FYAI_BIN not set/executable" >&2; exit 99; }

MOCK_PID=""
TEST_DIR=""

# How much longer every wait in a case waits. A sanitized build, or a suite run
# with many jobs at once, takes several times as long for the same work: a case
# must wait that much longer rather than report a failure that says nothing
# about the code. CMake sets it for the sanitized tree; the PTY drivers read
# the same variable. An exported FYAI_TIMEOUT_SCALE wins over it, so one run
# can wait longer without reconfiguring.
FYAI_TIMEOUT_SCALE="${FYAI_TIMEOUT_SCALE:-${FYAI_TIMEOUT_SCALE_DEFAULT:-1}}"
case "$FYAI_TIMEOUT_SCALE" in
*[!0-9]*|"") FYAI_TIMEOUT_SCALE=1 ;;
0) FYAI_TIMEOUT_SCALE=1 ;;
esac
export FYAI_TIMEOUT_SCALE

fail() {
	echo "FAIL: $*" >&2
	[ -f "$TEST_DIR/stdout" ] && { echo "--- stdout ---" >&2; cat "$TEST_DIR/stdout" >&2; }
	[ -f "$TEST_DIR/stderr" ] && { echo "--- stderr ---" >&2; cat "$TEST_DIR/stderr" >&2; }
	[ -f "$TEST_DIR/requests.jsonl" ] && { echo "--- requests ---" >&2; cat "$TEST_DIR/requests.jsonl" >&2; }
	exit 1
}

# A case that needs what this platform cannot do reports it and is not run.
# CTest reads status 77 as a skip.
skip() {
	echo "SKIP: $*" >&2
	exit 77
}

fyai_test_cleanup() {
	mock_stop_quiet
	if [ -n "$TEST_DIR" ]; then
		rm -rf "$TEST_DIR"
		TEST_DIR=""
	fi
}

# A signal leaves no scratch behind: the shell runs the EXIT trap only on a
# normal exit, so a case stopped by a CTest timeout must clean up here.
fyai_test_signal() {
	local sig="$1"

	fyai_test_cleanup
	trap - "$sig"
	kill -s "$sig" $$
}

# The sandbox without a project: scratch dir, redirected HOME/XDG_*, scrubbed
# provider env. A case that tests what fyai does with no arena uses this one.
fyai_test_setup_bare() {
	# A case can set up more than one time. Remove the sandbox of the
	# previous one, and keep making each new one beside it and not in it.
	: "${FYAI_TMPDIR_BASE:=${TMPDIR:-/tmp}}"
	if [ -n "$TEST_DIR" ]; then
		cd "$FYAI_TMPDIR_BASE" || exit 99
		fyai_test_cleanup
	fi
	TEST_DIR="$(mktemp -d "$FYAI_TMPDIR_BASE/fyai-test-XXXXXX")" || exit 99
	trap fyai_test_cleanup EXIT
	for sig in HUP INT QUIT TERM; do
		trap "fyai_test_signal $sig" "$sig"
	done
	cd "$TEST_DIR" || exit 99

	# Every temporary file the run makes lands in the scratch dir, so the
	# cleanup removes it with the rest of the case.
	export TMPDIR="$TEST_DIR/tmp"
	mkdir -p "$TMPDIR"

	export HOME="$TEST_DIR/home"
	export XDG_STATE_HOME="$TEST_DIR/home/.local/state"
	export XDG_CONFIG_HOME="$TEST_DIR/home/.config"
	mkdir -p "$HOME"
	unset OPENAI_API_KEY OPENROUTER_API_KEY DEEPSEEK_API_KEY ANTHROPIC_API_KEY \
	      GOOGLE_API_KEY || true
}

# Create the sandbox: scratch dir, redirected HOME/XDG_*, scrubbed provider
# env, and a local .fyai so the arena walk-up lands here.
fyai_test_setup() {
	fyai_test_setup_bare

	printf 'display:\n  markdown: false\n' > config.yaml
	${FYAI_VALGRIND} "$FYAI_BIN" init >/dev/null 2>&1 || fail "fyai init"
	rm -f config.yaml
}

# mock_start <scenario.json relative to tests/scenarios, or absolute>
mock_start() {
	local scenario="$1"

	[ -f "$scenario" ] || scenario="$SCENARIOS_DIR/$1"
	[ -f "$scenario" ] || fail "scenario not found: $1"

	rm -f "$TEST_DIR/port" "$TEST_DIR/requests.jsonl" "$TEST_DIR/served"
	"$PYTHON" "$MOCK_PROVIDER" "$scenario" "$TEST_DIR" \
		2>"$TEST_DIR/mock.log" &
	MOCK_PID=$!

	local i=0
	local tries=$((100 * FYAI_TIMEOUT_SCALE))
	while [ ! -f "$TEST_DIR/port" ]; do
		kill -0 "$MOCK_PID" 2>/dev/null || fail "mock server died: $(cat "$TEST_DIR/mock.log")"
		i=$((i + 1))
		[ "$i" -gt "$tries" ] && fail "mock server did not bind"
		sleep 0.05
	done
	MOCK_PORT="$(cat "$TEST_DIR/port")"
	MOCK_URL="http://127.0.0.1:$MOCK_PORT"
	export MOCK_URL
}

mock_stop_quiet() {
	if [ -n "$MOCK_PID" ]; then
		kill "$MOCK_PID" 2>/dev/null || true
		MOCK_PID=""
	fi
}

# Stop the mock and assert the whole scenario was consumed.
mock_stop() {
	local expect="${1:-}"

	mock_stop_quiet
	if [ -n "$expect" ]; then
		local served
		served="$(cat "$TEST_DIR/served" 2>/dev/null || echo 0)"
		[ "$served" -eq "$expect" ] || \
			fail "expected $expect requests, served $served"
	fi
}

# run_fyai [args...]: run fyai with the invariant test flags; capture output.
# The exit status lands in FYAI_STATUS (never aborts the case by itself).
run_fyai() {
	set +e
	# markdown is off via the arena config seeded by fyai_test_setup, not a
	# CLI flag: --set is a durable command op that needs an existing arena,
	# which the init/no-storage verbs don't have yet.
	${FYAI_VALGRIND} "$FYAI_BIN" -k test-key --color off "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
}

# run_limited SECONDS command...: run a command, and stop it if it continues
# after the limit. `timeout(1)` is a GNU tool. A platform that does not have
# it must also report a command that does not stop by itself. The function
# returns the status of the command. A command that was stopped reports its
# signal.
run_limited() {
	local limit="$1" pid watch rc
	shift
	"$@" &
	pid=$!
	( sleep "$limit"; kill -TERM "$pid" 2>/dev/null ) &
	watch=$!
	set +e
	wait "$pid"
	rc=$?
	set -e
	kill -TERM "$watch" 2>/dev/null
	wait "$watch" 2>/dev/null
	return "$rc"
}

assert_status() {
	[ "$FYAI_STATUS" -eq "$1" ] || fail "exit status $FYAI_STATUS, expected $1"
}

assert_status_nonzero() {
	[ "$FYAI_STATUS" -ne 0 ] || fail "exit status 0, expected failure"
}

assert_stdout_contains() {
	grep -qF -- "$1" "$TEST_DIR/stdout" || fail "stdout missing: $1"
}

assert_stdout_not_contains() {
	grep -qF -- "$1" "$TEST_DIR/stdout" && fail "stdout unexpectedly has: $1" || true
}

assert_stderr_contains() {
	grep -qF -- "$1" "$TEST_DIR/stderr" || fail "stderr missing: $1"
}

assert_stderr_not_contains() {
	grep -qF -- "$1" "$TEST_DIR/stderr" && fail "stderr unexpectedly has: $1" || true
}

assert_file_content() {
	[ -f "$1" ] || fail "missing file: $1"
	[ "$(cat "$1")" = "$2" ] || fail "bad content in $1: $(cat "$1")"
}

# assert_request <index> <python-expression over r>
# r is the recorded request object {path, auth, content_type, body}.
# The expression must evaluate truthy.
assert_request() {
	local idx="$1" expr="$2"

	"$PYTHON" - "$TEST_DIR/requests.jsonl" "$idx" "$expr" <<'EOF' || fail "request assertion [$idx]: $expr"
import json, sys
path, idx, expr = sys.argv[1], int(sys.argv[2]), sys.argv[3]
reqs = [json.loads(l) for l in open(path)]
if idx >= len(reqs):
    sys.exit("request %d not recorded (%d total)" % (idx, len(reqs)))
r = reqs[idx]
if not eval(expr):
    sys.exit("assertion failed on request %d: %s\n%s" %
             (idx, expr, json.dumps(r, indent=2)))
EOF
}

# Assert that no request matches the Python expression over r.
assert_no_request() {
	local expr="$1"

	"$PYTHON" - "$TEST_DIR/requests.jsonl" "$expr" <<'EOF' || fail "a request matched (expected none): $expr"
import json, sys
path, expr = sys.argv[1], sys.argv[2]
reqs = [json.loads(l) for l in open(path)]
for i, r in enumerate(reqs):
    if eval(expr):
        sys.exit("request %d matched: %s\n%s" % (i, expr, json.dumps(r, indent=2)))
EOF
}

# assert_any_request <python-expression over r>: true for at least one request.
# Use when request order is not deterministic (e.g. concurrent MCP startup).
assert_any_request() {
	local expr="$1"

	"$PYTHON" - "$TEST_DIR/requests.jsonl" "$expr" <<'EOF' || fail "no request matched: $expr"
import json, sys
path, expr = sys.argv[1], sys.argv[2]
reqs = [json.loads(l) for l in open(path)]
if not any(eval(expr) for r in reqs):
    sys.exit("no request matched: %s\n%s" %
             (expr, json.dumps(reqs, indent=2)))
EOF
}

# assert_state <fyai dump/display args...> then grep the fixed string $LAST arg
assert_state_contains() {
	local needle="$1"; shift
	${FYAI_VALGRIND} "$FYAI_BIN" "$@" >"$TEST_DIR/state.out" 2>&1 || fail "fyai $* failed"
	grep -qF -- "$needle" "$TEST_DIR/state.out" || \
		{ cat "$TEST_DIR/state.out" >&2; fail "state missing: $needle"; }
}

assert_state_absent() {
	local needle="$1"; shift
	${FYAI_VALGRIND} "$FYAI_BIN" "$@" >"$TEST_DIR/state.out" 2>&1 || fail "fyai $* failed"
	grep -qF -- "$needle" "$TEST_DIR/state.out" && \
		{ cat "$TEST_DIR/state.out" >&2; fail "state unexpectedly has: $needle"; } || true
}

pass() {
	echo "PASS: $(basename "$0")"
	exit 0
}
