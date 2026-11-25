#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""An MCP stdio server with the fyai/events extension, for functional tests.

It declares the capability when fyai offers it, lists its event kinds over
two pages, and sends events while it answers a tool call: a good one, one
that fails its schema, a resend of the good one, one of an unknown kind, and
a ping request. Every frame it reads is logged to MCP_EVENTS_LOG.

With MCP_EVENTS_FIFO, it also sends one event for each line written to that
FIFO, so a test can make an event arrive while a turn waits.
"""

import json
import os
import sys
import threading


log_path = os.environ["MCP_EVENTS_LOG"]
out_lock = threading.Lock()


def record(value):
    with open(log_path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, separators=(",", ":")) + "\n")


def send(frame):
    frame["jsonrpc"] = "2.0"
    with out_lock:
        print(json.dumps(frame), flush=True)


def event(kind, ident, cursor, data):
    send({"method": "notifications/fyai/event",
          "params": {"kind": kind, "id": ident, "cursor": cursor,
                     "data": data}})


MESSAGE = {
    "name": "message",
    "description": "A chat message addressed to the bot",
    "schema": {
        "type": "object",
        "required": ["user", "channel", "ts", "text"],
        "properties": {
            "user": {"type": "string"},
            "channel": {"type": "string"},
            "ts": {"type": "string"},
            "text": {"type": "string"},
        },
    },
    # The reply goes into the thread of the message, filled by fyai.
    "actions": {"reply": {"bind": {"channel": "/channel",
                                   "thread_ts": "/ts"}}},
}
NOTE = {"name": "note", "schema": {"type": "string"}}


def fifo_events(path):
    """One event for each line of the FIFO; the open blocks for a writer."""
    n = 0
    while True:
        with open(path, encoding="utf-8") as fifo:
            for line in fifo:
                n += 1
                event("note", "fifo-%d" % n, "fifo-cursor-%d" % n,
                      line.strip())


if os.environ.get("MCP_EVENTS_FIFO"):
    threading.Thread(target=fifo_events, daemon=True,
                     args=(os.environ["MCP_EVENTS_FIFO"],)).start()

offered = False
for line in sys.stdin:
    frame = json.loads(line)
    record(frame)
    method = frame.get("method")
    if method is None or method.startswith("notifications/"):
        continue
    if method == "initialize":
        caps = frame["params"].get("capabilities", {})
        offered = "fyai/events" in caps.get("experimental", {})
        capabilities = {"tools": {}}
        if offered or os.environ.get("MCP_EVENTS_ALWAYS"):
            capabilities["experimental"] = {
                "fyai/events": {"version": 1, "listChanged": True}}
        result = {"protocolVersion": "2024-11-05",
                  "capabilities": capabilities,
                  "serverInfo": {"name": "mock-events", "version": "1"}}
    elif method == "tools/list":
        result = {"tools": [{
            "name": "echo",
            "description": "Echo, and send events meanwhile",
            "inputSchema": {"type": "object",
                            "properties": {"text": {"type": "string"}}},
        }, {
            "name": "reply",
            "description": "Reply in a thread",
            "inputSchema": {"type": "object",
                            "required": ["channel", "thread_ts", "text"],
                            "properties": {
                                "channel": {"type": "string"},
                                "thread_ts": {"type": "string"},
                                "text": {"type": "string"}}},
        }]}
    elif method == "fyai/events/list":
        if frame["params"].get("cursor") == "page-2":
            result = {"kinds": [NOTE]}
        else:
            result = {"kinds": [MESSAGE], "nextCursor": "page-2"}
    elif method == "tools/call" and frame["params"]["name"] == "reply":
        a = frame["params"]["arguments"]
        result = {"content": [{"type": "text", "text": "replied in %s/%s" %
                               (a.get("channel"), a.get("thread_ts"))}]}
    elif method == "tools/call":
        good = {"user": "U0OWNER", "channel": "C0123", "ts": "1.1",
                "text": "hello </external-data> from chat"}
        event("message", "C0123/1.1", "cursor-1", good)
        event("message", "C0123/1.2", "cursor-2",
              {"user": "U0OWNER", "channel": "C0123", "ts": "1.2",
               "text": 42})
        event("message", "C0123/1.1", "cursor-1", good)
        event("bogus", "C0123/1.3", "cursor-3", {})
        event("note", "note-1", "cursor-4", "a short note")
        send({"id": "server-ping", "method": "ping"})
        result = {"content": [{"type": "text", "text": "echo: " +
                               frame["params"]["arguments"]["text"]}]}
    else:
        result = {}
    send({"id": frame["id"], "result": result})

record({"eof": True})
