#!/bin/bash
# SPDX-License-Identifier: MIT
# Foreign import dry-run identifies both supported JSONL formats and reports
# their portable conversation records without publishing a branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai init
assert_status 0

mkdir -p "$TEST_DIR/claude-root/projects/project-a"
mkdir -p "$TEST_DIR/claude-root/projects/project-a/session/subagents"
mkdir -p "$TEST_DIR/codex-root/sessions/2026/01/01"
cp "$TESTS_DIR/data/codex-session-index.jsonl" \
	"$TEST_DIR/codex-root/session_index.jsonl"
cp "$TESTS_DIR/data/claude-session.jsonl" \
	"$TEST_DIR/claude-root/projects/project-a/claude-test.jsonl"
cp "$TESTS_DIR/data/claude-session.jsonl" \
	"$TEST_DIR/claude-root/projects/project-a/session/subagents/agent-a.jsonl"
cp "$TESTS_DIR/data/codex-session.jsonl" \
	"$TEST_DIR/codex-root/sessions/2026/01/01/codex-test.jsonl"
sed "s|@CWD@|$TEST_DIR|g" \
	"$TESTS_DIR/data/codex-session-picker.jsonl" > \
	"$TEST_DIR/codex-root/sessions/2026/01/01/codex-picker.jsonl"

run_fyai import --from claude-code --source-root "$TEST_DIR/claude-root" \
	--list --all --json
assert_status 0
test "$(grep -o '"id": "claude-test"' "$TEST_DIR/stdout" | wc -l)" \
	-eq 1 || fail "Claude discovery included a subagent transcript"

run_fyai import --from codex --source-root "$TEST_DIR/codex-root" \
	--list --all --json
assert_status 0
grep -q '"id": "codex-test"' "$TEST_DIR/stdout" || \
	fail "Codex discovery did not find the rollout"
grep -q '"cwd": "/tmp/project"' "$TEST_DIR/stdout" || \
	fail "Codex discovery did not preserve the recorded directory"
grep -q 'Repair session picker' "$TEST_DIR/stdout" || \
	fail "Codex discovery did not use the indexed thread name"
grep -q 'Injected repository instructions' "$TEST_DIR/stdout" && \
	fail "Codex discovery used injected context as its title" || true

run_fyai import --from auto --source-root "$TEST_DIR/codex-root" \
	--list --all
assert_status_nonzero

run_fyai import --from codex --source-root "$TEST_DIR/missing" --list --all
assert_status_nonzero

run_fyai import --from fyai --list
assert_status_nonzero

run_fyai import --from auto --dry-run --json \
	-i "$TESTS_DIR/data/claude-session.jsonl"
assert_status 0
grep -q '"source":[[:space:]]*"claude-code"' "$TEST_DIR/stdout" || \
	fail "Claude Code input was not detected"
grep -q '"messages":[[:space:]]*7' "$TEST_DIR/stdout" || \
	fail "Claude Code messages were not counted"
grep -q '"tool_calls":[[:space:]]*2' "$TEST_DIR/stdout" || \
	fail "Claude Code tool call was not counted"
grep -q '"tool_results":[[:space:]]*2' "$TEST_DIR/stdout" || \
	fail "Claude Code tool result was not counted"
grep -q '"compactions":[[:space:]]*1' "$TEST_DIR/stdout" || \
	fail "Claude Code compaction was not counted"

run_fyai import --from auto --dry-run --json \
	-i "$TESTS_DIR/data/codex-session.jsonl"
assert_status 0
grep -q '"source":[[:space:]]*"codex"' "$TEST_DIR/stdout" || \
	fail "Codex input was not detected"
grep -q '"messages":[[:space:]]*1' "$TEST_DIR/stdout" || \
	fail "Codex messages were not counted"
grep -q '"tool_calls":[[:space:]]*1' "$TEST_DIR/stdout" || \
	fail "Codex tool call was not counted"
