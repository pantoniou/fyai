#!/bin/bash
# SPDX-License-Identifier: MIT
# With isolation on, the process that runs the agent holds no key: not in its
# arguments, not in its environment. The transport holds it, and no other
# process of the user can read the transport.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

KEY_ARG="sk-arg-0123456789abcdef"
KEY_ENV="sk-env-0123456789abcdef"

fyai_test_setup
mock_start transport_delay.json

# The mock holds the reply until the case releases it, so the run stays alive
# for as long as the inspection takes, however loaded the machine is.
FYAI_TRANSPORT_ISOLATION=level-b OPENAI_API_KEY="$KEY_ENV" \
	"$FYAI_BIN" --color off --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	-k "$KEY_ARG" "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null &
pid=$!

# Wait for the request to reach the provider, by state and not by time.
# A bound on a failure, not a delay: a slow runner can stall for minutes.
tries=$((6000 * FYAI_TIMEOUT_SCALE))
i=0
while [ ! -s "$TEST_DIR/requests.jsonl" ]; do
	kill -0 "$pid" 2>/dev/null || fail "fyai ended before it made a request"
	i=$((i + 1))
	[ "$i" -gt "$tries" ] && fail "no request reached the provider"
	sleep 0.05
done

# The agent process: no key in its arguments or environment.
! tr '\0' '\n' <"/proc/$pid/cmdline" | grep -q "$KEY_ARG" ||
	fail "the key is in the arguments of the agent process"
! tr '\0' '\n' <"/proc/$pid/environ" | grep -q "$KEY_ENV" ||
	fail "the key is in the environment of the agent process"
! tr '\0' '\n' <"/proc/$pid/environ" | grep -q "^OPENAI_API_KEY=" ||
	fail "OPENAI_API_KEY is in the environment of the agent process"
grep -q "^FYAI_TRANSPORT_FD=" "/proc/$pid/environ" 2>/dev/null ||
	tr '\0' '\n' <"/proc/$pid/environ" | grep -q "^FYAI_TRANSPORT_FD=" ||
	fail "the agent process has no transport channel"

# The transport is its child, and it holds the key where no one can read it.
tpid=""
for c in /proc/[0-9]*; do
	p="${c#/proc/}"
	ppid="$(awk '/^PPid:/ {print $2}' "$c/status" 2>/dev/null || true)"
	[ "$ppid" = "$pid" ] && tpid="$p" && break
done
[ -n "$tpid" ] || fail "the agent process has no transport child"
tr '\0' '\n' <"/proc/$tpid/cmdline" | grep -q '^transport$' ||
	fail "the child of the agent process is not the transport"
! tr '\0' '\n' <"/proc/$tpid/cmdline" | grep -q "$KEY_ARG" ||
	fail "the key is in the arguments of the transport"
if cat "/proc/$tpid/environ" >/dev/null 2>&1; then
	fail "the environment of the transport is readable"
fi

# Release the reply: the provider got the key, and the run finishes normally.
: >"$TEST_DIR/release"
wait "$pid" || fail "fyai failed: $(cat "$TEST_DIR/stderr")"
grep -q "slow reply" "$TEST_DIR/stdout" || fail "no reply"
assert_request 0 'r["auth"] == "Bearer '"$KEY_ARG"'"'
# The transport ends with the run.
i=0
while [ -d "/proc/$tpid" ] && grep -qv "^State:.*Z" "/proc/$tpid/status" 2>/dev/null; do
	i=$((i + 1))
	[ "$i" -gt "$tries" ] && fail "the transport outlived the run"
	sleep 0.05
done
mock_stop 1
