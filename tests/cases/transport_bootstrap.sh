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

# The mock tells its FIFO about each request, when a reader holds it. Open it
# before the run starts, so no line is lost.
exec 8<>"$FYAI_MOCK_NOTIFY"

# The mock holds the reply until the case releases it, so the run stays alive
# for as long as the inspection takes, however loaded the machine is.
FYAI_TRANSPORT_ISOLATION=level-b OPENAI_API_KEY="$KEY_ENV" \
	"$FYAI_BIN" --color off --set api=chat-completions --set display/stream=false \
	--set api_url="$MOCK_URL/v1/chat/completions" -m mock-model \
	-k "$KEY_ARG" "hello" >"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr" </dev/null 8<&- &
pid=$!

# Wait for the request to reach the provider: the read wakes when the mock
# writes. The deadline bounds a failure: a slow runner can stall for minutes.
line=""
while [ "$line" != request ]; do
	read -r -t $((300 * FYAI_TIMEOUT_SCALE)) -u 8 line ||
		fail "no request reached the provider"
done
exec 8<&-

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
# The transport ends with the run. A pidfd is readable when the process has
# exited, a zombie included, so the wait is for the exit and not for a poll.
python3 -I -c '
import os, select, sys
try:
    fd = os.pidfd_open(int(sys.argv[1]))
except ProcessLookupError:
    sys.exit(0)
sys.exit(0 if select.select([fd], [], [], float(sys.argv[2]))[0] else 1)
' "$tpid" $((300 * FYAI_TIMEOUT_SCALE)) || fail "the transport outlived the run"
mock_stop 1
