#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A stdio MCP server that refuses to shut down politely.

Speaks the same protocol as mock_mcp_stdio.py, but ignores SIGTERM and does
not exit when its stdin reaches EOF. That forces the client through the whole
shutdown escalation - close pipes, SIGTERM, SIGKILL - so the escalation is
exercised rather than only the fast path where the server leaves on EOF.
"""

import json
import signal
import sys
import time


signal.signal(signal.SIGTERM, signal.SIG_IGN)
signal.signal(signal.SIGINT, signal.SIG_IGN)

for line in sys.stdin:
    request = json.loads(line)
    method = request["method"]
    if method == "notifications/initialized":
        continue
    if method == "initialize":
        result = {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "mock-stubborn", "version": "1"},
        }
    elif method == "tools/list":
        result = {"tools": [{
            "name": "echo",
            "description": "Echo over stdio",
            "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}},
        }]}
    elif method == "tools/call":
        result = {"content": [{"type": "text", "text": "stdio: " +
                               request["params"]["arguments"]["text"]}]}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}),
          flush=True)

# EOF on stdin: a well-behaved server exits here. This one hangs around until
# it is killed.
while True:
    time.sleep(60)
