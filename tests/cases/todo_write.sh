#!/bin/bash
# SPDX-License-Identifier: MIT
# The todo_write tool replaces the todo list of the branch: the model call
# stores it, `todo` shows it, and `todo clear` drops it. A new branch
# inherits a copy of the list of its parent.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start todo_write.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "plan the work"
assert_status 0
assert_stdout_contains 'Planned.'

# The tool spec offers the Claude TodoWrite shape: full-list rewrite.
assert_request 0 'any(t["function"]["name"] == "todo_write" for t in r["body"]["tools"])'
assert_request 0 'any(t["function"]["name"] == "todo_write" and "todos" in t["function"]["parameters"]["properties"] for t in r["body"]["tools"])'

run_fyai todo
assert_status 0
assert_stdout_contains 'no todos'

# The finished list stays in the ref log behind the clear.
run_fyai list reflog
assert_status 0
assert_stdout_contains 'todo'

run_fyai todo --output json
assert_status 0
assert_stdout_contains '[]'

# A branch keeps its own list: set one on a child and clear it there.
run_fyai branch create child main
assert_status 0
assert_stdout_contains 'created branch child'
run_fyai --branch child tool todo_write '{"todos":[{"content":"child task","status":"pending","priority":"high"}]}'
assert_status 0
assert_stdout_contains 'todo list updated: 1 items'
run_fyai --branch child todo
assert_status 0
assert_stdout_contains 'child task'
run_fyai todo
assert_status 0
assert_stdout_contains 'no todos'

mock_stop 3
pass
