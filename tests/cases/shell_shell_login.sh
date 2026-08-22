#!/bin/bash
# SPDX-License-Identifier: MIT
# A shell call names the shell it runs under and whether that shell reads the
# profile of the user. `login` is what a command needs when the tool it calls
# is put on the path by the profile, and `shell` is what a command needs when
# it uses what another shell adds.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_shell_login.json

# A login shell reads this; a shell that is not one does not.
printf 'FYAI_LOGIN_MARK=login-ok\nexport FYAI_LOGIN_MARK\n' > "$HOME/.profile"

# A shell of our own, which reports the command it was given. `-c` is $1 and
# the command is $2, as they are for any shell that runs a command.
cat > fakesh <<'SH'
#!/bin/sh
echo "fake-shell-ran: $2"
SH
chmod +x fakesh

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set "api_url=$MOCK_URL/v1/chat/completions" \
	 -m mock-model "check the shell selection"
assert_status 0
assert_stdout_contains "Shell selection checked."

# The login shell read the profile of the user.
assert_request 1 'any("mark=login-ok" in m.get("content", "") for m in r["body"]["messages"] if m.get("tool_call_id") == "call_login")'
# The named shell ran, and it was given the command.
assert_request 1 'any("fake-shell-ran: echo hello" in m.get("content", "") for m in r["body"]["messages"] if m.get("tool_call_id") == "call_shell")'

mock_stop 2
pass
