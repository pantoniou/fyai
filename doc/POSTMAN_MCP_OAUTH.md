# Using Postman MCP OAuth with `fyai`

Postman runs a hosted Streamable HTTP MCP server with OAuth:

```text
https://mcp.postman.com/minimal
```

Unlike providers that require a separately registered OAuth application,
Postman supports MCP dynamic client registration. `fyai` discovers and
registers its OAuth client automatically; no Postman API key, client JSON, or
client secret is required.

## Choose a tool set

Postman exposes three US-hosted endpoints:

| Endpoint | Tool set | Intended use |
| --- | --- | --- |
| `/minimal` | Minimal | Essential Postman workspace operations |
| `/code` | Code | API discovery and client-code generation |
| `/mcp` | Full | The complete Postman tool catalog |

Start with `/minimal`. Larger tool catalogs consume more model context and
grant the agent more capabilities. The EU endpoints currently support API-key
authentication only, not OAuth.

## Configure the server

Add the minimal endpoint and explicitly permit browser authorization:

```sh
./build/fyai config set mcp/servers/postman \
  '{endpoint: https://mcp.postman.com/minimal, '\
'auth: {type: oauth, allow_browser: true}}'
./build/fyai config set mcp/enabled true
```

Review the stored, non-secret configuration:

```sh
./build/fyai config get mcp/servers/postman
```

## Log in

Start an interactive session and request login:

```text
/mcp login postman
```

Alternatively, make a normal model request. The first MCP initialization
automatically starts OAuth when Postman returns its authorization challenge.

During login, `fyai`:

1. Fetches Postman's protected-resource metadata.
2. Fetches its OAuth authorization-server metadata.
3. Dynamically registers an MCP OAuth client.
4. Creates a PKCE verifier and loopback redirect listener.
5. Opens Postman's authorization page in the browser.
6. Exchanges the authorization code for access and refresh tokens.
7. Retries MCP initialization and discovers the selected tools.

Review the requested Postman access in the browser before approving it.

## Verify the connection

In an interactive session:

```text
/mcp
```

The Postman row should report `ready`, `authenticated`, and a nonzero tool
count. Start with a read-only request:

```text
List my Postman workspaces. Do not create or modify anything.
```

Then ask for the available collections in a workspace. Test write operations
only after read-only discovery works and the requested operation is understood.

## Change the tool set

Change the endpoint and restart `fyai`:

```sh
./build/fyai config set mcp/servers/postman/endpoint \
  https://mcp.postman.com/code
```

Use `https://mcp.postman.com/mcp` for the full catalog. An OAuth token is bound
to its target MCP resource. After changing endpoints, use `/mcp logout postman`
and `/mcp login postman` if the server requests a new grant.

## Tokens and logout

OAuth credentials are stored in fyai's machine-local authentication state,
outside the conversation arena. They are not written into repository config.

To remove the access and refresh tokens while retaining the server config:

```text
/mcp logout postman
```

To disable only Postman:

```sh
./build/fyai config set mcp/servers/postman/enabled false
```

To disable all MCP endpoints:

```sh
./build/fyai config set mcp/enabled false
```

## Troubleshooting

Use `/mcp` to inspect the live state and last endpoint error. Common causes:

- Browser login is disabled: set `auth/allow_browser` to `true`.
- The browser used the wrong Postman account: log out and repeat login.
- The selected Postman workspace does not grant the signed-in user access.
- An organization policy restricts OAuth applications or requested actions.
- The EU endpoint was selected: use an API key or the US OAuth endpoint.

Enable sanitized lifecycle logging when diagnosing connection state:

```sh
./build/fyai config set logging/mcp true
```

Logs do not include authorization headers, tokens, tool arguments, or response
bodies.

## References

- [Postman MCP server][postman-mcp]
- [MCP authorization specification][mcp-authorization]

[postman-mcp]:
  https://github.com/postmanlabs/postman-mcp-server
[mcp-authorization]:
  https://modelcontextprotocol.io/specification/draft/basic/authorization
