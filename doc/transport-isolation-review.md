# Transport isolation patch review

Reviewed: patches on `devel` above `master` at `5be4f4c8`.

Status at `f96084b4`: findings 2, 3 and 4 are resolved. Finding 1 is resolved
for the agent image; the same-user store access remains a documented limit of
level B.

## Findings

### 1. Secret-backed API keys re-enter the agent process (P1)

**Status: resolved for the agent image.** `resolve_secret()` and the
automatic provider lookup do not read the secret store when
`fyai_transport_supervised()` is true (`src/core/fyai_config.c`). Level B installs
no filesystem or keyring policy, so a tool of the same user can still read
the store. Section 5 of `agent-transport-isolation-sdd.md` states this limit.

The bootstrap removes credential environment variables before it executes the
agent image. Configuration loading in that new image still resolves an
`api_key` secret reference with `fyai_secret_kernel_get()` and copies the value
into `cfg->api_key`. Automatic provider lookup does the same for
`fyai:api-key/<provider>`. As a result, an invocation can report transport
isolation while its agent holds the raw provider key.

The persistent keyring lookup is based on the user's UID. The patch does not
restrict the agent or its tools from making the same lookup. Environment
scrubbing does not protect credentials stored there.

Evidence: [`resolve_secret()`](../src/core/fyai_config.c#L313-L346),
[`fyai_config_apply()`](../src/core/fyai_config.c#L363-L383),
[automatic provider lookup](../src/core/fyai_config.c#L2921-L2935), and
[bootstrap environment scrubbing](../src/transport/fyai_transport_boot.c#L483-L519).

**Suggested fix:** In a supervised image, keep only the credential reference
in agent configuration. Resolve its value exclusively in the transport. Apply
an enforceable policy to the agent and tool processes so they cannot read the
same secret source directly. Add an end-to-end case using a secret-backed key
that checks the agent's memory-facing configuration and tool access.

### 2. A sub-agent control connection can change another execution's grant (P1)

**Status: resolved.** A secondary connection records its execution.
`op_grant()` accepts only that execution or a descendant of it, and only the
primary connection changes profiles, sets a credential, or starts and ends
the transport (`src/transport/fyai_transport_verb.c`).

Each sub-agent receives a control connection. The transport accepts `profiles`
from any such connection and applies them to the global profile set. It also
accepts `grant` with any execution ID. There is no check that the caller owns
the execution or that a merged profile preserves an existing endpoint and
credential source. A holder of a sub-agent control connection can therefore
replace a profile used by the root or change the root's grant.

Evidence: [control dispatch](../src/transport/fyai_transport_verb.c#L496-L553),
[`op_grant()`](../src/transport/fyai_transport_verb.c#L464-L476),
[`op_profiles()`](../src/transport/fyai_transport_verb.c#L478-L494), and
[profile merge replacement](../src/transport/fyai_transport.c#L493-L521).

**Suggested fix:** Associate each secondary control connection with its
registered execution. Restrict admissions and grant changes to that
execution's descendants. Reject an attempt to replace an existing profile's
URL, authentication, or credential source from a secondary connection. Test
cross-execution grant and profile changes through the control protocol.

### 3. Catalogue updates move raw credentials into the agent environment (P1)

**Status: resolved.** `op_envgrant()` writes the values into a socket that
only the command child reads; the agent does not hold them. Only the primary
connection can ask for the grant (`src/transport/fyai_transport_verb.c`,
`src/provider/fyai_catalog.c`).

For a catalogue update, the agent asks the transport to `fetch` the configured
environment variables. The transport returns their values, and
`fyai_transport_fetch_env()` installs them in the agent process with `putenv()`
before launching the configured command. The synchronous and interactive
catalogue update paths both do this. During that interval, the agent holds
the raw values; the launched command inherits them. This violates the stated
guarantee that the transport alone holds provider credentials.

Evidence: [`op_fetch()`](../src/transport/fyai_transport_verb.c#L390-L411),
[`fyai_transport_fetch_env()`](../src/transport/fyai_transport_boot.c#L797-L841),
[synchronous catalogue update](../src/provider/fyai_catalog.c#L743-L751), and
[interactive catalogue update](../src/provider/fyai_catalog.c#L845-L855).

**Suggested fix:** Run this credential-bearing command from a distinct,
explicitly trusted process, or make catalogue updates use transport-mediated
requests. Do not return credential values over an agent control connection.
Cover both update paths with a test that checks the agent process while the
command is running.

### 4. `level-a` and `auto` do not behave as documented (P2)

**Status: resolved in the documentation.** The schema states that `level-b`
is the only level that runs, that `level-a` stops the invocation, and that
`auto` takes `level-b`.

Bootstrap always resolves `auto` to `level-b` and rejects every explicit
`level-a` request. The configuration schema and design document say that
`level-a` is selectable and that `auto` chooses the highest supported level.
On a host with usable cgroup v2 delegation, neither behavior occurs.

Evidence: [bootstrap level selection](../src/transport/fyai_transport_boot.c#L422-L429),
[configuration schema](../data/config.schema.yaml#L428-L440), and
[level definitions](agent-transport-isolation-sdd.md#L255-L270).

**Suggested fix:** Implement and verify cgroup placement before exposing
`level-a`, then probe it for `auto`. Until then, document that `level-b` is the
only supported level and make the schema description match.

## Verification

`cmake --build build -j4` succeeded. The focused check
`ctest --test-dir build -R 'transport|redact' --output-on-failure -j4` passed
all 69 tests. The findings above come from code-path review; those tests do
not cover the credential and cross-execution control cases described here.
