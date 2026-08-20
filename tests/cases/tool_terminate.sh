#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify SIGTERM and SIGKILL termination of the complete tool process group.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tool_terminate.json

"$PYTHON" - "$FYAI_BIN" "$MOCK_URL/v1/responses" <<'EOF' || fail "cancelled tool did not terminate cleanly"
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
TAGS = ("fyai-term-stubborn", "fyai-term-polite")


def descendants(root):
    """Every process below @root, by walking the parent map once."""
    ps = subprocess.run(["ps", "-eo", "pid,ppid,args"], capture_output=True,
                        text=True).stdout
    rows = []
    for line in ps.split("\n")[1:]:
        parts = line.strip().split(None, 2)
        if len(parts) == 3:
            rows.append((int(parts[0]), int(parts[1]), parts[2]))
    children = {}
    for cpid, ppid, args in rows:
        children.setdefault(ppid, []).append((cpid, args))
    out, stack = [], [root]
    while stack:
        for cpid, args in children.get(stack.pop(), []):
            out.append((cpid, args))
            stack.append(cpid)
    return out


def tagged():
    """Our tagged processes, below this run's fyai alone.

    The tags are unique across tools but not across concurrent runs of this
    case, so the search is scoped to the process tree this test started.
    """
    return ["%d %s" % (cpid, args) for cpid, args in descendants(pid)
            if any(t in args for t in TAGS) and "ps -eo" not in args]


pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(BIN, [BIN, "-k", "test-key", "--theme", "dark",
                   "--set", "display/markdown=true",
                   "--set", "display/stream=false",
                   "--set", "builtin_shell=true", "--set", "api=responses",
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


drain(3.0)
if tagged():
    die("tagged processes existed before the run: %r" % tagged())

os.write(fd, b"run it\n")
deadline = time.monotonic() + 20
while time.monotonic() < deadline and len(tagged()) < 2:
    drain(0.2)
if len(tagged()) < 2:
    die("the tool never started its children: %r" % tagged())

buf = b""
os.write(fd, b"\x03")

# The ladder is SIGTERM then, after a grace period, SIGKILL. Allow well past
# it: this asserts termination happens, not how fast.
deadline = time.monotonic() + 20
while time.monotonic() < deadline and tagged():
    drain(0.2)

left = tagged()
if left:
    die("cancelled tool left processes behind: %r" % left)

# And the turn must actually end - a cancel that kills the tool but leaves the
# turn running is the same hang from the user's side.
deadline = time.monotonic() + 15
while time.monotonic() < deadline and "❯".encode() not in buf:
    drain(0.2)
if "❯".encode() not in buf:
    die("the prompt never came back after cancelling the tool")

try:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
except OSError:
    pass
EOF

mock_stop 1
pass
