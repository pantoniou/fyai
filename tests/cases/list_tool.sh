#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the list tool: it gives the agents, views, shells and waits that the
# caller owns, by the names it gave them, and refuses a kind it does not know.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start list_tool.json

run_fyai --set api=chat-completions --set display/stream=false --set tools=true \
	 --set api_url="$MOCK_URL/v1/chat/completions" -m mock-model "set things up and list them"
assert_status 0
assert_stdout_contains 'Listed.'

assert_request 0 'any(t["function"]["name"] == "list" for t in r["body"]["tools"])'
assert_request 0 'not any("list" in t["function"]["parameters"]["properties"].get("action", {}).get("enum", []) for t in r["body"]["tools"] if t["function"]["name"] == "project_view")'
assert_request 6 'any(m.get("tool_call_id") == "all" and json.loads(m["content"])["agents"] == [{"name": "helper", "state": "finished", "view": False}] and json.loads(m["content"])["views"] == [] and [s["name"] for s in json.loads(m["content"])["shells"]] == ["srv"] and json.loads(m["content"])["shells"][0]["command"] == "sleep 30" and [w["name"] for w in json.loads(m["content"])["waits"]] == ["later"] and json.loads(m["content"])["waits"][0]["reason"] == "check the build" for m in r["body"]["messages"])'
assert_request 6 'any(m.get("tool_call_id") == "waits" and list(json.loads(m["content"])) == ["waits"] for m in r["body"]["messages"])'
# A tool result goes on the wire as text: an object as the output of a function call is
# a request error at the providers.
assert_request 7 'all(isinstance(m.get("content"), str) for m in r["body"]["messages"] if m.get("role") == "tool")'
assert_request 7 'any(m.get("tool_call_id") == "bad" and str(m["content"]).startswith("tool error:") for m in r["body"]["messages"])'

mock_stop 8
pass
