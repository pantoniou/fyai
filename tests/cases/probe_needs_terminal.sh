#!/bin/bash
# SPDX-License-Identifier: MIT
# A run whose output is not a terminal never queries the terminal. The process
# can still have a controlling terminal, as a run on a build machine does, and
# nothing answers there: a probe would wait for the full time limit.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup_bare

PROBE_CONFIG="$TEST_DIR/probe.yaml"
printf 'arena_dir: %s/.fyai/arena\n' "$TEST_DIR" > "$PROBE_CONFIG"

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" <<'PY' || fail "a run with output to a file queried the terminal"
import os, pty, select, sys, time

BIN, DIR = sys.argv[1], sys.argv[2]

pid, fd = pty.fork()
if pid == 0:
    os.chdir(DIR)
    os.environ["TERM"] = "xterm-256color"
    out = os.open(os.path.join(DIR, "probe.out"), os.O_WRONLY | os.O_CREAT, 0o644)
    os.dup2(out, 1)
    os.dup2(out, 2)
    os.execv(BIN, [BIN, "-k", "test-key", "--color", "on", "--config",
                   os.path.join(DIR, "probe.yaml"), "hello"])

# The pty master is the terminal that nothing answers on. Wait for the run to
# end; a probe would hold it for the time limit of the probe.
asked, end, ended = b"", time.time() + 15, False
while not ended and time.time() < end:
    if select.select([fd], [], [], 0.2)[0]:
        try:
            asked += os.read(fd, 65536)
        except OSError:
            pass
    done, _ = os.waitpid(pid, os.WNOHANG)
    ended = bool(done)
if not ended:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    sys.exit("the run did not end in time: it waited on the terminal")
if asked:
    sys.exit("the run wrote to the terminal: %r" % asked[:200])
PY

pass
