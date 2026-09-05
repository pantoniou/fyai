#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify fresh-frame waits, screen waits, frame counts, and child status
# after PTY EOF.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup_bare

"$PYTHON" - "$TESTS_DIR" "$TEST_DIR" <<'PY' || fail "PTY driver regression"
import os
import subprocess
import sys

tests, scratch = sys.argv[1:]
child = r'''
import os, sys, time, tty
tty.setraw(0)
os.write(1, b'\x1b[?25h')
line = b''
while not line.endswith(b'\n'):
    line += os.read(0, 1)
os.write(1, b'Done.\x1b[?2026l')
line = b''
while not line.endswith(b'\n'):
    ch = os.read(0, 1)
    if ch == b'x':
        if sys.argv[1] == 'held':
            while not os.path.exists(sys.argv[3]):
                time.sleep(0.01)
        if sys.argv[1] in ('fresh', 'held'):
            os.write(1, b'Done.\x1b[?2026l')
        elif sys.argv[1] == 'partial':
            os.write(1, b'Done.')
        elif sys.argv[1] == 'gone':
            os.write(1, b'\x1b[2J\x1b[H\x1b[?2026l')
        elif sys.argv[1] == 'stays':
            os.write(1, b'\x1b[?2026l')
        elif sys.argv[1] == 'frames':
            os.write(1, b'.\x1b[?2026l.\x1b[?2026l')
        continue
    line += ch
for fd in (0, 1, 2):
    os.close(fd)
time.sleep(0.1)
os._exit(int(sys.argv[2]))
'''

release = os.path.join(scratch, 'release')
for mode, status, after, error in (
    ('fresh', 0, 'raw:78|wait-frame:Done.', None),
    ('held', 0, 'raw:78|release:' + release + '|wait-frame:Done.', None),
    ('stale', 0, 'raw:78|wait-frame:Done.', 'never contained'),
    ('partial', 0, 'raw:78|wait-frame:Done.', 'never contained'),
    # What is still on the screen, and how many frames were painted: a
    # step that waits for either must read the screen and the frames, not
    # the bytes that once carried them.
    ('gone', 0, 'raw:78|wait-gone:Done.', None),
    ('stays', 0, 'raw:78|wait-gone:Done.', 'never left the screen'),
    ('frames', 0, 'raw:78|frame:2', None),
    ('partial', 0, 'raw:78|frame:1', 'painted 0 of 1 frames'),
    ('fresh', 7, '', 'exited with status 7'),
):
    env = dict(os.environ, FYAI_PTY_INPUT='hello', FYAI_PTY_NEEDLE='Done.',
               FYAI_PTY_AFTER=after, FYAI_PTY_AFTER_PAUSE='0',
               FYAI_PTY_AFTER_TIMEOUT='0.3', FYAI_PTY_TIMEOUT='3',
               FYAI_TIMEOUT_SCALE='1')
    result = subprocess.run(
        [sys.executable, os.path.join(tests, 'pty_driver.py'),
         os.path.join(scratch, mode + str(status) + '.out'),
         sys.executable, '-c', child, mode, str(status), release],
        env=env, capture_output=True, text=True, timeout=5)
    if error is None:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode != 0 and error in result.stderr, result.stderr
PY

pass