grep -q '"tool_results":[[:space:]]*1' "$TEST_DIR/stdout" || \
	fail "Codex tool result was not counted"
grep -q '"compactions":[[:space:]]*1' "$TEST_DIR/stdout" || \
	fail "Codex compaction was not counted"

# Codex records catalogue tools as custom calls wrapped in orchestration
# JavaScript. Import the called tool and its arguments, not that wrapper.
run_fyai --branch imported-custom import --from codex \
	-i "$TESTS_DIR/data/codex-session-custom-tools.jsonl"
assert_status 0
run_fyai --branch imported-custom export -o codex-custom.md
assert_status 0
grep -q 'make && ./hello' codex-custom.md || \
	fail "Codex exec_command did not become a shell call"
grep -q 'hello.c' codex-custom.md || \
	fail "Codex apply_patch content was not imported"
grep -q 'tool: exec_command' codex-custom.md || \
	fail "Codex exec wrapper did not resolve through the tool catalogue"
grep -q 'yield-time_ms' codex-custom.md && \
	fail "Codex orchestration arguments leaked into the shell call" || true
grep -q 'tool: apply_patch' codex-custom.md || \
	fail "Codex patch wrapper did not resolve through the tool catalogue"
grep -q 'custom_tool_call' codex-custom.md && \
	fail "Codex orchestration wrapper leaked into the transcript" || true
grep -q '\[{"type": "input_text"' codex-custom.md && \
	fail "Codex structured tool output was emitted as JSON" || true
grep -q 'Script completed' codex-custom.md && \
	fail "Codex orchestration status leaked into shell output" || true
grep -q "result: '{}'" codex-custom.md && \
	fail "Codex empty patch result was displayed" || true
run_fyai --branch imported-custom --color off history
assert_status 0
grep -q 'shell' "$TEST_DIR/stdout" || \
	fail "Codex exec_command did not replay as a shell"
grep -q '⎿  make && ./hello' "$TEST_DIR/stdout" || \
	fail "Codex shell did not replay as a native tool exchange"
grep -q 'make && ./hello' "$TEST_DIR/stdout" || \
	fail "Codex shell command was absent from replay"
grep -q 'exec_command `' "$TEST_DIR/stdout" && \
	fail "Codex wire tool name leaked into replay" || true

run_fyai import --from codex --dry-run --json \
	-i "$TESTS_DIR/data/codex-session-loss.jsonl"
assert_status 0
grep -q '"losses":[[:space:]]*1' "$TEST_DIR/stdout" || \
	fail "unsupported Codex content was not reported"

run_fyai --branch imported-loss import --from codex \
	-i "$TESTS_DIR/data/codex-session-loss.jsonl"
assert_status 0
run_fyai --branch imported-loss export -o imported-loss.md
assert_status 0
grep -q '\[foreign content omitted\]' imported-loss.md || \
	fail "unsupported content did not receive a transcript placeholder"

# A real import publishes the source's final context on a new branch.  The
# compacted-away prefix must not leak back into that active context.
run_fyai import --from codex --source-root "$TEST_DIR/codex-root" \
	--session codex-test
assert_status 0
grep -q 'import/codex/codex-test' "$TEST_DIR/stdout" || \
	fail "Codex import did not report its destination"

run_fyai --branch import/codex/codex-test export -o codex-import.md
assert_status 0
grep -q 'Compact summary: the target is sample.c.' codex-import.md || \
	fail "Codex compact replacement was not imported"
grep -q 'Ready to continue.' codex-import.md || \
	fail "Codex final assistant response was not imported"
grep -q 'kind: tool_call' codex-import.md || \
	fail "Codex compact replacement tool call was not imported"
grep -q 'int imported;' codex-import.md || \
	fail "Codex compact replacement tool result was not imported"
if grep -q 'Inspect it.' codex-import.md; then
	fail "Codex pre-compaction context remained active"
fi

