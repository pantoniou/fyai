#!/bin/bash
# SPDX-License-Identifier: MIT
# The resume picker opens on the session tree, resumes the one Enter selects,
# and stores nothing when it is cancelled.
set -eu
. "$(dirname "$0")/../harness.sh"

# A temporary base can end in a slash, as it does on macOS runners.
FYAI_TMPDIR_BASE="${TMPDIR:-/tmp}/"
fyai_test_setup

run_fyai branch create older
assert_status 0
run_fyai branch describe older "<command-name>/clear</command-name>"
assert_status 0
run_fyai branch create newer
assert_status 0
run_fyai branch describe newer "the newest session"
assert_status 0

# Make `newer` the most recently updated session, so the row the picker opens
# on is the one this case names.
run_fyai -b newer --set temperature=0.5
assert_status 0

# The picker holds the keys as soon as it opens, so the line the driver sends
# first is read as keys: Enter resumes, Escape cancels.

# --- Enter resumes the selected session ----------------------------------
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=90 \
FYAI_PTY_INPUT="" \
FYAI_PTY_READY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_AFTER="wait-frame:switched to branch|send:/help|wait:Settings|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pick.out" \
	"$FYAI_BIN" -k test-key -m mock-model resume

# The picker drew the sessions it offers and selected the newest one.
grep -qF "toggle foreign sessions" "$TEST_DIR/pick.out" || \
	fail "the picker did not open"
grep -qF "the newest session" "$TEST_DIR/pick.out" || \
	fail "the picker did not describe the sessions"
"$PYTHON" - "$TEST_DIR/pick.out" <<'EOF' || \
	fail "the picker did not use the configured colour theme"
import sys

data = open(sys.argv[1], "rb").read()
end = data.find(b"switched to branch")
if end < 0 or b"\x1b[38;2;" not in data[:end]:
    raise SystemExit("no themed foreground in the picker")
if b"/clear" in data[:end]:
    raise SystemExit("injected Claude command metadata in the picker")
EOF
# Enter resumed the newest of them.
grep -qF "switched to branch newer" "$TEST_DIR/pick.out" || \
	fail "Enter did not resume the selected session"
"$PYTHON" - "$TEST_DIR/pick.out" <<'EOF' || \
	fail "resumed session lost automatic colour"
import sys

data = open(sys.argv[1], "rb").read()
start = data.rfind(b"switched to branch")
help_text = data.find(b"Settings", start)
if start < 0 or help_text < 0 or b"\x1b[" not in data[start:help_text]:
    raise SystemExit("no styling after branch configuration was adopted")
EOF

# A source transcript appears beside native sessions and Enter imports it
# before resuming its generated branch.
mkdir -p "$TEST_DIR/claude-root/projects/project-a"
sed "s|@CWD@|$TEST_DIR|g" \
	"$TESTS_DIR/data/claude-session-picker.jsonl" > \
	"$TEST_DIR/claude-root/projects/project-a/claude-picker.jsonl"
touch -t 203001010000 \
	"$TEST_DIR/claude-root/projects/project-a/claude-picker.jsonl"
CLAUDE_CONFIG_DIR="$TEST_DIR/claude-root" \
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="" FYAI_PTY_SUBMIT_INPUT=0 \
FYAI_PTY_READY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/foreign-picker.snap" \
FYAI_PTY_AFTER="raw:66|wait-frame:claude-code|raw:66|wait-gone:Continue this imported session|raw:66|wait-frame:claude-code|raw:1b5b48|wait-frame:Group · import|raw:6a|wait-frame:/claude-code|raw:6a|wait-frame:2030-01-01|snapshot|raw:69|wait-frame:Ready to continue.|raw:1b|wait-frame:2030-01-01|raw:0d|wait:switched to branch|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/foreign-pick.out" \
	"$FYAI_BIN" -k test-key --color off -m mock-model \
		--set display/branch_preview=bottom resume

grep -qF "import/claude-code/claude-picker" "$TEST_DIR/foreign-pick.out" || \
	fail "the picker did not show the Claude session"
grep -qF "import [group]" "$TEST_DIR/foreign-pick.out" &&
	grep -qF "claude-code [group]" "$TEST_DIR/foreign-pick.out" || \
	fail "the picker did not group foreign sessions in the branch tree"
grep -qF "switched to branch import/claude-code/claude-picker" \
	"$TEST_DIR/foreign-pick.out" || \
	fail "Enter did not import and resume the Claude session"
