#!/bin/bash
# SPDX-License-Identifier: MIT
# A work band shows the last rows of its content, so anything past its cap
# scrolls off the top - and the top is the invocation. The band must stay
# within its cap, or a long-running command loses the label and the command
# that say what is running.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_band_invocation_held.json

FYAI_PTY_COLS=100 FYAI_PTY_INPUT="run it" \
FYAI_PTY_NEEDLE="Done." \
FYAI_PTY_PROGRESS_NEEDLE="⋯" \
FYAI_PTY_PROGRESS_TIMEOUT=5 \
FYAI_PTY_PROGRESS_RELEASE="$TMPDIR/band-release" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/tool_update_interval_ms=0 \
    --set display/tool_preview_lines=5 \
    --set builtin_shell=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

# The cap binds once the body fills it, which is when the omission row appears.
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' || fail "the band dropped its invocation rows"
import sys

sys.path.insert(0, sys.argv[2])
from screen import rows_at

rows = rows_at(sys.argv[1], "⋯".encode())
body = [i for i, r in enumerate(rows) if "⋯" in r]
if not body:
    raise SystemExit("no bounded body on screen")
above = rows[:body[0]]
if not any(r.strip().endswith("shell") for r in above):
    raise SystemExit("the shell label scrolled off the band:\n" +
                     "\n".join(rows))
if not any("⎿" in r for r in above):
    raise SystemExit("the command row scrolled off the band:\n" +
                     "\n".join(rows))
PY

mock_stop 2
pass
