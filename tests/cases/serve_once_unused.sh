#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that `serve --once` that no session reaches ends on its own: the
# client that started it to hand over a session died first, and a server
# that waited for it would stay forever. The wait is the time a client
# waits to be admitted.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$FYAI_TIMEOUT_SCALE" <<'PY' ||
import os, subprocess, sys, time

binary, cwd, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
log = open(cwd + "/server.err", "wb")
server = subprocess.Popen([binary, "serve", "--once"], cwd=cwd,
                          stdout=subprocess.DEVNULL, stderr=log)
# The admission wait is 10 seconds; the deadline only bounds a failure.
try:
    server.wait(timeout=60 * scale)
except subprocess.TimeoutExpired:
    server.kill()
    server.wait()
    raise SystemExit("the server that no session reached did not end")
if server.returncode != 0:
    raise SystemExit("the server ended with %d" % server.returncode)
PY
	fail "serve --once without a session: $(cat "$TEST_DIR/server.err")"
grep -q "no session came to the server" "$TEST_DIR/server.err" ||
	fail "the server did not say why it ended: $(cat "$TEST_DIR/server.err")"
pass
