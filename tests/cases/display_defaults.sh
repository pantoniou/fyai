#!/bin/bash
# SPDX-License-Identifier: MIT
# An interactive session opens on the page renderer, on the alternate screen,
# with the work pane at half the terminal and the mouse controls of a tile.
# The arena of a case states another display, thus the defaults need a case of
# their own.
set -eu
. "$(dirname "$0")/../harness.sh"

# An arena with no stored display, so the compiled defaults answer.
fyai_test_setup_bare
mkdir -p "$HOME"
"$FYAI_BIN" init >/dev/null 2>&1 || fail "fyai init"

want()
{
    key=$1
    value=$2
    got=$("$FYAI_BIN" -k test-key -m mock-model --transient \
          config get "display/$key" 2>/dev/null) || \
        fail "cannot read display/$key"
    [ "$got" = "$value" ] || \
        fail "display/$key is $got, expected $value"
}

want renderer page
want screen fullscreen
want work_zoom_rows half
want work_controls full

# The schema and the sample document the same defaults.
"$PYTHON" - "$FYAI_BIN" <<'PY' || fail "the schema does not state the defaults"
import subprocess
import sys

schema = subprocess.run([sys.argv[1], "config", "schema"],
                        capture_output=True, text=True, check=True).stdout
want = {"renderer": "page", "screen": "fullscreen",
        "work_zoom_rows": "half", "work_controls": "full"}
missing = []
for key, value in want.items():
    at = schema.find("%s:" % key)
    if at < 0 or ("default: %s" % value) not in schema[at:at + 900]:
        missing.append("%s: %s" % (key, value))
if missing:
    raise SystemExit("the schema does not default " + ", ".join(missing))
PY

pass
