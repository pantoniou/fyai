#!/bin/bash
# SPDX-License-Identifier: MIT
# The request cache keeps the text of the conversation that a request sent and
# makes only the text of the new turns for the next one. A session of one
# process with chat completions sends the same requests with the cache and without
# it. The cache lasts for one exchange: the first request of a line makes the
# text of the conversation, and the next request of the same line uses it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
api=chat-completions
path=chat/completions
splits=yes
. "$TESTS_DIR/request_cache_session.sh"
pass
