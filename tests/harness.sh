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

[ -x "$FYAI_BIN" ] || { echo "harness: FYAI_BIN not set/executable" >&2; exit 99; }

MOCK_PID=""
TEST_DIR=""

fail() {
	echo "FAIL: $*" >&2
	[ -f "$TEST_DIR/stdout" ] && { echo "--- stdout ---" >&2; cat "$TEST_DIR/stdout" >&2; }
	[ -f "$TEST_DIR/stderr" ] && { echo "--- stderr ---" >&2; cat "$TEST_DIR/stderr" >&2; }
	[ -f "$TEST_DIR/requests.jsonl" ] && { echo "--- requests ---" >&2; cat "$TEST_DIR/requests.jsonl" >&2; }
	exit 1
}

fyai_test_cleanup() {
	mock_stop_quiet
	[ -n "$TEST_DIR" ] && rm -rf "$TEST_DIR"
}

# Create the sandbox: scratch dir, redirected HOME/XDG_*, scrubbed provider
# env, and a local .fyai so the arena walk-up lands here.
fyai_test_setup() {
	TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/fyai-test-XXXXXX")" || exit 99
	trap fyai_test_cleanup EXIT
	cd "$TEST_DIR" || exit 99

	export HOME="$TEST_DIR/home"
	export XDG_STATE_HOME="$TEST_DIR/home/.local/state"
	export XDG_CONFIG_HOME="$TEST_DIR/home/.config"
	mkdir -p "$HOME"
	unset OPENAI_API_KEY OPENROUTER_API_KEY DEEPSEEK_API_KEY ANTHROPIC_API_KEY || true

	printf 'display:\n  markdown: false\n' > config.yaml
	"$FYAI_BIN" init >/dev/null 2>&1 || fail "fyai init"
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
	while [ ! -f "$TEST_DIR/port" ]; do
		kill -0 "$MOCK_PID" 2>/dev/null || fail "mock server died: $(cat "$TEST_DIR/mock.log")"
		i=$((i + 1))
		[ "$i" -gt 100 ] && fail "mock server did not bind"
		sleep 0.05
	done
	MOCK_PORT="$(cat "$TEST_DIR/port")"
	MOCK_URL="http://127.0.0.1:$MOCK_PORT"
	export MOCK_URL
}

mock_stop_quiet() {
	[ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null
	MOCK_PID=""
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
	"$FYAI_BIN" -k test-key --color off --no-markdown "$@" \
		>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null
	FYAI_STATUS=$?
	set -e
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

# assert_state <fyai dump/display args...> then grep the fixed string $LAST arg
assert_state_contains() {
	local needle="$1"; shift
	"$FYAI_BIN" "$@" >"$TEST_DIR/state.out" 2>&1 || fail "fyai $* failed"
	grep -qF -- "$needle" "$TEST_DIR/state.out" || \
		{ cat "$TEST_DIR/state.out" >&2; fail "state missing: $needle"; }
}

assert_state_absent() {
	local needle="$1"; shift
	"$FYAI_BIN" "$@" >"$TEST_DIR/state.out" 2>&1 || fail "fyai $* failed"
	grep -qF -- "$needle" "$TEST_DIR/state.out" && \
		{ cat "$TEST_DIR/state.out" >&2; fail "state unexpectedly has: $needle"; } || true
}

pass() {
	echo "PASS: $(basename "$0")"
	exit 0
}
