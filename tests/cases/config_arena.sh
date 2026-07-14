#!/bin/bash
# SPDX-License-Identifier: MIT
# Config: the repo config lives in the arena container root - init ingests
# it, show/get/set/export operate on it, and no .fyai/config.yaml appears.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# init already ingested the harness config.yaml (display.markdown: false)
run_fyai config get display/markdown
assert_status 0
assert_stdout_contains "false"

# no plain-file repo config anywhere
[ ! -e .fyai/config.yaml ] || fail ".fyai/config.yaml was created"

# set is visible to a subsequent invocation (durable across exits)
run_fyai config set model arena-model
assert_status 0
run_fyai config get model
assert_status 0
assert_stdout_contains "arena-model"

# slash-separated keys reach nested levels
run_fyai config set display/code_theme kanagawa
assert_status 0
run_fyai config get display/code_theme
assert_status 0
assert_stdout_contains "kanagawa"

run_fyai config set sandbox true
assert_status 0
run_fyai config show
assert_status 0
assert_stdout_contains "sandbox: true"
run_fyai config effective
assert_status 0
assert_stdout_contains "sandbox: true"

# Landlock confinement cannot be disabled: the dedicated verb refuses.
run_fyai sandbox off
assert_status_nonzero
assert_stderr_contains "cannot be disabled"

run_fyai sandbox on
assert_status 0
run_fyai sandbox show
assert_status 0
assert_stdout_contains "~/.ssh"
assert_stdout_contains "443"

# effective config picks the arena values up
run_fyai config show
assert_status 0
assert_stdout_contains "model: arena-model"

# export round-trips
run_fyai config export
assert_status 0
assert_stdout_contains "model: arena-model"
assert_stdout_contains "code_theme: kanagawa"

# a raw api_key never enters the arena
run_fyai config set api_key sk-raw-secret
assert_status_nonzero
assert_stderr_contains "raw api_key"

# init inside an initialized project creates a new project underneath
mkdir -p subdir
cd subdir
run_fyai init
assert_status 0
assert_stderr_contains "nested inside the fyai project"
[ -e .fyai/arena ] || fail "nested init created no arena"
# ... which shadows the enclosing arena from here down: the nested arena is
# seeded from the embedded config sample, so it carries the sample's model
# (gpt-5.4-mini), not the enclosing arena's arena-model.
run_fyai config get model
assert_status 0
assert_stdout_contains "gpt-5.4-mini"
run_fyai config get sandbox
assert_status 0
assert_stdout_contains "true"
cd ..

# messages-mode feature notes stay off config verbs
run_fyai --set api=messages config get model
assert_status 0
assert_stderr_not_contains "built-in shell tool"

pass
