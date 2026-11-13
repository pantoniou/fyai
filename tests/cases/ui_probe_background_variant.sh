#!/bin/bash
# SPDX-License-Identifier: MIT
# A measured terminal background selects the startup palette even when
# COLORFGBG describes a terminal that used to have the opposite background.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
fyai_test_on_exit 'rm -rf "$CAPTURES"'

session()
{
    label=$1
    background=$2
    hint=$3
    fyai_test_setup
    export background
    COLORFGBG="$hint" \
    COLORTERM=truecolor \
    pty_run "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --color on --set display/markdown=true \
        --theme ember:auto \
        --set display/screen=fullscreen -m mock-model -i <<'PTY'
timeout: 10
background: "${background}"
steps:
  - type: "!sh -c 'printf READY; sleep 2'"
  - wait-row: "▌ READY"
  - write: "\x1d"
PTY
    cp "$TEST_DIR/pty.out" "$CAPTURES/$label.out"
}

session dark rgb:1e1e/1e1e/2e2e '0;15'
session light rgb:ffff/ffff/ffff '15;0'

"$PYTHON" - "$CAPTURES" <<'PY' || fail "startup palette ignored the terminal background"
import pathlib
import re
import sys

for label, want_light in (("dark", False), ("light", True)):
    data = (pathlib.Path(sys.argv[1]) / (label + ".out")).read_bytes()
    grounds = re.findall(rb"\x1b\[48;2;(\d+);(\d+);(\d+)m", data)
    if not grounds:
        raise SystemExit(label + ": no palette background was drawn")
    rgb = grounds[0]
    light = (299 * int(rgb[0]) + 587 * int(rgb[1]) + 114 * int(rgb[2])) >= 128000
    if light != want_light:
        raise SystemExit(label + ": startup background is " + repr(rgb))
PY

pass
