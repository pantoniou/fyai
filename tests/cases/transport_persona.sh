#!/bin/bash
# SPDX-License-Identifier: MIT
# A persona that selects a model of another provider runs under credential
# isolation. The user session states the profiles of every persona at the
# transport, and the sub-agent receives them in its grant: it states none.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

FYAI_TEST_TRANSPORT=level-b exec "$(dirname "$0")/agent_persona_provider.sh"
