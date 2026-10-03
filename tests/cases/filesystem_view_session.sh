#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"
FYAI_TMPDIR_BASE="$(dirname "$FYAI_BIN")"
fyai_test_setup
unshare -Urnm true 2>/dev/null || exit 77
mkdir project
printf baseline > project/file
run_fyai view create demo "$TEST_DIR/project"
assert_status 0
run_fyai branch list --output json
assert_status 0
BRANCH=$("$PYTHON" - "$TEST_DIR/stdout" <<'PYBRANCH'
import json, sys
for row in json.load(open(sys.argv[1])):
    if row.get('current'):
        print(row['branch'])
        break
PYBRANCH
)
[ -n "$BRANCH" ] || fail 'missing active branch'
FYAI_PTY_INPUT="/view enter demo sh -c 'test -t 0 && printf changed > file && printf \"%s%s\\n\" VIEW -READY'" \
FYAI_PTY_NEEDLE="VIEW-READY" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-gone:Ctrl-] returns to the prompt" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" --branch "$BRANCH" -k test-key -m mock-model -i
[ "$(cat project/file)" = baseline ] || fail 'view changed the host'
run_fyai view diff demo --stat
assert_status 0
assert_stdout_contains 'file'
FYAI_PTY_INPUT="/view enter demo" \
FYAI_PTY_NEEDLE="Ctrl-] returns to the prompt" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="raw:1d|wait-gone:Ctrl-] returns to the prompt|send:/view list|wait-screen:Filesystem views|send:/view list --full|wait-screen:storage: owned|send:!printf '%s%s\\n' BANG -READY|wait-screen:BANG-READY|raw:1d|wait-gone:Ctrl-] returns to the prompt|send:/zoom view-1|wait-screen:Ctrl-] returns to the prompt|send:printf '%s%s\\n' SHELL -READY; exit|wait-screen:SHELL-READY|wait-gone:Ctrl-] returns to the prompt" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/shell.out" \
    "$FYAI_BIN" --branch "$BRANCH" -k test-key -m mock-model -i
if grep -q "view command failed" "$TEST_DIR/shell.out"; then
    fail 'a tool child notified the parent view exit callback'
fi
pass
