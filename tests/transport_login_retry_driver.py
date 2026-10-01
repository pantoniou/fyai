#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A 401 to a login request is retried at the transport.

Usage: transport_login_retry_driver.py FYAI_BIN MOCK_URL ARENA_DIR

The provider rejects the first request. While it holds that reply, the driver
stores another token, as a refresh by another process does. The transport then
runs the request again with the stored token, and the agent sees the second
reply only. The case needs no refresh and no network but the mock provider.
"""

import json
import os
import sys
import time

from transport_driver import Agent, END, Fail, Transport, chat, check, reply_text
from transport_login_driver import write_store


def main(fyai, mock_url, arena):
    run = os.path.dirname(os.path.dirname(arena))
    log = os.path.join(run, "requests.jsonl")
    url = mock_url + "/v1/chat/completions"
    tr = Transport(fyai, arena)
    try:
        tr.ok("init", level="level-b", log=False)
        write_store(3600, access="secret-old")
        tr.ok("profiles", profiles=[
            {"name": "main", "url": url, "auth": "bearer", "tag": "chat",
             "credential": "oauth:chatgpt"}])
        agent = Agent(tr, 7, [{"profile": "main"}])

        def renew():
            deadline = time.monotonic() + 60
            while not os.path.exists(log) or not open(log).read().strip():
                check(time.monotonic() < deadline, "the provider saw no request")
                time.sleep(0.01)
            write_store(3600, access="secret-new")
            open(os.path.join(run, "release"), "w").close()

        start, data, kind, _ = agent.call("main", chat("hi"), between=renew,
                                          content_type="application/json")
        check(kind == END and start["status"] == 200,
              "the agent saw %r" % (start,))
        check(reply_text(data) == "after retry", "wrong reply %r" % data)
        seen = [json.loads(l) for l in open(log)]
        check([r["auth"] for r in seen] ==
              ["Bearer secret-old", "Bearer secret-new"],
              "the provider saw %r" % [r["auth"] for r in seen])
        tr.ok("shutdown")
        check(tr.proc.wait(30) == 0, "transport did not exit cleanly")
    finally:
        if tr.proc.poll() is None:
            tr.proc.kill()
            tr.proc.wait()
    print("OK")


if __name__ == "__main__":
    try:
        main(*sys.argv[1:4])
    except Fail as e:
        print("FAIL: %s" % e)
        sys.exit(1)