"$PYTHON" - "$TEST_DIR/foreign-picker.snap" "$TESTS_DIR" <<'EOF' || \
	fail "the picker regions were not separated"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

screen = Screen(24, 100)
screen.feed(open(sys.argv[1], "rb").read())
rows = screen.display()
selected = next(i for i, row in enumerate(rows) if "claude-code · claude-p" in row)
if "3 turns · 130 tokens" not in rows[selected + 1]:
    raise SystemExit("the selected session has no turn and token summary")
session = next(i for i, row in enumerate(rows)
               if "Continue this imported session with a title longer" in row)
if "than forty-eight bytes." not in rows[session]:
    raise SystemExit("the session title was clipped before layout")
tree = next(i for i, row in enumerate(rows) if "import [group]" in row)
commands = next(i for i, row in enumerate(rows)
                if "toggle foreign sessions" in row)
if tree < selected + 2 or rows[tree - 1].strip():
    raise SystemExit("no blank row before the session tree")
if commands <= session or rows[commands - 1].strip():
    raise SystemExit("no blank row before the command legend")
if "select" not in rows[commands + 1] or "fold" not in rows[commands + 1]:
    raise SystemExit("the command legend is incomplete")
if not any("Ready to continue." in row for row in rows):
    raise SystemExit("the foreign session has no automatic preview")
EOF
"$PYTHON" - "$TEST_DIR/foreign-pick.out" <<'EOF' || \
	fail "foreign discovery delayed the initial native-session frame"
import sys

data = open(sys.argv[1], "rb").read()
native = data.find(b"main [current]")
foreign = data.find(b"claude-code")
if native < 0 or foreign < 0 or native > foreign:
    raise SystemExit("foreign row preceded the native-session frame")
EOF

# Modern Codex rollouts put injected user-role context before turn_context.
# Both picker views name the session with the real prompt after that boundary.
mkdir -p "$TEST_DIR/codex-root/sessions/2026/01/01"
cp "$TESTS_DIR/data/codex-session-index.jsonl" \
	"$TEST_DIR/codex-root/session_index.jsonl"
sed "s|@CWD@|$TEST_DIR|g" \
	"$TESTS_DIR/data/codex-session-picker.jsonl" > \
	"$TEST_DIR/codex-root/sessions/2026/01/01/codex-picker.jsonl"
CODEX_HOME="$TEST_DIR/codex-root" CLAUDE_CONFIG_DIR="$TEST_DIR/no-claude" \
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="" FYAI_PTY_SUBMIT_INPUT=0 \
FYAI_PTY_READY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_AFTER="raw:66|wait-frame:Repair session picker|raw:67|drain:0.2|raw:1b|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/codex-picker.out" \
	"$FYAI_BIN" -k test-key --color off -m mock-model \
		--set display/branch_preview=off resume

grep -qF "Repair session picker" "$TEST_DIR/codex-picker.out" || \
	fail "the picker did not show the Codex thread name"
grep -qF "codex [group]" "$TEST_DIR/codex-picker.out" || \
	fail "the picker did not group Codex sessions"
grep -qF "gitgraph overview" "$TEST_DIR/codex-picker.out" && \
	fail "the resume picker switched to gitgraph" || true
grep -qF "the newest session" "$TEST_DIR/codex-picker.out" || \
	fail "tree view did not show the native session description"
grep -qF "Injected repository instructions" "$TEST_DIR/codex-picker.out" && \
	fail "the picker displayed injected Codex context" || true

# Selecting a session does not move HEAD.
run_fyai root show
assert_status 0
assert_stdout_contains "main"

# --- Escape cancels and stores nothing -----------------------------------
run_fyai list reflog
assert_status 0
cp "$TEST_DIR/stdout" "$TEST_DIR/reflog.before"

FYAI_PTY_ROWS=24 FYAI_PTY_COLS=90 \
FYAI_PTY_INPUT=$'\x1b' \
FYAI_PTY_READY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_NEEDLE="toggle foreign sessions" \
FYAI_PTY_AFTER="drain:0.5" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/cancel.out" \
	"$FYAI_BIN" -k test-key --color off -m mock-model resume

grep -qF "toggle foreign sessions" "$TEST_DIR/cancel.out" || \
	fail "the picker did not open"
grep -qF "switched to branch" "$TEST_DIR/cancel.out" && \
	fail "Escape resumed a session" || true

# A cancelled picker publishes nothing.
run_fyai list reflog
assert_status 0
cmp -s "$TEST_DIR/reflog.before" "$TEST_DIR/stdout" || \
	fail "a cancelled picker changed the stored state"

pass
