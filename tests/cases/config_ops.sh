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

pass
