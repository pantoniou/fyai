#!/bin/bash
# SPDX-License-Identifier: MIT
# mock-play.sh - drive an interactive fyai against the mock provider by hand.
#
#   tests/mock-play.sh [-b FYAI] [-s SCENARIO] [-k] [-- FYAI-ARGS...]
#
# The session runs in a private sandbox (HOME, XDG_*, project) made by the
# test harness, with the shipped display defaults. The default scenario is
# tests/play/menu.yaml: type `help` for its words. -k keeps the sandbox, its
# requests.jsonl and mock.log after the session ends.
set -eu

here="$(cd "$(dirname "$0")" && pwd)"
fyai="$here/../build/fyai"
scenario="$here/play/menu.yaml"
keep=

while getopts "b:s:kh" opt; do
	case "$opt" in
	b) fyai="$OPTARG" ;;
	s) scenario="$OPTARG" ;;
	k) keep=1 ;;
	*)
		sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//'
		exit 2
		;;
	esac
done
shift $((OPTIND - 1))
[ "${1:-}" != "--" ] || shift

[ -x "$fyai" ] || { echo "mock-play: no fyai at $fyai (-b)" >&2; exit 2; }
fyai="$(cd "$(dirname "$fyai")" && pwd)/$(basename "$fyai")"
[ -f "$scenario" ] || { echo "mock-play: no scenario $scenario" >&2; exit 2; }
scenario="$(cd "$(dirname "$scenario")" && pwd)/$(basename "$scenario")"

FYAI_BIN="$fyai"
FYAI_MOCK_LOG=1
. "$here/harness.sh"

fyai_test_setup_bare
# A kept sandbox stays where the cleanup would remove it.
[ -z "$keep" ] || fyai_test_on_exit \
	"cp -r '$TEST_DIR' '${TMPDIR%/tmp}-kept' && echo 'mock-play: kept ${TMPDIR%/tmp}-kept' >&2"
mock_start "$scenario"
# The arena names the mock, so that a sub-agent reaches it as the session does.
export FYAI_PLAY_KEY=test-key
# The files of the mock stand in the project: the diffs of the calls leave
# them out.
printf '%s\n' "model: mock-model" "api: chat-completions" \
	"api_url: $MOCK_URL/v1/chat/completions" \
	"api_key: { type: env, value: FYAI_PLAY_KEY }" "tools: true" \
	"view:" "  ignore: [port, served, mock.log, requests.jsonl, tmp]" \
	>config.yaml
"$FYAI_BIN" init >/dev/null 2>&1 || fail "fyai init"
rm -f config.yaml

# fyai runs as a child that the script waits for: a trap interrupts a wait,
# not a command in the foreground, so a signal that ends the script reaches
# the session, and the mock goes with them. Ctrl-C is the session's.
set +e
trap ':' INT
trap 'kill -TERM "$play_pid" 2>/dev/null' TERM HUP QUIT
"$FYAI_BIN" -i "$@" <&0 &
play_pid=$!
while :; do
	wait "$play_pid"
	status=$?
	kill -0 "$play_pid" 2>/dev/null || break
done
set -e
mock_stop_quiet
exit "$status"
