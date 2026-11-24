#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A hermetic stand-in for the Claude Code CLI of the claude-code backend.

It speaks the CLI side of the stream-json protocol of the Claude Agent SDK:
the answer to initialize, the handshake of the SDK MCP server "fyai", the
stream events of each scripted reply, and a tools/call request for each
tool_use block. It writes what fyai sends to a log, one JSON object a line.

Usage: fake_claude.py SCENARIO LOG [CLI arguments...]

fyai removes the environment of the CLI, so a case makes a wrapper that
names the scenario file (JSON) and the log file, which is appended. See
fake_claude_install in harness.sh.

Scenario:
  {"account": {...},          # the initialize account; a subscription if absent
   "processes": [             # one entry for each start, in order
     {"turns": [...],         # one entry for each user message: {"replies", "result"}
      "replies": [            # or the replies of the one user message
        {"blocks": [{"text": "..."},
                    {"tool": "exec_command", "input": {...}, "id": "toolu_1"}],
         "fault": "duplicate" | "unknown" | "garbage" | "exit"}],
      "result": "error text"}]}   # end with an error result instead

A "duplicate" reply sends the tools/call of its first tool twice; "unknown"
sends one for an id that no reply made; "garbage" writes a line that is not
JSON; "exit" ends the process in the middle of the words of the reply and
"exit_in_tool" in the middle of a tool call; "crash" ends it after the
reply, while its calls run.
"""
import json
import os
import sys

SCENARIO, LOG = sys.argv[1], sys.argv[2]
ARGV = sys.argv[3:]
PREFIX = "mcp__fyai__"


def log(**entry):
    with open(LOG, "a") as f:
        f.write(json.dumps(entry) + "\n")


def send(frame):
    sys.stdout.write(json.dumps(frame) + "\n")
    sys.stdout.flush()


class Peer:
    def __init__(self):
        self.queue = []
        self.next_id = 0

    def read(self):
        if self.queue:
            return self.queue.pop(0)
        line = sys.stdin.readline()
        if not line:
            log(event="eof")
            sys.exit(0)
        return json.loads(line)

    def wait(self, pred):
        """The first frame that @pred takes; keep the others."""
        kept = []
        while True:
            frame = self.read()
            if pred(frame):
                self.queue = kept + self.queue
                return frame
            kept.append(frame)

    def request(self, message):
        """Send an MCP message to fyai; return the request id."""
        rid = "req-%d" % self.next_id
        self.next_id += 1
        send({"type": "control_request", "request_id": rid,
              "request": {"subtype": "mcp_message", "server_name": "fyai",
                          "message": message}})
        return rid

    def response(self, rid):
        frame = self.wait(lambda f: f.get("type") == "control_response" and
                          f["response"].get("request_id") == rid)
        return frame["response"]


def event(ev):
    send({"type": "stream_event", "event": ev, "parent_tool_use_id": None,
          "session_id": "fake"})


def reply(peer, r, n):
    blocks = r.get("blocks", [])
    fault = r.get("fault")
    event({"type": "message_start", "message": {
        "id": "msg_%d" % n, "type": "message", "role": "assistant",
        "model": "fake", "content": [],
        "usage": {"input_tokens": 3, "output_tokens": 0}}})
    calls = []
    for i, b in enumerate(blocks):
        if "text" in b:
            event({"type": "content_block_start", "index": i,
                   "content_block": {"type": "text", "text": ""}})
            event({"type": "content_block_delta", "index": i,
                   "delta": {"type": "text_delta", "text": b["text"]}})
            if fault == "garbage":
                sys.stdout.write("this is not json\n")
                sys.stdout.flush()
            if fault == "exit":
                sys.stderr.write("fake-claude: lost the connection\n")
                sys.stderr.flush()
                os._exit(3)
        else:
            tid = b.get("id", "toolu_%d_%d" % (n, i))
            event({"type": "content_block_start", "index": i,
                   "content_block": {"type": "tool_use", "id": tid,
                                     "name": PREFIX + b["tool"],
                                     "input": {}}})
            if fault == "exit_in_tool":
                event({"type": "content_block_delta", "index": i,
                       "delta": {"type": "input_json_delta",
                                 "partial_json": json.dumps(b["input"])[:5]}})
                sys.stderr.write("fake-claude: lost the connection\n")
                sys.stderr.flush()
                os._exit(3)
            event({"type": "content_block_delta", "index": i,
                   "delta": {"type": "input_json_delta",
                             "partial_json": json.dumps(b["input"])}})
            calls.append((tid, b))
        event({"type": "content_block_stop", "index": i})
    # The CLI asks for each call before the reply ends.
    pending = []
    for k, (tid, b) in enumerate(calls):
        ids = [tid, tid] if fault == "duplicate" and k == 0 else [tid]
        for t in ids:
            pending.append((t, peer.request({
                "jsonrpc": "2.0", "id": 100 + len(pending),
                "method": "tools/call",
                "params": {"name": b["tool"], "arguments": b["input"],
                           "_meta": {"claudecode/toolUseId": t}}})))
    if fault == "unknown":
        pending.append(("toolu_unknown", peer.request({
            "jsonrpc": "2.0", "id": 999, "method": "tools/call",
            "params": {"name": "exec_command", "arguments": {},
                       "_meta": {"claudecode/toolUseId": "toolu_unknown"}}})))
    event({"type": "message_delta",
           "delta": {"stop_reason": "tool_use" if calls else "end_turn"},
           "usage": {"output_tokens": 5}})
    event({"type": "message_stop"})
    if fault == "crash":
        # Gone while fyai runs the calls: their results find no CLI.
        sys.stderr.write("fake-claude: crashed\n")
        sys.stderr.flush()
        os._exit(3)
    for tid, rid in pending:
        resp = peer.response(rid)
        mcp = resp.get("response", {}).get("mcp_response", {})
        log(event="call", id=tid, subtype=resp.get("subtype"),
            result=mcp.get("result"), error=mcp.get("error"))


def main():
    with open(SCENARIO) as f:
        scenario = json.load(f)
    starts = 0
    if os.path.exists(LOG):
        with open(LOG) as f:
            starts = sum(1 for line in f if '"event": "start"' in line)
    procs = scenario.get("processes", [])
    proc = procs[min(starts, len(procs) - 1)]
    try:
        with open("/proc/%d/cmdline" % os.getppid(), "rb") as f:
            parent = f.read().split(b"\0")
        parent = [a.decode(errors="replace") for a in parent if a]
    except OSError:
        parent = []
    # The CLI keeps its login and state in its configuration directory.
    config = os.environ.get("CLAUDE_CONFIG_DIR") or \
        os.path.join(os.environ.get("HOME", "/nonexistent"), ".claude")
    writes = {}
    for name, path in (("config", os.path.join(config, ".fake-state")),
                       ("home", os.path.join(os.environ.get("HOME", "/"),
                                             ".fake-home-probe"))):
        try:
            with open(path, "w") as f:
                f.write("ok\n")
            writes[name] = "ok"
        except OSError as e:
            writes[name] = e.strerror
    log(event="start", pid=os.getpid(), argv=ARGV, cwd=os.getcwd(),
        parent=parent, config_dir=os.environ.get("CLAUDE_CONFIG_DIR"),
        writes=writes,
        api_key="ANTHROPIC_API_KEY" in os.environ)

    peer = Peer()
    init = peer.wait(lambda f: f.get("type") == "control_request")
    account = scenario.get("account", {
        "email": "user@example.com", "subscriptionType": "Claude Team",
        "apiProvider": "firstParty"})
    send({"type": "control_response", "response": {
        "subtype": "success", "request_id": init["request_id"],
        "response": {"account": account}}})

    rid = peer.request({"jsonrpc": "2.0", "id": 0, "method": "initialize",
                        "params": {"protocolVersion": "2025-11-25",
                                   "capabilities": {}}})
    peer.response(rid)
    peer.response(peer.request({"jsonrpc": "2.0",
                                "method": "notifications/initialized"}))
    tools = peer.response(peer.request({"jsonrpc": "2.0", "id": 1,
                                        "method": "tools/list"}))
    names = [t["name"] for t in
             tools["response"]["mcp_response"]["result"]["tools"]]
    log(event="tools", names=names)

    # A process serves one user turn, or each of its "turns" in order.
    for k, turn in enumerate(proc.get("turns", [proc])):
        user = peer.wait(lambda f: f.get("type") == "user")
        log(event="user", turn=k, content=user["message"]["content"])
        for n, r in enumerate(turn.get("replies", [])):
            reply(peer, r, n)
        if "result" in turn:
            send({"type": "result", "subtype": "error_during_execution",
                  "is_error": True, "result": turn["result"]})
        else:
            send({"type": "result", "subtype": "success", "is_error": False,
                  "result": "done"})
    user = peer.wait(lambda f: f.get("type") == "user")
    log(event="unexpected-user", content=user["message"]["content"])
    sys.exit(4)


if __name__ == "__main__":
    main()
