#!/bin/bash
# SPDX-License-Identifier: MIT
# A remote session shows what a local session shows: the start, an answer, a
# command of the user in its tile, and the screen after the tile closes.
set -eu
. "$(dirname "$0")/../oracle-harness.sh"

oracle_setup claude_sdk_branch.json
"$FYAI_BIN" branch create oracle/session >/dev/null 2>&1
oracle_compare --branch oracle/session <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: "❯"
  - checkpoint: start
  - type: Oracle question.
  - wait-screen: First answer.
  - wait-row: "❯"
  - checkpoint: answer
  - type: "!printf 'bang-row\\n'"
  - wait-screen: Esc closes
  - checkpoint: bang
  - write: "\e"
  - wait-gone: bang-row
  - checkpoint: closed
  - type: /quit
PTY
pass