# Repeating the same source identity returns the existing branch.
run_fyai import --from codex -i "$TESTS_DIR/data/codex-session.jsonl"
assert_status 0
grep -q 'already branch import/codex/codex-test' "$TEST_DIR/stdout" || \
	fail "Codex reimport was not idempotent"

run_fyai import --from claude-code \
	-i "$TESTS_DIR/data/claude-session.jsonl"
assert_status 0
run_fyai --branch import/claude-code/claude-test export \
	-o claude-import.md
assert_status 0
grep -q 'Compact summary: the target is sample.c.' claude-import.md || \
	fail "Claude compact summary was not imported"
grep -q 'kind: tool_call' claude-import.md || \
	fail "Claude post-compaction tool call was not imported"
grep -q 'tool: read_file' claude-import.md || \
	fail "Claude Read did not map to read_file"
grep -q 'int imported;' claude-import.md || \
	fail "Claude post-compaction tool result was not imported"
if grep -q 'Inspect it.' claude-import.md; then
	fail "Claude pre-compaction context remained active"
fi
run_fyai --branch import/claude-code/claude-test --color off history
assert_status 0
grep -q 'read sample.c' "$TEST_DIR/stdout" || \
	fail "Claude Read did not replay as a native tool exchange"
grep -q 'Read `' "$TEST_DIR/stdout" && \
	fail "Claude wire tool name leaked into replay" || true

run_fyai --branch claude-native-tools import --from claude-code \
	-i "$TESTS_DIR/data/claude-session-native-tools.jsonl"
assert_status 0
run_fyai --branch claude-native-tools export -o claude-native-tools.md
assert_status 0
grep -q 'tool: write_file' claude-native-tools.md || \
	fail "Claude Write did not map to write_file"
grep -q 'tool: read_file' claude-native-tools.md || \
	fail "Claude Read did not map to read_file"
grep -q 'tool: exec_command' claude-native-tools.md || \
	fail "Claude Bash did not map to exec_command"
grep -q 'file_path' claude-native-tools.md && \
	fail "Claude path argument leaked into native tool calls" || true
run_fyai --branch claude-native-tools --color off history
assert_status 0
grep -q 'write hello.c' "$TEST_DIR/stdout" || \
	fail "Claude Write did not use native display"
grep -q 'read hello.c' "$TEST_DIR/stdout" || \
	fail "Claude Read did not use native display"
grep -q 'Build hello' "$TEST_DIR/stdout" || \
	fail "Claude Bash did not use its native shell description"

# An explicit branch is a distinct destination and must not move stored HEAD.
run_fyai root print main
assert_status 0
cp "$TEST_DIR/stdout" "$TEST_DIR/main-before"
run_fyai --branch imported-explicit import --from claude-code \
	-i "$TESTS_DIR/data/claude-session.jsonl"
assert_status 0
run_fyai --branch imported-explicit export -o explicit-import.md
assert_status 0
grep -q 'Ready to continue.' explicit-import.md || \
	fail "the explicit import destination has no conversation"
run_fyai root print main
assert_status 0
cmp "$TEST_DIR/main-before" "$TEST_DIR/stdout" || \
	fail "foreign import changed the main branch"

run_fyai import --from claude-code --dry-run \
	-i "$TESTS_DIR/data/codex-session.jsonl"
assert_status_nonzero

# The relaxed-flow parser reads JSONL from standard input as multiple generic
# documents too.  run_fyai fixes stdin, so invoke the binary directly.
set +e
"$FYAI_BIN" -k test-key --color off import --from auto --dry-run --json \
	<"$TESTS_DIR/data/codex-session.jsonl" \
	>"$TEST_DIR/stdout" 2>"$TEST_DIR/stderr"
FYAI_STATUS=$?
set -e
assert_status 0
grep -q '"source":[[:space:]]*"codex"' "$TEST_DIR/stdout" || \
	fail "Codex standard input was not detected"

# Native Markdown import retains its established option path.
run_fyai import --dry-run -i "$TESTS_DIR/data/imported_conversation.md"
assert_status_nonzero

pass
