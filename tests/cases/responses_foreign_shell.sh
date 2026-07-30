#!/bin/bash
# SPDX-License-Identifier: MIT
# Test the Responses grammar on a provider other than OpenAI.
#
# Multiple providers use Responses, but not all providers accept the built-in
# shell tool and its `shell_call` items. An unsupported item does not match the
# provider input union, and the provider rejects the complete request. Omit the
# declaration and send stored items as the standard `shell` function tool.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start responses_foreign_shell.json

# Select the model before the mock URL. A model change derives a new endpoint
# and would overwrite a previously configured mock URL.
run_fyai config set model openrouter/command-a
assert_status 0
run_fyai config set api responses
assert_status 0
run_fyai config set builtin_shell true
assert_status 0
run_fyai config set api_url "$MOCK_URL/v1/responses"
assert_status 0

run_fyai --set display/stream=false "run the shell command"
assert_status 0
assert_stdout_contains "Foreign provider round trip done."

# Do not declare the built-in shell to this provider. Offer the function shell
# tool instead.
assert_request 0 'not any(t.get("type") == "shell" for t in r["body"]["tools"])'
assert_request 0 'any(t.get("name") == "shell" for t in r["body"]["tools"])'

# Send the stored native items as a standard function call and result.
assert_request 1 'not any(i.get("type", "").startswith("shell_call") for i in r["body"]["input"])'
assert_request 1 'any(i.get("type") == "function_call" and i.get("name") == "shell" and "echo foreign-shell-ran" in i.get("arguments", "") for i in r["body"]["input"])'

# A function_call_output contains a string, not the native output sequence.
assert_request 1 'any(i.get("type") == "function_call_output" and isinstance(i.get("output"), str) and "foreign-shell-ran" in i["output"] for i in r["body"]["input"])'

mock_stop 2
pass
