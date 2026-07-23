#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scenario-driven mock AI provider for the fyai test suite.

Serves canned replies for the OpenAI Chat Completions and Responses APIs
(and the Anthropic Messages API, ready for when fyai grows that client).

Usage: mock_provider.py <scenario.json> [<rundir>]

The scenario file is a JSON object:

    {
      "steps": [ <step>, <step>, ... ]
    }

Each step describes the reply to one request, in order of arrival:

    {
      "status": 200,                 # optional HTTP status (default 200)
      "delay": 2.0,                  # optional seconds to wait before replying
                                     # (exercises the ^C-abort / wait path)
      "response": { ... },           # buffered JSON body, or
      "sse": [ {...}, {...} ],       # SSE events, each emitted as data: <json>
      "done": true,                  # append a final "data: [DONE]" line
      "raw": "verbatim body",        # or a verbatim (possibly invalid) body
      "raw_sse": ["data: junk", ""], # or verbatim SSE lines
      "chunk_split": [5, 17],        # byte offsets to split the SSE stream at
      "close_after": 3               # emit only the first N sse events, then
                                     # close the connection (truncated stream)
    }

The run directory (default: cwd) receives:
    port           - the chosen listen port, written once bound
    requests.jsonl - one JSON object per request: path, auth, body
    served         - running count of requests served
"""

import json
import os
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


class QuietHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    """HTTPServer without the reverse-DNS lookup on bind.

    HTTPServer.server_bind() calls socket.getfqdn() on the bind address to
    set self.server_name. On some hosts (observed on macOS GitHub Actions
    runners) that reverse lookup for 127.0.0.1 stalls for ~15s, blowing past
    the test harness's few-second wait for the "port" file. server_name is
    only used for logging here, so skip the lookup and use the raw address.
    """

    daemon_threads = True

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = host
        self.server_port = port


class MockState:
    def __init__(self, scenario, rundir):
        self.steps = scenario.get("steps", [])
        self.rundir = rundir
        self.lock = threading.Lock()
        self.served = 0


STATE = None


def sse_payload(step):
    """Build the full SSE byte stream for a step."""
    out = []
    events = step.get("sse", [])
    close_after = step.get("close_after")
    if close_after is not None:
        events = events[:close_after]
    for ev in events:
        if isinstance(ev, dict) and "type" in ev:
            out.append("event: %s\n" % ev["type"])
        out.append("data: %s\n\n" % json.dumps(ev, separators=(",", ":")))
    for line in step.get("raw_sse", []):
        out.append(line + "\n")
    if step.get("done") and close_after is None:
        out.append("data: [DONE]\n\n")
    return "".join(out).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("mock: " + fmt % args + "\n")

    def do_POST(self):
        st = STATE
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = None

        with st.lock:
            idx = st.served
            st.served += 1
            step = st.steps[idx] if idx < len(st.steps) else None
            record = {
                "path": self.path,
                "auth": self.headers.get("Authorization", ""),
                "x_api_key": self.headers.get("x-api-key", ""),
                "anthropic_version": self.headers.get("anthropic-version", ""),
                "mcp_session_id": self.headers.get("Mcp-Session-Id", ""),
                "mcp_protocol_version": self.headers.get("MCP-Protocol-Version", ""),
                "client_port": self.client_address[1],
                "content_type": self.headers.get("Content-Type", ""),
                "body": parsed if parsed is not None else body,
            }
            with open(os.path.join(st.rundir, "requests.jsonl"), "a") as f:
                f.write(json.dumps(record) + "\n")
            with open(os.path.join(st.rundir, "served"), "w") as f:
                f.write("%d\n" % st.served)

        # Optional pre-response delay (seconds), for interrupt/timeout tests:
        # the client waits with no body byte yet, exercising the ^C abort path.
        delay = step.get("delay") if step else None
        if delay:
            time.sleep(float(delay))

        if step is None:
            payload = b'{"error":{"message":"mock: scenario exhausted"}}'
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        status = step.get("status", 200)

        if "sse" in step or "raw_sse" in step:
            payload = sse_payload(step)
            truncated = step.get("close_after") is not None
            self.send_response(status)
            self.send_header("Content-Type", "text/event-stream")
            if truncated:
                # advertise more than we send, then drop the connection
                self.send_header("Content-Length", str(len(payload) + 64))
            else:
                self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            splits = step.get("chunk_split", [])
            chunk_delay = float(step.get("chunk_delay", 0))
            pos = 0
            for split in splits:
                if split <= pos or split >= len(payload):
                    continue
                self.wfile.write(payload[pos:split])
                self.wfile.flush()
                pos = split
                if chunk_delay:
                    time.sleep(chunk_delay)
            self.wfile.write(payload[pos:])
            self.wfile.flush()
            if truncated:
                self.close_connection = True
                self.wfile.flush()
                try:
                    self.connection.shutdown(1)
                except OSError:
                    pass
            return

        if "raw" in step:
            payload = step["raw"].encode()
        else:
            payload = json.dumps(step.get("response", {}),
                                 separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        for key, value in step.get("headers", {}).items():
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_DELETE(self):
        st = STATE
        with st.lock:
            idx = st.served
            st.served += 1
            step = st.steps[idx] if idx < len(st.steps) else None
            record = {
                "path": self.path,
                "method": "DELETE",
                "auth": self.headers.get("Authorization", ""),
                "mcp_session_id": self.headers.get("Mcp-Session-Id", ""),
                "mcp_protocol_version": self.headers.get("MCP-Protocol-Version", ""),
                "client_port": self.client_address[1],
                "body": "",
            }
            with open(os.path.join(st.rundir, "requests.jsonl"), "a") as f:
                f.write(json.dumps(record) + "\n")
            with open(os.path.join(st.rundir, "served"), "w") as f:
                f.write("%d\n" % st.served)

        if step is None:
            status = 500
            payload = b'{"error":{"message":"mock: scenario exhausted"}}'
        else:
            status = step.get("status", 200)
            payload = step.get("raw", "").encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    global STATE
    if len(sys.argv) < 2:
        sys.exit("usage: mock_provider.py <scenario.json> [<rundir>]")
    with open(sys.argv[1]) as f:
        scenario = json.load(f)
    rundir = sys.argv[2] if len(sys.argv) > 2 else "."

    STATE = MockState(scenario, rundir)

    server = QuietHTTPServer(("127.0.0.1", 0), Handler)
    port_path = os.path.join(rundir, "port")
    with open(port_path + ".tmp", "w") as f:
        f.write("%d\n" % server.server_address[1])
    os.rename(port_path + ".tmp", port_path)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
