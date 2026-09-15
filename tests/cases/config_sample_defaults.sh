#!/bin/bash
# SPDX-License-Identifier: MIT
# config.yaml.sample is the default configuration: fyai init starts a project
# from it, and it says that every key holds its compiled-in default. Each key of
# the configuration schema is in it, set or commented out, and a key set in it
# holds the default of the schema. A new key or a changed default that is not
# added to the sample fails here.
set -eu
. "$(dirname "$0")/../harness.sh"

"$PYTHON" - "$TESTS_DIR/../config.yaml.sample" \
    "$TESTS_DIR/../data/config.schema.yaml" <<'PY' ||
import re
import sys

import yaml

sample_text = open(sys.argv[1], encoding="utf-8").read()
sample = yaml.safe_load(sample_text)
schema = yaml.safe_load(open(sys.argv[2], encoding="utf-8"))

# Derived, secret or example values that the sample sets on purpose.
EXEMPT = ("catalog", "api_key/", "mcp/auth_token/", "model", "agent/personas")

leaves = []


def walk(node, path):
    for key, value in (node.get("properties") or {}).items():
        path_key = path + [key]
        if value.get("properties"):
            walk(value, path_key)
        else:
            leaves.append(("/".join(path_key), value))


walk(schema, [])
problems = []
for key, spec in leaves:
    if key.startswith(EXEMPT):
        continue
    node, present = sample, True
    for part in key.split("/"):
        if not isinstance(node, dict) or part not in node:
            present = False
            break
        node = node[part]
    if not present:
        leaf = key.split("/")[-1]
        if not re.search(r"(?m)^\s*#\s*%s:" % re.escape(leaf), sample_text):
            problems.append("%s is not in the sample" % key)
        continue
    if "default" in spec and not isinstance(node, dict) and node != spec["default"]:
        problems.append("%s is %r in the sample, the default is %r"
                        % (key, node, spec["default"]))
if problems:
    raise SystemExit("\n".join(problems))
PY
    fail "config.yaml.sample does not match the configuration schema"

pass
