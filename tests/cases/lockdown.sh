#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify /session lockdown and /session yolo: each sets the group of isolation keys together,
# a restart follows when the credential transport must start or end, and
# nothing changes when a switch cannot run.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
# The level of this run is set by the environment under the transport gate.
[ -z "${FYAI_TEST_TRANSPORT:-}" ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tool_diff.json

session() {
	set +e
	printf '%s\n' "$@" | "$FYAI_BIN" -b lock -k test-key --color off \
		--set auth=api-key --set api=chat-completions --set display/stream=false --set tools=true \
		--set display/markdown=false --set api_url="$MOCK_URL/v1/chat/completions" \
		-m mock-model -i >"$TEST_DIR/out" 2>"$TEST_DIR/err"
	rc=$?
	set -e
}

stored() {
	"$FYAI_BIN" -b lock config get "$1" 2>/dev/null
}

# Nothing is set to begin with.
[ "$(stored agent/transport_isolation)" != auto ] || fail 'the transport was on at the start'

# /session lockdown stores the keys, and the session restarts to start the transport.
"$FYAI_BIN" branch create lock >/dev/null 2>&1 || fail 'no branch'
session '/session lockdown' '/exit'
[ "$rc" = 0 ] || fail "lockdown ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qF 'lockdown: credential transport auto' "$TEST_DIR/out" || fail "lockdown said nothing: $(cat "$TEST_DIR/out" "$TEST_DIR/err")"
[ "$(stored agent/transport_isolation)" = auto ] || fail 'lockdown did not store the transport level'
[ "$(stored agent/isolation)" = view ] || fail 'lockdown did not store the views of sub-agents'
# The next session follows the stored level: it runs with the transport.
session '/status' '/exit'
[ "$rc" = 0 ] || fail "the isolated session ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qiE 'level-b' "$TEST_DIR/out" || fail "the session did not run isolated: $(cat "$TEST_DIR/out" "$TEST_DIR/err")"

# /session yolo takes everything off. The session keeps its transport, which ends when
# fyai starts again: the next session runs without it.
session '/session yolo' '/exit'
[ "$rc" = 0 ] || fail "yolo ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qF 'yolo: credential transport none' "$TEST_DIR/out" || fail "yolo said nothing: $(cat "$TEST_DIR/out" "$TEST_DIR/err")"
[ "$(stored agent/transport_isolation)" = none ] || fail 'yolo did not store the transport level'
[ "$(stored agent/isolation)" = none ] || fail 'yolo did not store the views of sub-agents'
session '/status' '/exit'
! grep -qiE 'level-b' "$TEST_DIR/out" || fail 'the session kept the transport after yolo'

mock_stop 0
pass
