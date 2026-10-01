#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup

"$PYTHON" - "$FYAI_BIN" "$TEST_DIR" <<'PY'
import json, os, pathlib, selectors, signal, subprocess, sys, time, urllib.parse

binary, scratch = sys.argv[1:]
state = pathlib.Path(os.environ["XDG_STATE_HOME"]) / "fyai" / "auth.json"

def attempt(extra=(), callback=None):
    process = subprocess.Popen([binary, "--color", "off", "auth", "login", "--manual", *extra],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout = b""
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 10 * float(os.environ.get("FYAI_TIMEOUT_SCALE", "1"))
    try:
        while b"Paste the complete redirect URL:" not in stdout:
            remaining = deadline - time.monotonic()
            assert remaining > 0, stdout
            assert selector.select(remaining), stdout
            chunk = os.read(process.stdout.fileno(), 65536)
            assert chunk, (stdout, process.stderr.read())
            stdout += chunk
        text = stdout.decode()
        start = text.index("https://auth.openai.com/")
        url = text[start:].split()[0]
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(url).query)
        assert url.startswith("https://auth.openai.com/api/accounts/authorize?")
        assert query["resource"] == ["https://api.openai.com/v1"]
        assert "chatgpt.tokens.use.direct" in query["scope"][0].split()
        assert query["nonce"][0] != query["state"][0]
        assert query["code_challenge_method"] == ["S256"]
        assert query["redirect_uri"][0].startswith("http://127.0.0.1:")
        if callback:
            payload = callback(query)
        else:
            payload = "state=" + urllib.parse.quote(query["state"][0]) + "&error=access_denied"
        process.stdin.write((query["redirect_uri"][0] + "?" + payload + "\n").encode())
        process.stdin.flush()
        out, err = process.communicate(timeout=10)
        assert process.returncode != 0, (stdout + out, err)
        return query, err.decode()
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

first, err = attempt()
assert first["client_id"] == ["dynamic_agent_client"]
assert first["agent_name_hint"] == ["fyai"]
assert "permission declined" in err
host = first["ext_agent_host_id"][0]
assert host.startswith("urn:uuid:")
assert state.stat().st_mode & 0o077 == 0
assert json.loads(state.read_text())["ext_agent_host_id"] == host
second, err = attempt(callback=lambda q: "state=" + q["state"][0] + "&code=test-code")
assert second["ext_agent_host_id"] == [host]
assert "issued client ID" in err
assert first["state"] != second["state"]
assert first["nonce"] != second["nonce"]

registration = json.loads(state.read_text())
registration.update(client_id="oaiapp_saved", subject="saved-user", email="saved@example.com")
record = dict(registration)
registration["registrations"] = {"oaiapp_saved": record}
state.write_text(json.dumps(registration))
returned, err = attempt(callback=lambda q: "state=" + q["state"][0] + "&code=test-code&client_id=oaiapp_wrong")
assert returned["client_id"] == ["oaiapp_saved"]
assert "agent_name_hint" not in returned
assert "differs from the selected registration" in err
assert json.loads(state.read_text())["client_id"] == "oaiapp_saved"
selected, err = attempt(("--account", "oaiapp_saved"))
assert selected["client_id"] == ["oaiapp_saved"]
added, err = attempt(("--new-account",))
assert added["client_id"] == ["dynamic_agent_client"]
assert added["ext_agent_host_id"] == [host]
assert json.loads(state.read_text())["client_id"] == "oaiapp_saved"
PY

run_fyai auth accounts --output json
assert_status 0
assert_stdout_contains 'oaiapp_saved'
assert_stdout_contains 'saved@example.com'
run_fyai auth login --device-code
assert_status_nonzero
assert_stderr_contains 'requires browser sign-in'
run_fyai auth logout
assert_status 0
run_fyai auth accounts --output json
assert_stdout_contains 'oaiapp_saved'
run_fyai auth usage --output json
assert_status 0
assert_stdout_contains 'https://chatgpt.com/settings/usage'
assert_stdout_contains '"total_tokens": 0'
assert_stdout_contains 'Unavailable through the documented direct-client API'
pass
