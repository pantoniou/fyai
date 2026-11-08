#!/bin/bash
# SPDX-License-Identifier: MIT
# An editor that a command of a remote session starts runs in a terminal of
# the worker, which the client shows as a local session shows its editor: in
# a tile that takes the pane and the keys.
set -eu
. "$(dirname "$0")/../oracle-harness.sh"

oracle_setup claude_sdk_branch.json
"$FYAI_BIN" branch create oracle/editor >/dev/null 2>&1
cat >"$TEST_DIR/editor.sh" <<'SH'
#!/bin/sh
# The last output has no newline: a pseudo-terminal can deliver a line and its
# newline in two reads, and the cursor would rest on one row or the next.
printf 'editor-ready\nsettled'
read line
SH
chmod +x "$TEST_DIR/editor.sh"
export EDITOR="$TEST_DIR/editor.sh" VISUAL="$TEST_DIR/editor.sh"
oracle_compare --branch oracle/editor <<'PTY'
rows: 30
cols: 100
# The editor gets a file of its own in each run.
mask:
  - "fyai-config-[A-Za-z0-9]+"
steps:
  - wait-screen: "❯"
  - type: /config edit
  - wait-screen: settled
  - checkpoint: editor
  - wait-screen: Ctrl-] returns to the prompt
  - type: done
  - wait-gone: Ctrl-] returns to the prompt
  - checkpoint: after
  - type: /quit
PTY
pass
