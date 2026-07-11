#!/bin/bash
# SPDX-License-Identifier: MIT
# ChatGPT auth selection and machine-local credential status. OAuth transport
# itself is covered by the protocol unit/mock tests; this case proves that no
# credential enters the arena-backed configuration.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

run_fyai auth status
assert_status 0
assert_stdout_contains 'Status'
assert_stdout_contains 'Signed out'

run_fyai config set auth chatgpt
assert_status 0
run_fyai config get auth
assert_stdout_contains "chatgpt"

run_fyai config set auth invalid
assert_status_nonzero
assert_stderr_contains "not in enum"

mkdir -p "$XDG_STATE_HOME/fyai"
"$PYTHON" - "$XDG_STATE_HOME/fyai/auth.json" <<'PY'
import base64, json, os, sys, time
def enc(value):
    return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b"=").decode()
jwt = ".".join((enc({"alg": "none"}), enc({
    "email": "user@example.com",
    "exp": int(time.time()) + 3600,
    "https://api.openai.com/auth": {
        "chatgpt_account_id": "account-test",
        "chatgpt_plan_type": "pro"
    }
}), enc("signature")))
with open(sys.argv[1], "w") as f:
    json.dump({
        "type": "chatgpt",
        "access_token": "secret-access",
        "refresh_token": "secret-refresh",
        "id_token": jwt,
        "expires_at": int(time.time()) + 3600
    }, f)
os.chmod(sys.argv[1], 0o600)
PY

run_fyai auth status
assert_status 0
assert_stdout_contains 'Provider'
assert_stdout_contains 'openai'
assert_stdout_contains 'user@example.com'
assert_stdout_contains 'account-test'
assert_stdout_contains 'Subscription'
assert_stdout_contains 'pro'
assert_stdout_contains 'Effective method'
assert_stdout_contains 'chatgpt'
assert_stdout_not_contains "secret-access"
assert_stdout_not_contains "secret-refresh"

run_fyai auth openai info --json
assert_status 0
assert_stdout_contains '"provider": "openai"'
assert_stdout_contains '"status": "signed_in"'
assert_stdout_contains '"plan": "pro"'
assert_stdout_not_contains "secret-access"

run_fyai auth anthropic status
assert_status_nonzero
assert_stderr_contains "not supported yet"

# The arena stores only intent, never the token material.
run_fyai config effective
assert_stdout_contains "auth: chatgpt"
assert_stdout_not_contains "secret-access"
assert_stdout_not_contains "secret-refresh"

pass
