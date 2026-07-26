# MCP OAuth

## Overview

`fyai` supports OAuth 2.0 authorization-code login for remote MCP servers. The
implementation uses PKCE and a loopback redirect receiver. OAuth operations use
the application event loop.

Browser login is disabled unless the server configuration contains:

```yaml
auth:
  type: oauth
  allow_browser: true
```

## Discovery

For a protected MCP resource, `fyai` performs these operations:

1. Read the HTTP authentication challenge.
2. Get the protected-resource metadata.
3. Get the authorization-server metadata.
4. Select a configured client or register a dynamic client.
5. Start the loopback receiver.
6. Open the authorization URL.
7. Exchange the authorization code.
8. Retry the parked MCP request.

Each operation has explicit completion, cancellation, and cleanup state.

## Clients

A configured client has a stable redirect URI. It remains configured after
logout.

A dynamic client is bound to the loopback URI that was used for registration.
Logout removes it from memory. A later login registers a new client for the new
receiver URI.

Use `fyai mcp oauth import-client` to import a downloaded client document. The
command stores non-secret fields in the configuration. It stores the client
secret in the local credential store or records an environment-variable
reference.

## Credentials

Credentials are stored per MCP server in the private fyai state directory.
Updates use the shared authorization lock and atomic file replacement. Raw
client secrets and tokens are not stored in the arena configuration.

Before use, `fyai` refreshes a token that is near expiry. It retries one
authenticated request after an HTTP 401 response. A timer starts refresh five
minutes before expiry.

Explicit login or logout cancels an active discovery, refresh, or browser wait.
Each terminal path releases the discovery state.

## Loopback receiver

The receiver binds to `127.0.0.1`. It accepts multiple connections because a
browser can request icons or open speculative connections. A request for an
unrelated path gets HTTP 404 and does not stop the login.

The receiver checks the OAuth state value. It reports a bad state, timeout, or
transport failure as a terminal error.

## Tests

The test suite covers:

- configured and dynamic clients;
- client import and secret references;
- login, refresh, logout, and cancellation;
- token rotation and HTTP 401 recovery; and
- loopback receiver success and error paths.
