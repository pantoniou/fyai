#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify two concurrent sub-agents tiling one work pane.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agents_parallel.json

FYAI_PTY_INPUT="delegate both halves" \
FYAI_PTY_MID_NEEDLE="second half" \
FYAI_PTY_MID_TIMEOUT="3" \
FYAI_PTY_NEEDLE="Both sub-agents reported." \
FYAI_PTY_AFTER="send:/status|wait:Usage / total" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/snapshot.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "parallel agent bands are wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# Both delegations are labelled, by name and description.
for needle in (b"[alpha] first half", b"[beta] second half"):
    if needle not in plain:
        raise SystemExit("missing agent label: %r" % needle)
# Both delegations were on screen while both were still running: each label
# is written before any agent is marked done. Proximity in the byte stream
# would only measure how often the compositor happened to repaint.
first_done = data.find(b"\x1b[32m\xe2\x97\x8f")
if first_done < 0:
    raise SystemExit("no agent was ever marked done")
for needle in (b"[alpha]", b"[beta]"):
    at = data.find(needle)
    if at < 0 or at > first_done:
        raise SystemExit("agent bands never ran concurrently: %r" % needle)

# A sub-agent that is still working has a blinking mark in the pending colour
# on the title row of its screen.
if not re.search(rb"\x1b\[33m(\x1b\[[0-9;]*m)*\xe2\x97\x8f", data):
    raise SystemExit("a running sub-agent was not marked as running")
if not re.search(rb"\x1b\[33m(?:\x1b\[[0-9;]*m)* ", data):
    raise SystemExit("the running sub-agent mark never blinked off")
pending_dots = re.findall(
    rb"\x1b\[33m(?:\x1b\[[0-9;]*m)*\xe2\x97\x8f", data)
if len(pending_dots) < 2:
    raise SystemExit("the running sub-agent mark never blinked on again")
if not re.search(rb"\x1b\[32m(\x1b\[[0-9;]*m)*\xe2\x97\x8f", data):
    raise SystemExit("a finished sub-agent was not marked as done")
# What each sub-agent drew is on its own screen. The compositor writes only
# the cells that changed, so a word can reach the capture in pieces: the
# reading is the screen, not the byte stream.
import os
sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

screen = Screen(30, 100)
screen.feed(data)
shown = "\n".join(screen.lines())

if "REPORT-ALPHA" not in shown:
    raise SystemExit("the first sub-agent's screen was not shown")
if "REPORT-BETA" not in shown:
    raise SystemExit("the second sub-agent's screen was not shown")
# Do not show the full delegated task.
for needle in ("TASK-ALPHA", "TASK-BETA"):
    if needle in shown:
        raise SystemExit("sub-agent task leaked: %r" % needle)
if "Both sub-agents reported." not in shown:
    raise SystemExit("parent final answer missing")

# Require nonoverlapping tiles in one work-pane row.
# Use persistent tile titles instead of timing-dependent final output.
sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames_with_both

frames = frames_with_both(data, "[alpha]", "[beta]", 30, 100)
if not frames:
    raise SystemExit("the two sub-agent screens were never both on screen")
tiled = [f for f in frames if f[0] == f[1]]
if not tiled:
    raise SystemExit(
        "the sub-agent screens were stacked, not tiled: rows %r"
        % (sorted({(a, b) for a, b, _ in frames}),))
for _, _, row in tiled:
    a_at = row.index("[alpha]")
    b_at = row.index("[beta]")
    if not a_at < b_at:
        raise SystemExit("the first sub-agent is not left of the second")
    # Require two bounded columns separated by the pane rule.
    if b_at < 50:
        raise SystemExit("the second tile did not start at its own column: %d"
                         % b_at)
    if "\u2503" not in row[a_at:b_at]:
        raise SystemExit("no rule between the tiles")
EOF

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "the first completed parallel agent was not retired promptly"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

data = open(sys.argv[1], "rb").read()
retired = False
for disp in frames(data, 30, 100):
    shown = "\n".join(disp)
    # Alpha's report has been committed while beta is still live. The work
    # pane must now contain beta alone, not alpha's completed surface too.
    if "REPORT-ALPHA" not in shown or "[beta]" not in shown:
        continue
    if sum(line.count("● agent") for line in disp) <= 1:
        retired = True
if not retired:
    raise SystemExit("no frame retired alpha while beta was still displayed")
EOF

"$PYTHON" "$TESTS_DIR/assert_work_retired.py" \
    "$TEST_DIR/snapshot.out" "● agent" || fail "completed agents remained in the work pane"

# Each sub-agent ran its own restricted, agent-free tool loop.
assert_request 1 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'
assert_request 2 'not any(t["function"]["name"] == "agent" for t in r["body"]["tools"])'
# The parent folded both reports back as the two tool results.
assert_request 3 \
	'all(any(m.get("tool_call_id") == call and "REPORT" in m.get("content", "") '\
'for m in r["body"]["messages"]) for call in ("call_agent_alpha", "call_agent_beta"))'

mock_stop 4
pass
