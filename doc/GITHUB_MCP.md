# Using the GitHub MCP Server with `fyai`

This guide connects `fyai` to GitHub's hosted MCP server using a
fine-grained personal access token (PAT). Start with the read-only endpoint so
that the initial test cannot modify repositories, issues, or pull requests.

GitHub's hosted endpoint is `https://api.githubcopilot.com/mcp/`. Appending
`readonly` restricts the advertised tools to read operations. See the
[GitHub remote MCP documentation](https://github.com/github/github-mcp-server/blob/main/docs/remote-server.md).

## 1. Create a fine-grained GitHub token

Open [GitHub's fine-grained token creation page](https://github.com/settings/personal-access-tokens/new)
and configure:

- Token name: `fyai-mcp`
- Expiration: a short period, such as 30 days
- Resource owner: your GitHub account or organization
- Repository access: public repositories for an initial test, or only the
  selected private repositories that `fyai` should access
- Repository permissions:
  - Contents: Read-only
  - Issues: Read-only
  - Pull requests: Read-only
  - Actions: Read-only, if workflow information is needed
  - Metadata is granted automatically

GitHub recommends fine-grained tokens where possible. Treat the token like a
password. See [GitHub's PAT documentation](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/managing-your-personal-access-tokens).

## 2. Export the token

Never put the raw token in `config.yaml`. To avoid recording it in shell
history, read it interactively:

```sh
read -rsp "GitHub MCP token: " GITHUB_MCP_PAT
echo
export GITHUB_MCP_PAT
```

The environment variable must be present whenever `fyai` starts.

## 3. Configure `fyai`

Run these commands from an initialized repository:

```sh
./build/fyai config set mcp/servers/github \
  '{endpoint: "https://api.githubcopilot.com/mcp/readonly", auth_token: {type: env, value: GITHUB_MCP_PAT}}'

./build/fyai config set mcp/enabled true
```

Verify the effective configuration. This displays the environment-variable
reference, never the token itself:

```sh
./build/fyai config get mcp
```

The result should contain values equivalent to:

```yaml
enabled: true
servers:
  github:
    endpoint: https://api.githubcopilot.com/mcp/readonly
    auth_token: {type: env, value: GITHUB_MCP_PAT}
```

`fyai` supports multiple named Streamable HTTP servers. Each enabled entry in
`mcp.servers` owns an independent persistent HTTP connection, MCP session and
request sequence. For example, add another server without replacing GitHub:

```sh
./build/fyai config set mcp/servers/internal \
  '{endpoint: "https://mcp.example.com/", auth_token: {type: env, value: INTERNAL_MCP_TOKEN}, timeout: 60}'
```

Set `enabled: false` in an individual server mapping to retain its
configuration without connecting to it. The older top-level `mcp.endpoint`,
`mcp.auth_token`, and related keys remain supported as a single server named
`default` when `mcp.servers` is empty.

## 4. Run read-only tests

Replace the model name if necessary:

```sh
./build/fyai -m gpt-5.4 \
  "Use the GitHub MCP tools to describe the repository github/github-mcp-server. Include its description and default branch."
```

Test a private repository:

```sh
./build/fyai -m gpt-5.4 \
  "Use GitHub MCP to list the five most recent open issues in OWNER/REPOSITORY."
```

Test pull-request access:

```sh
./build/fyai -m gpt-5.4 \
  "Use GitHub MCP to summarize open pull requests in OWNER/REPOSITORY. Do not modify anything."
```

## 5. Use MCP interactively

Start an interactive session:

```sh
./build/fyai -i
```

Inspect the connection and issue a request:

```text
/mcp show
Using GitHub MCP, show my open pull requests in OWNER/REPOSITORY.
```

Discovered tools are namespaced internally, for example:

```text
mcp__github__get_file_contents
mcp__github__issue_read
mcp__github__pull_request_read
```

Prompts normally do not need to mention these internal names.

## 6. Enable write operations deliberately

After read-only access works, switch to the default endpoint:

```sh
./build/fyai config set mcp/servers/github/endpoint \
  '"https://api.githubcopilot.com/mcp/"'
```

Grant the PAT only the write permissions required by the intended operations.
For example, set Issues to Read and write only if `fyai` must create or update
issues. Prefer the `/readonly` endpoint for normal use.

## Troubleshooting

Enable MCP lifecycle and request-metadata logging with:

```sh
./build/fyai config set logging/mcp true
```

In an interactive session, it can also be controlled with:

```text
/log mcp start
/log mcp stop
/log mcp clear
/log mcp view
```

The YAML log is stored at `.fyai/logs/mcp.yaml`. It records connection reuse,
JSON-RPC methods and IDs, HTTP status, elapsed time, discovery counts, and tool
names, including the server name for each event. Authorization values, request
arguments, and response bodies are not logged.

Confirm that the token exists in the current shell without printing it:

```sh
test -n "$GITHUB_MCP_PAT" && echo "token is set"
```

Inspect the non-secret configuration:

```sh
./build/fyai config get mcp/servers/github/endpoint
./build/fyai config get mcp/servers/github/auth_token
./build/fyai config get mcp/enabled
```

An HTTP `401` usually means that the token is absent, expired, or rejected.
Export it in the same shell that runs `fyai`.

If a private repository is missing, verify that:

- The PAT includes that repository.
- Any required organization approval is complete.
- The required repository permissions are enabled.
- Any applicable SAML SSO authorization is satisfied.

Disable MCP persistently with:

```sh
./build/fyai config set mcp/enabled false
```

Or disable it for the active interactive session with:

```text
/mcp off
```

## Current scope and implementation priorities

Named Streamable HTTP servers and text tool results are supported. Remaining
MCP work, in priority order, is:

1. Integrate server and per-tool allow, deny, and approval modes with tool
   execution.
2. Complete the Streamable HTTP lifecycle: list pagination, session recovery,
   shutdown, reconnect/backoff, and GET/SSE notifications.
3. Add the stdio transport for locally launched servers.
4. Handle `isError`, structured output, resource links, and non-text tool
   content.
5. Add arbitrary HTTP headers and OAuth authentication.
6. Expose MCP resources and prompts.
7. Load tool schemas lazily when many servers are configured and extend
   `/mcp` with server management and detailed status.
