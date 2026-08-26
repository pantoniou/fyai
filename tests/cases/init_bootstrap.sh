#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify what fyai does when it is started with no project.
#
# The arena directory is chosen from the working directory, so a run started
# in the wrong place must not leave a .fyai behind that carries no
# configuration. Without a terminal there is nobody to ask: the run stops and
# names `fyai init`. On a terminal the user is asked, and an answer of yes
# creates the project with a working configuration.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup_bare

# Non-interactive: refuse, and leave nothing behind.
run_fyai "hello"
assert_status_nonzero
assert_stderr_contains "run \`fyai init\`"
if [ -e "$TEST_DIR/.fyai" ]; then fail "a refused run created $TEST_DIR/.fyai"; fi

# The same for a verb that needs storage.
run_fyai dump state
assert_status_nonzero
assert_stderr_contains "run \`fyai init\`"
if [ -e "$TEST_DIR/.fyai" ]; then fail "a refused verb created $TEST_DIR/.fyai"; fi

# On a terminal, an answer of no also leaves nothing behind.
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" n <<'PY' || fail "the prompt did not appear"
import os, pty, select, sys, time

BIN, DIR, ANSWER = sys.argv[1], sys.argv[2], sys.argv[3]

pid, fd = pty.fork()
if pid == 0:
    os.chdir(DIR)
    os.environ["TERM"] = "xterm-256color"
    os.execv(BIN, [BIN, "-k", "test-key", "--color", "off", "-i"])

buf, sent, end = b"", False, time.time() + 20
while time.time() < end:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if not sent and b"[y/N]" in buf:
            os.write(fd, ANSWER.encode() + b"\n")
            sent = True
            end = time.time() + 10
# Closing a pty master can report EIO on macOS. The read is over: the
# descriptor goes all the same.
try:
    os.close(fd)
except OSError:
    pass
os.waitpid(pid, 0)
sys.exit(0 if b"[y/N]" in buf else 1)
PY
if [ -e "$TEST_DIR/.fyai" ]; then fail "an answer of no created $TEST_DIR/.fyai"; fi

# An answer of yes creates the project, and its config reads back.
"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" y <<'PY' || fail "the prompt did not appear"
import os, pty, select, sys, time

BIN, DIR, ANSWER = sys.argv[1], sys.argv[2], sys.argv[3]

pid, fd = pty.fork()
if pid == 0:
    os.chdir(DIR)
    os.environ["TERM"] = "xterm-256color"
    os.execv(BIN, [BIN, "-k", "test-key", "--color", "off", "-i"])

buf, sent, end = b"", False, time.time() + 20
while time.time() < end:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        if not sent and b"[y/N]" in buf:
            os.write(fd, ANSWER.encode() + b"\n")
            sent = True
            end = time.time() + 10
        elif sent and b"initialized" in buf:
            # End the reader. The program may have left already, and a pty
            # that has no reader reports EIO rather than a short write.
            try:
                os.write(fd, b"\x04")
            except OSError:
                break
# Closing a pty master can report EIO on macOS. The read is over: the
# descriptor goes all the same.
try:
    os.close(fd)
except OSError:
    pass
os.waitpid(pid, 0)
sys.exit(0 if b"initialized" in buf else 1)
PY
[ -d "$TEST_DIR/.fyai/arena" ] || fail "an answer of yes created no arena"

run_fyai config get model
assert_status 0
[ -s "$TEST_DIR/stdout" ] || fail "the new project has no model configured"

exit 0
