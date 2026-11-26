#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""An MCP server over HTTP with the fyai/events extension, for tests.

It answers POSTs, and sends its events on the stream that fyai opens with a
GET. A call of the tool `echo` puts on the stream: a good event, one that
fails its schema and a ping; then it closes the stream. On the stream that
fyai opens again it sends another event and a second ping, and the answer of
the call, a stream of events itself, waits for the answer to that ping and
carries one more event before the result. Each frame it reads and each GET
are logged to MCP_EVENTS_LOG. It writes its port to MCP_HTTP_PORT, a FIFO.

With MCP_HTTP_NO_STREAM, a GET is refused with 405.
"""

import json
import os
import queue
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

log_path = os.environ["MCP_EVENTS_LOG"]
log_lock = threading.Lock()
stream = queue.Queue()
pong = threading.Event()
CLOSE = object()

MESSAGE = {
    "name": "message",
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
    "principal": "/user",
}
NOTE = {"name": "note", "schema": {"type": "string"}}


def record(value):
    with log_lock, open(log_path, "a", encoding="utf-8") as out:
        out.write(json.dumps(value, separators=(",", ":")) + "\n")


def event(kind, ident, cursor, data):
    return {"jsonrpc": "2.0", "method": "notifications/fyai/event",
            "params": {"kind": kind, "id": ident, "cursor": cursor,
                       "data": data}}


def sse(frame, ident=None):
    head = "id: %s\n" % ident if ident else ""
    return ("%sevent: message\ndata: %s\n\n" %
            (head, json.dumps(frame))).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, body=b"", ctype="application/json", extra=()):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        record({"get": True,
                "accept": self.headers.get("Accept"),
                "session": self.headers.get("Mcp-Session-Id"),
                "last_event_id": self.headers.get("Last-Event-ID")})
        if os.environ.get("MCP_HTTP_NO_STREAM"):
            self.reply(405, b"")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(b": open\n\n")
        self.wfile.flush()
        while True:
            item = stream.get()
            if item is CLOSE:
                return
            self.wfile.write(item)
            self.wfile.flush()

    def do_DELETE(self):
        record({"delete": True})
        self.reply(200)

    def do_POST(self):
        frame = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        record(frame)
        method = frame.get("method")
        if frame.get("id") == "server-ping-2":
            pong.set()
        if method is None or "id" not in frame:
            self.reply(202)
            return
        extra = ()
        if method == "initialize":
            result = {"protocolVersion": "2025-06-18",
                      "capabilities": {"tools": {}, "experimental": {
                          "fyai/events": {"version": 1}}},
                      "serverInfo": {"name": "mock-events-http",
                                     "version": "1"}}
            extra = (("Mcp-Session-Id", "sess-1"),)
        elif method == "tools/list":
            result = {"tools": [{
                "name": "echo", "description": "Echo, and send events",
                "inputSchema": {"type": "object",
                                "properties": {"text": {"type": "string"}}},
            }]}
        elif method == "fyai/events/list":
            result = {"kinds": [MESSAGE, NOTE]}
        elif method == "tools/call":
            self.call(frame)
            return
        else:
            result = {}
        self.reply(200, json.dumps({"jsonrpc": "2.0", "id": frame["id"],
                                    "result": result}).encode(),
                   extra=extra)

    def call(self, frame):
        good = {"user": "U0OWNER", "channel": "C0123", "ts": "1.1",
                "text": "hello over http"}
        stream.put(sse(event("message", "C0123/1.1", "cursor-1", good), 1))
        stream.put(sse(event("message", "C0123/1.2", "cursor-2",
                             {"user": "U0OWNER", "channel": "C0123",
                              "ts": "1.2", "text": 42}), 2))
        stream.put(sse({"jsonrpc": "2.0", "id": "server-ping",
                        "method": "ping"}, 3))
        stream.put(CLOSE)
        # On the stream that opens again.
        stream.put(sse(event("note", "note-2", "cursor-4",
                             "after the stream opened again"), 4))
        stream.put(sse({"jsonrpc": "2.0", "id": "server-ping-2",
                        "method": "ping"}, 5))
        # The answer comes once the second stream was served.
        if not pong.wait(60):
            record({"timeout": "server-ping-2"})
        body = sse(event("note", "note-3", "cursor-5", "within the answer"))
        body += sse({"jsonrpc": "2.0", "id": frame["id"], "result": {
            "content": [{"type": "text", "text": "echo: " +
                         frame["params"]["arguments"]["text"]}]}})
        self.reply(200, body, ctype="text/event-stream")


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
with open(os.environ["MCP_HTTP_PORT"], "w") as port:
    port.write("%d\n" % server.server_address[1])
server.serve_forever()
