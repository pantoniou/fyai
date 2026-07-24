#!/bin/bash
# SPDX-License-Identifier: MIT
# Global --set/--get/--delete config ops (repeatable, slash paths, typed YAML
# values) and the --transient overlay. No API key is needed for config-only
# runs.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# Bare --set/--get: no verb, no prompt, no API key required.
run_fyai --set model=set-model --get model
assert_status 0
assert_stdout_contains "set-model"

# Typed values: a flow mapping stays a mapping; --get emits one line.
run_fyai --set 'a/b/c={x: 1}'
assert_status 0
run_fyai --get a/b/c
assert_status 0
assert_stdout_contains "{x: 1}"

# Nested get of a parent returns the sub-tree as a one-line flow document.
run_fyai --get a
assert_status 0
assert_stdout_contains "{b: {c: {x: 1}}}"

# Persisted across invocations (durable by default).
run_fyai config get model
assert_status 0
assert_stdout_contains "set-model"

# --delete removes a leaf; the parent stays.
run_fyai --delete a/b/c
assert_status 0
run_fyai config get a
assert_status 0
assert_stdout_contains "{b: {}}"

# --transient: the edit is visible this run but never persists.
run_fyai --transient --set model=ghost --get model
assert_status 0
assert_stdout_contains "ghost"
run_fyai config get model
assert_status 0
assert_stdout_contains "set-model"

# A raw api_key is still refused by --set.
run_fyai --set api_key=sk-raw
assert_status_nonzero
assert_stderr_contains "raw api_key"

# Logical secret references are non-secret configuration even when the current
# host does not provide a secret-store backend.
run_fyai config set api_key '{ type: secret, value: api-key/openai }'
assert_status 0
run_fyai config get api_key
assert_status 0
assert_stdout_contains 'type: secret'
run_fyai config delete api_key
assert_status 0

run_fyai config set api_key '{ type: auto }'
assert_status 0
run_fyai config get api_key
assert_status 0
assert_stdout_contains 'type: auto'

run_fyai config set api_key '{ type: secret }'
assert_status_nonzero
assert_stderr_contains 'api_key.value is required'

# Credential references remain valid configuration when their environment
# variables are unavailable. Resolution and its diagnostic belong to the MCP
# server startup that actually needs the credential.
unset MCP_API_KEY
run_fyai config set mcp/enabled true
assert_status 0
run_fyai config get mcp/enabled
assert_status 0
assert_stdout_contains 'true'

pass
