#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify /session lockdown and /session yolo: each sets the group of isolation keys
# together, the session restarts when the credential transport or the view of the
# session must start, and /session shows the state.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
# The level of this run is set by the environment under the transport gate.
[ -z "${FYAI_TEST_TRANSPORT:-}" ] || exit 77
. "$(dirname "$0")/../harness.sh"

FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
# The scratch directory of a view must lie outside the project.
export TMPDIR="$FYAI_TMPDIR_BASE"
VIEW=no
unshare -Urnm true 2>/dev/null && VIEW=yes
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

[ "$(stored agent/transport_isolation)" != auto ] || fail 'the transport was on at the start'
"$FYAI_BIN" branch create lock >/dev/null 2>&1 || fail 'no branch'
# The user sets the views of sub-agents by hand.
"$FYAI_BIN" -b lock config set agent/isolation none >/dev/null 2>&1 || fail 'cannot set agent/isolation'

# The state before anything is set.
session '/session' '/exit'
[ "$rc" = 0 ] || fail "/session ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qF 'credential transport: off' "$TEST_DIR/out" || fail "/session does not say the transport is off: $(cat "$TEST_DIR/out")"
grep -qF 'session view: none' "$TEST_DIR/out" || fail "/session names a view: $(cat "$TEST_DIR/out")"

# lockdown stores the keys, and the session restarts to start the transport and the view.
session '/session lockdown' '/exit'
[ "$rc" = 0 ] || fail "lockdown ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qF 'lockdown: credential transport auto' "$TEST_DIR/out" || fail "lockdown said nothing: $(cat "$TEST_DIR/out" "$TEST_DIR/err")"
[ "$(stored agent/transport_isolation)" = auto ] || fail 'lockdown did not store the transport level'
# The views of sub-agents are the user's setting, and lockdown leaves them alone.
[ "$(stored agent/isolation)" = none ] || fail 'lockdown changed the views of sub-agents'
if [ "$VIEW" = yes ]; then
	[ "$(stored view/isolate_session)" = true ] || fail 'lockdown did not store the view of the session'
fi

# The next session follows the stored keys: the transport, and the view of the session.
session '/session' '/status' '/exit'
[ "$rc" = 0 ] || fail "the isolated session ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qE 'credential transport: level-b' "$TEST_DIR/out" || fail "the session did not run with the transport: $(cat "$TEST_DIR/out" "$TEST_DIR/err")"
if [ "$VIEW" = yes ]; then
	grep -qF 'session view: session' "$TEST_DIR/out" || fail "the session did not run in its view: $(cat "$TEST_DIR/out")"
	grep -qE '^ *View +│ +session' "$TEST_DIR/out" || fail "/status does not name the view: $(cat "$TEST_DIR/out")"
fi

# yolo takes everything off. The session keeps its transport and its view, which end
# when fyai starts again: the next session runs without them.
session '/session yolo' '/exit'
[ "$rc" = 0 ] || fail "yolo ended with status $rc: $(cat "$TEST_DIR/err")"
grep -qF 'yolo: credential transport none' "$TEST_DIR/out" || fail "yolo said nothing: $(cat "$TEST_DIR/out" "$TEST_DIR/err")"
[ "$(stored agent/transport_isolation)" = none ] || fail 'yolo did not store the transport level'
[ "$(stored agent/isolation)" = none ] || fail 'yolo changed the views of sub-agents'
[ "$(stored view/isolate_session)" != true ] || fail 'yolo kept the view of the session'
session '/session' '/exit'
grep -qF 'credential transport: off' "$TEST_DIR/out" || fail "the session kept the transport after yolo: $(cat "$TEST_DIR/out")"
grep -qF 'session view: none' "$TEST_DIR/out" || fail "the session kept its view after yolo: $(cat "$TEST_DIR/out")"

mock_stop 0
pass
