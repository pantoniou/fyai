#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Minimal persistent newline-framed MCP stdio server for functional tests."""

import json
import os
import sys
import time


log_path = os.environ["MCP_STDIO_LOG"]


def record(value):
    with open(log_path, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, separators=(",", ":")) + "\n")


def extra_fds():
    """Return open descriptors above standard error."""
    scan_fd = os.open("/dev/fd", os.O_RDONLY)
    try:
        names = os.listdir(scan_fd)
    finally:
        os.close(scan_fd)
    found = []
    for name in names:
        if not name.isdigit():
            continue
        fd = int(name)
        if fd <= 2 or fd == scan_fd:
            continue
        try:
            os.fstat(fd)
        except OSError:
            continue
        found.append(fd)
    return sorted(found)


for line in sys.stdin:
    request = json.loads(line)
    record({
        "request": request,
        "cwd": os.getcwd(),
        "env": os.environ.get("MCP_TEST_VALUE", ""),
        # What the child must not have inherited: a provider credential, and
        # any descriptor above the standard three.
        "secret": os.environ.get("OPENAI_API_KEY", ""),
        "extra_fds": extra_fds(),
    })
    method = request["method"]
    if method == "notifications/initialized":
        continue
    if method == "initialize":
        result = {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "mock-stdio", "version": "1"},
        }
    elif method == "tools/list":
        # A notification may arrive between request and response.
        print(json.dumps({"jsonrpc": "2.0", "method": "notifications/message",
                          "params": {"level": "info", "data": "ready"}}), flush=True)
        result = {"tools": [{
            "name": "echo",
            "description": "Echo over stdio",
            "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}},
        }]}
    elif method == "tools/call":
        time.sleep(float(os.environ.get("MCP_CALL_DELAY", "0")))
        result = {"content": [{"type": "text", "text": "stdio: " +
                                request["params"]["arguments"]["text"]}]}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}),
          flush=True)
    if method == "tools/list" and os.environ.get("MCP_EXIT_AFTER_LIST"):
        break

record({"eof": True})
