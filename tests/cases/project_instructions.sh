#!/bin/bash
# SPDX-License-Identifier: MIT
# AGENTS.md / CLAUDE.md ingestion: repo-scoped instruction files discovered from
# the project root down to the cwd are folded into the system prompt of a new
# conversation (and thus the first request), outermost-first; a continuation
# keeps the frozen copy even after the files change.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup   # creates ./.fyai (a project root marker) and inits the arena
mock_start state_continuation.json

# Root-level AGENTS.md + CLAUDE.md, and a nested subdir with its own AGENTS.md.
printf 'ROOT_AGENTS_MARKER: obey the root agents file.\n' > AGENTS.md
printf 'ROOT_CLAUDE_MARKER: and the root claude file.\n' > CLAUDE.md
mkdir -p sub
printf 'SUB_AGENTS_MARKER: nested guidance wins recency.\n' > sub/AGENTS.md

run_fyai --chat-completions --no-stream \
	 -u "$MOCK_URL/v1/chat/completions" -m mock-model "first turn"
assert_status 0

# The first request's system message carries all three, ordered outermost
# (root) first, nested cwd last.
assert_request 0 'any(m["role"]=="system" and "ROOT_AGENTS_MARKER" in m["content"] for m in r["body"]["messages"])'
assert_request 0 'any(m["role"]=="system" and "ROOT_CLAUDE_MARKER" in m["content"] for m in r["body"]["messages"])'

# Nested cwd wins recency: run from sub/, its AGENTS.md must appear and after
# the root one in the concatenated system text.
mock_stop 1
mock_start state_continuation.json
( cd sub && run_fyai --chat-completions --no-stream \
	--new -u "$MOCK_URL/v1/chat/completions" -m mock-model "from subdir" )
assert_status 0
assert_request 0 'next(m["content"] for m in r["body"]["messages"] if m["role"]=="system").find("SUB_AGENTS_MARKER") > next(m["content"] for m in r["body"]["messages"] if m["role"]=="system").find("ROOT_AGENTS_MARKER")'
mock_stop 1

pass
