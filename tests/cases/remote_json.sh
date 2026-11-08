#!/bin/bash
# SPDX-License-Identifier: MIT
# The ends of a remote session that are both fyai speak images; with images
# off they speak JSON, as an SDK client does. A driver and a watcher run the
# same in both.
set -eu
export FYAI_REMOTE_NO_IMAGE=1
exec "$(dirname "$0")/remote_watch.sh"
