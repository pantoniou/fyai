#!/bin/bash
# SPDX-License-Identifier: MIT
# display/keys moves a key of the prompt: Ctrl-D no longer ends an empty
# prompt when it is unbound, and F10 does when it is bound to fytim.quit. A
# key bound to an unknown action is refused with a warning, and the defaults
# stand.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

"$PYTHON" - "$FYAI_BIN" <<'PY' || fail "display/keys did not move the keys"
import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time
from term_reply import answer_da1

BIN = sys.argv[1]


def spawn(keys):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.execv(BIN, [BIN, "-k", "test-key", "-i", "--set",
                       "display/keys=" + keys])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    return pid, fd


class Run:
    def __init__(self, keys):
        self.pid, self.fd = spawn(keys)
        self.out = b""
        self.status = None

    def pump(self, done, seconds):
        """Read the terminal until @done(self) holds or the child ends."""
        end = time.monotonic() + seconds
        while time.monotonic() < end and not done(self):
            if select.select([self.fd], [], [], 0.1)[0]:
                try:
                    chunk = os.read(self.fd, 65536)
                except OSError:
                    chunk = b""
                if chunk:
                    answer_da1(self.fd, chunk)
                    self.out += chunk
            if self.status is None:
                w, st = os.waitpid(self.pid, os.WNOHANG)
                if w:
                    self.status = st
        return done(self)

    def prompt(self):
        return self.pump(lambda r: b"\x1b[?25h" in r.out, 15)

    def ended(self, seconds):
        return self.pump(lambda r: r.status is not None, seconds)

    def kill(self):
        try:
            os.kill(self.pid, 9)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


# Ctrl-D is unbound and F10 ends the prompt. One write keeps the order of the
# two keys, so reaching the end means that Ctrl-D was ignored.
r = Run('{prompt: {Ctrl-d: "", F10: fytim.quit}}')
try:
    if not r.prompt():
        raise SystemExit("fyai never reached an interactive prompt")
    os.write(r.fd, b"\x04\x1b[21~")
    if not r.ended(5):
        raise SystemExit("F10 bound to fytim.quit did not end the session")
    if not os.WIFEXITED(r.status) or os.WEXITSTATUS(r.status) != 0:
        raise SystemExit("the session did not end cleanly: %r" % (r.status,))
finally:
    r.kill()

# Ctrl-D alone changes nothing when it is unbound.
r = Run('{prompt: {Ctrl-d: ""}}')
try:
    if not r.prompt():
        raise SystemExit("fyai never reached an interactive prompt")
    os.write(r.fd, b"\x04")
    if r.ended(1):
        raise SystemExit("an unbound Ctrl-D ended the session")
finally:
    r.kill()

# An action the library does not know: the warning names the setting, and
# Ctrl-D still ends the prompt.
r = Run('{prompt: {F10: no.such.action}}')
try:
    if not r.prompt():
        raise SystemExit("fyai never reached an interactive prompt")
    os.write(r.fd, b"\x04")
    if not r.ended(5):
        raise SystemExit("Ctrl-D did not end the session with the defaults")
    if b"display/keys" not in r.out:
        raise SystemExit("no warning for an unknown action")
finally:
    r.kill()
PY

pass
