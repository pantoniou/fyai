#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" "$TESTS_DIR" <<'PY'
import fcntl, os, pathlib, re, select, signal, struct, subprocess, sys, termios, time
import urllib.parse

binary, scratch, tests = sys.argv[1:]
sys.path.insert(0, tests)
from pty_driver import Terminal

master, slave = os.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 1000, 0, 0))
env = dict(os.environ, TERM="xterm-256color")

def control_terminal():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
process = subprocess.Popen([binary, "-k", "test-key", "--color", "off", "--set",
                           "api=chat-completions", "--set", "display/screen=fullscreen",
                           "--set", "display/renderer=page", "-i"],
                          stdin=slave, stdout=slave, stderr=slave, env=env,
                          preexec_fn=control_terminal)
os.close(slave)
terminal = Terminal(40, 1000)
output = b""
deadline = time.monotonic() + 20 * float(env.get("FYAI_TIMEOUT_SCALE", "1"))

def until(predicate):
    global output
    while not predicate():
        assert time.monotonic() < deadline, screen()
        assert process.poll() is None, output[-4000:]
        if select.select([master], [], [], 0.1)[0]:
            output += terminal.read(master)

def screen():
    return "\n".join("".join(row) for row in terminal.screen.grid)

try:
    until(lambda: any(row.lstrip().startswith("❯")
                      for row in terminal.screen.display()))
    os.write(master, b"/auth login --manual\n")
    pattern = r"https://auth.openai.com/api/accounts/authorize\?[^\s┃]+"
    def login_query():
        text = "".join(row.strip(" ┃") for row in terminal.screen.display())
        match = re.search(pattern, text.split("Paste the complete", 1)[0])
        if not match or "Paste the complete" not in screen():
            return None
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(match.group()).query)
        return query if {"redirect_uri", "state", "code_challenge"} <= query.keys() else None

    until(lambda: login_query() is not None)
    query = login_query()
    # A rejected token must not enter stored conversation data.
    marker = "secret-do-not-store"
    os.write(master, (marker + "\n").encode())
    until(lambda: "not a bare token" in screen())

    callback = query["redirect_uri"][0] + "?" + urllib.parse.urlencode({
        "state": query["state"][0], "error": "access_denied", "code": marker})
    os.write(master, (callback + "\n").encode())
    until(lambda: "permission declined" in screen())
    os.write(master, b"/auth status\n")
    until(lambda: "Signed out" in screen())
    os.write(master, b"\x1b\x00")
    until(lambda: "Signed out" not in screen())
    os.write(master, b"/exit\n")
    while process.poll() is None:
        assert time.monotonic() < deadline, screen()
        if select.select([master], [], [], 0.1)[0]:
            try:
                output += terminal.read(master)
            except OSError:
                break
    assert process.wait(timeout=3) == 0
    state = pathlib.Path(env["XDG_STATE_HOME"]) / "fyai"
    for file in state.rglob("*"):
        if file.is_file():
            assert marker.encode() not in file.read_bytes(), file
finally:
    pathlib.Path(scratch, "pty.out").write_bytes(output)
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=3)
    os.close(master)
PY
pass
