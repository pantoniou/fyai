#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Soak of the terminal client of `--remote`: a PTY script types N questions
# into one session of `fyai serve`, and this script samples the resident set
# of the client, the server and the worker while it runs. Memory that keeps
# growing with the turns is a leak or an unbounded cache.
#
#   tests/bench/remote_client_soak.sh ./build/fyai [turns] [reply_kb]
set -eu
FYAI_BIN="${1:?usage: remote_client_soak.sh <fyai-binary> [turns] [reply_kb]}"
export FYAI_BIN
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/remote-harness.sh"

TURNS="${2:-100}"
REPLY_KB="${3:-1}"
SAMPLE_EVERY="${SOAK_EVERY:-$((TURNS / 10))}"
[ "$SAMPLE_EVERY" -ge 1 ] || SAMPLE_EVERY=1

fyai_test_setup
"$PYTHON" - "$TEST_DIR/soak.json" "$TURNS" "$REPLY_KB" <<'PY'
import json, sys
path, n, kb = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
pad = ("lorem ipsum dolor sit amet " * 64)
pad = (pad * (kb * 1024 // len(pad) + 1))[:kb * 1024]
steps = []
for i in range(n):
    steps.append({"response": {
        "id": "chatcmpl-s%d" % i, "object": "chat.completion",
        "created": 1735689600 + i, "model": "mock-model",
        "choices": [{"index": 0, "message": {"role": "assistant",
                     "content": "answer %d: %s end %d." % (i + 1, pad, i + 1)},
                     "finish_reason": "stop"}],
        "usage": {"prompt_tokens": 10 * i, "completion_tokens": 50,
                  "total_tokens": 10 * i + 50}}})
json.dump({"steps": steps}, open(path, "w"))
PY
mock_start "$TEST_DIR/soak.json"
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
	--set display/screen=fullscreen --set display/markdown=true \
	--set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
"$FYAI_BIN" branch create soak/churn >/dev/null 2>&1
if [ -n "${SOAK_WRAP_SERVER:-}" ]; then
	# shellcheck disable=SC2086
	$SOAK_WRAP_SERVER "$FYAI_BIN" serve >"$TEST_DIR/server.out" 2>"$TEST_DIR/server.err" &
	SERVER_PID=$!
	trap remote_test_cleanup EXIT
	while ! [ -S "$TEST_DIR/.fyai/unix/remote.sock" ]; do
		kill -0 "$SERVER_PID" || exit 1
		sleep 0.2
	done
	sleep 2
else
	remote_serve_start
fi

{
	printf 'rows: 40\ncols: 100\ntimeout: 1200\nsteps:\n  - wait-screen: "❯"\n'
	for i in $(seq 1 "$TURNS"); do
		# SOAK_CHURN=1 moves the head back before each question, so the
		# conversation keeps its length and the replica drops turns.
		if [ "${SOAK_CHURN:-0}" = 1 ] && [ "$i" -gt 2 ]; then
			printf '  - type: /reset soak/churn~2\n  - wait-gone: "end %d."\n' "$((i - 1))"
		fi
		printf '  - type: question %d\n  - wait-screen: "end %d."\n' "$i" "$i"
	done
	printf '  - type: /quit\n'
} >"$TEST_DIR/soak.yaml"

kmem() {	# kmem PID: anonymous and file resident kilobytes
	awk '/^RssAnon:/ {a=$2} /^RssFile:/ {f=$2} END {printf "%d %d", a, f}' \
		"/proc/$1/status" 2>/dev/null || printf '0 0'
}
# idle PID...: wait until none of the processes used CPU time in a poll, so a
# sample does not catch the buffers of a turn that is still running.
cpu_ticks() {
	local p t=0 v
	for p in "$@"; do
		v="$(awk '{print $14 + $15}' "/proc/$p/stat" 2>/dev/null || echo 0)"
		t=$((t + v))
	done
	echo "$t"
}
idle() {
	local before after n=0
	before="$(cpu_ticks "$@")"
	while [ "$n" -lt 100 ]; do
		sleep 0.05
		after="$(cpu_ticks "$@")"
		[ "$after" = "$before" ] && return 0
		before="$after"
		n=$((n + 1))
	done
}
kids() {	# kids PID: all descendants
	local p
	for p in $(cat "/proc/$1/task/$1/children" 2>/dev/null); do
		echo "$p"
		kids "$p"
	done
}

pty_run "$TEST_DIR/soak.out" "$(command -v env)" -u MOCK_KEY \
	${SOAK_WRAP_CLIENT:-} "$FYAI_BIN" --remote . --branch soak/churn <"$TEST_DIR/soak.yaml" &
DRIVER=$!

echo "turn | client anon file | server anon file | worker anon file | cpu ms/turn client server worker"
last_c=0; last_s=0; last_w=0; last_n=0
TICK_MS=$((1000 / $(getconf CLK_TCK)))
next=$SAMPLE_EVERY
while kill -0 "$DRIVER" 2>/dev/null; do
	done_turns=$( { wc -l <"$TEST_DIR/requests.jsonl"; } 2>/dev/null || echo 0)
	if [ "$done_turns" -ge "$next" ]; then
		client=""
		worker=""
		for p in $(kids "$DRIVER"); do
			cmd="$(tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null || true)"
			case "$cmd" in
			*"--remote . "*) client="$p" ;;
			esac
		done
		for p in $(kids "$SERVER_PID"); do
			worker="$p"
		done
		if [ -n "${SOAK_SMAPS:-}" ]; then
			[ -z "$client" ] || cp "/proc/$client/smaps" "$SOAK_SMAPS/client.$done_turns" 2>/dev/null || true
			cp "/proc/$SERVER_PID/smaps" "$SOAK_SMAPS/server.$done_turns" 2>/dev/null || true
			[ -z "$worker" ] || cp "/proc/$worker/smaps" "$SOAK_SMAPS/worker.$done_turns" 2>/dev/null || true
		fi
		# shellcheck disable=SC2086
		idle $client $worker
		nc=$([ -n "$client" ] && cpu_ticks "$client" || echo 0)
		ns=$(cpu_ticks "$SERVER_PID")
		nw=$([ -n "$worker" ] && cpu_ticks "$worker" || echo 0)
		dn=$((done_turns - last_n)); [ "$dn" -ge 1 ] || dn=1
		cpus="$(( (nc - last_c) * TICK_MS / dn )) $(( (ns - last_s) * TICK_MS / dn )) $(( (nw - last_w) * TICK_MS / dn ))"
		last_c=$nc; last_s=$ns; last_w=$nw; last_n=$done_turns
		printf '%4d | %s | %s | %s | %s\n' "$done_turns" \
			"$( [ -n "$client" ] && kmem "$client" || echo '0 0')" \
			"$(kmem "$SERVER_PID")" \
			"$( [ -n "$worker" ] && kmem "$worker" || echo '0 0')" "$cpus"
		next=$((done_turns + SAMPLE_EVERY))
	fi
done
wait "$DRIVER" || echo "driver status $?"
remote_test_stop
mock_stop_quiet 2>/dev/null || true
