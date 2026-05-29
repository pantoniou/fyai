#!/bin/bash
# SPDX-License-Identifier: MIT
# A sandboxed tool sub-execution sanitizes the environment (secrets in the
# parent env never reach the tool) and, when Landlock is available, confines the
# tool - without breaking ordinary shell operations. Environment sanitization is
# location- and kernel-independent, so this case is robust where Landlock is
# absent (the FS confinement itself is exercised directly in the unit path).
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

export MY_SECRET=topsecret-xyz
export OPENAI_API_KEY=sk-should-not-leak

# The tool's environment is stripped of everything but a safe allow-list, so
# neither an arbitrary secret nor a provider key is visible to the command.
run_fyai --set sandbox=true tool shell '{"command": "echo v=[$MY_SECRET] k=[$OPENAI_API_KEY]"}'
assert_status 0
assert_stdout_contains "v=[] k=[]"
assert_stdout_not_contains "topsecret-xyz"

# PATH is preserved so tools still resolve.
run_fyai --set sandbox=true tool shell '{"command": "test -n \"$PATH\" && echo path-ok"}'
assert_stdout_contains "path-ok"

# The sandbox does not break /dev/null redirection or basic commands.
run_fyai --set sandbox=true tool shell '{"command": "echo hi > /dev/null && echo devnull-ok"}'
assert_stdout_contains "devnull-ok"

# A file in the project stays readable under the sandbox.
run_fyai tool write_file '{"path": "keep.txt", "content": "visible\n"}'
run_fyai --set sandbox=true tool shell '{"command": "cat keep.txt"}'
assert_stdout_contains "visible"

pass
