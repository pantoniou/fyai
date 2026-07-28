#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that the time limit of a tool job stops the full process group.
#
# The limit belongs to the job in the parent, and not to the capture inside the
# job child, because only the parent can group-terminate. /bin/sh usually forks
# and does not exec, thus a limit applied in the child would stop the shell and
# leave the process that it started. This drives the real binary and then looks
# for that process.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tool_timeout.json

"$PYTHON" - "$FYAI_BIN" "$MOCK_URL/v1/responses" <<'EOF' || \
	fail "the tool time limit did not stop the process group"
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time

BIN, URL = sys.argv[1], sys.argv[2]
TAG = "fytmo"


def tagged():
    """The process we tagged, by argv. Ours alone - the tag is unique."""
    ps = subprocess.run(["ps", "-eo", "pid,args"], capture_output=True,
                        text=True).stdout
    return [l.strip() for l in ps.split("\n")
            if TAG in l and "ps -eo" not in l]


pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(BIN, [BIN, "-k", "test-key", "--theme", "dark",
                   "--set", "display/markdown=true",
                   "--set", "display/stream=false",
                   "--set", "builtin_shell=false", "--set", "api=responses",
                   "--set", "api_url=" + URL, "-m", "mock-model", "-i"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))

buf = b""


def drain(seconds):
    global buf
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk


def die(msg):
    for t in tagged():                      # never leave them behind
        try:
            os.kill(int(t.split()[0]), 9)
        except (OSError, ValueError):
            pass
    try:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    except OSError:
        pass
    raise SystemExit(msg)


if tagged():
    die("tagged processes existed before the run: %r" % tagged())

# Wait for the initial prompt instead of imposing a fixed startup delay.
deadline = time.monotonic() + 15
while time.monotonic() < deadline and b"\x1b[?25h" not in buf:
    drain(0.2)
if b"\x1b[?25h" not in buf:
    die("the initial prompt never appeared")

buf = b""
os.write(fd, b"run it\n")
deadline = time.monotonic() + 20
while time.monotonic() < deadline and not tagged():
    drain(0.2)
if not tagged():
    die("the tool never started its child: %r" % tagged())

# The limit is 1500 ms and the ladder that follows it is SIGTERM then SIGKILL.
# Allow well past both: this asserts that the group stops, not how fast.
deadline = time.monotonic() + 25
while time.monotonic() < deadline and tagged():
    drain(0.2)

left = tagged()
if left:
    die("the time limit left processes behind: %r" % left)

# The turn must also end, and the model must be told why.
deadline = time.monotonic() + 15
while time.monotonic() < deadline and b"the command was stopped." not in buf:
    drain(0.2)
if b"the command was stopped." not in buf:
    die("the model did not receive the time-limit result")

try:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
except OSError:
    pass
EOF

# The model must be told why the command stopped, and told it by the parent:
# from inside the job child a group terminate is indistinguishable from an
# interrupt, so an unqualified "interrupted" here is the regression.
assert_any_request "'timed out after 1500 ms' in json.dumps(r)"
mock_stop 2
pass
