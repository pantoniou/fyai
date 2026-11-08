#!/bin/bash
# SPDX-License-Identifier: MIT
# The head of the branch moves to another line: the client keeps the turns it
# shares with the worker, drops the others and takes the new ones, and its
# screen is the screen of a local session after the same commands.
set -eu
. "$(dirname "$0")/../oracle-harness.sh"

oracle_setup claude_sdk_branch.json
"$FYAI_BIN" branch create oracle/reset >/dev/null 2>&1
oracle_compare --branch oracle/reset <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: "❯"
  - type: First question.
  - wait-screen: First answer.
  - wait-row: "❯"
  - checkpoint: first
  - type: /reset oracle/reset~2
  - wait-gone: First answer.
  - checkpoint: reset
  - type: Other question.
  - wait-screen: Second answer.
  - wait-row: "❯"
  - checkpoint: other
  - type: /quit
PTY
pass
