#!/bin/bash
# SPDX-License-Identifier: MIT
# Config: import/export round-trip and $EDITOR-driven edit against the
# arena-resident config; raw secrets are rejected at every ingestion point.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

cat > import.yaml << 'EOF'
model: imported-model
temperature: 0.5
providers:
  mock:
    api: chat-completions
    api_key: { type: env, value: MOCK_KEY }
EOF

run_fyai config import import.yaml
assert_status 0

run_fyai config get model
assert_status 0
assert_stdout_contains "imported-model"

# export to a file and diff the YAML round-trip
run_fyai config export exported.yaml
assert_status 0
run_fyai config import exported.yaml
assert_status 0
run_fyai config export reexported.yaml
assert_status 0
diff exported.yaml reexported.yaml || fail "export/import round-trip drifted"

# edit: a scripted editor rewrites the model; the temp file is .yaml
cat > editor.sh << 'EOF'
#!/bin/sh
case "$1" in
*.yaml) ;;
*) echo "not a .yaml tempfile: $1" >&2; exit 1 ;;
esac
sed -i.bak 's/imported-model/edited-model/' "$1" && rm -f "$1.bak"
EOF
chmod +x editor.sh
EDITOR="$PWD/editor.sh" run_fyai config edit
assert_status 0
run_fyai config get model
assert_status 0
assert_stdout_contains "edited-model"

# an editor that fails leaves the config untouched
EDITOR="false" run_fyai config edit
assert_status_nonzero
run_fyai config get model
assert_stdout_contains "edited-model"

# import with a raw api_key must fail and must not change the config
cat > bad.yaml << 'EOF'
model: evil
api_key: sk-raw-secret
EOF
run_fyai config import bad.yaml
assert_status_nonzero
assert_stderr_contains "raw api_key"
run_fyai config get model
assert_stdout_contains "edited-model"

pass
