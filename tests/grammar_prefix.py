#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that the requests of a conversation keep the prefix a provider caches.

grammar_prefix.py REQUESTS SCENARIO FROM TO

Requests 0 and 1 are a tool turn of grammar FROM; requests 2 and 3 are two
later turns of grammar TO. Each request must start with the request before it
in the same grammar, and the tool turn must go back as the mock sent it.
"""

import json
import sys


def history(req, grammar):
    body = req["body"]
    return body["input"] if grammar == "responses" else body["messages"]


def uncached(messages):
    """The moving Messages cache marker is not a part of the prefix."""
    out = []
    for m in messages:
        m = dict(m)
        if isinstance(m.get("content"), list):
            m["content"] = [{k: v for k, v in b.items() if k != "cache_control"}
                            if isinstance(b, dict) else b
                            for b in m["content"]]
        out.append(m)
    return out


def expect_prefix(what, a, b):
    """Every message of a goes again in b; the last can gain blocks."""
    if len(b) < len(a):
        sys.exit("%s: the next request is shorter" % what)
    for i, m in enumerate(a):
        if m == b[i]:
            continue
        last = i == len(a) - 1
        ca, cb = m.get("content"), b[i].get("content")
        if (last and isinstance(ca, list) and isinstance(cb, list) and
                m.get("role") == b[i].get("role") and cb[:len(ca)] == ca):
            continue
        sys.exit("%s: message %d changed:\n%s\n%s" %
                 (what, i, json.dumps(m), json.dumps(b[i])))


def sent_turn(step, grammar):
    """The tool turn of the mock, in the shape the grammar sends it back."""
    reply = step["response"]
    if grammar == "responses":
        return reply["output"]
    if grammar == "chat-completions":
        msg = reply["choices"][0]["message"]
        return [{k: msg[k] for k in ("role", "content", "tool_calls")}]
    return [{"role": "assistant", "content": reply["content"]}]


def main():
    path, scenario, src, dst = sys.argv[1:5]
    reqs = [json.loads(line) for line in open(path)]
    steps = json.load(open(scenario))["steps"]
    if len(reqs) != 4:
        sys.exit("expected 4 requests, got %d" % len(reqs))

    h0 = uncached(history(reqs[0], src))
    h1 = uncached(history(reqs[1], src))
    expect_prefix(src, h0, h1)

    want = sent_turn(steps[0], src)
    got = h1[len(h0):len(h0) + len(want)]
    if got != want:
        sys.exit("%s: the tool turn is not sent as it came:\n%s\n%s" %
                 (src, json.dumps(want), json.dumps(got)))

    expect_prefix(dst, uncached(history(reqs[2], dst)),
                  uncached(history(reqs[3], dst)))


main()
