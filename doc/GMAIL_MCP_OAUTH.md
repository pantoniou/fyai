# Using Gmail MCP OAuth with `fyai`

This guide connects `fyai` directly to Google's hosted Gmail MCP server:

```text
https://gmailmcp.googleapis.com/mcp/v1
```

Google publishes normal MCP protected-resource and authorization-server
metadata, but does not offer Dynamic Client Registration. Import a Google
OAuth client before connecting.

## Prerequisites

1. Create a Google Cloud project.
2. Join the Google Workspace Developer Preview Program.
3. Register the Workspace account and Cloud project with that program.
4. Wait for Google's confirmation that the project is allowlisted.
5. Enable the Gmail API.
6. Enable the Gmail MCP API.
7. Configure the OAuth consent screen and intended audience.
8. Create an OAuth client.
9. Download its client JSON.
10. Add the necessary test users while the application is in testing.

Gmail MCP is currently a Developer Preview feature. Enabling its APIs is not
enough: Google must allowlist the calling Cloud project and Workspace account.
The [Developer Preview application][workspace-preview] requires an account in
a Google Workspace domain. It does not accept personal `@gmail.com` accounts,
service accounts, or Google Groups. Google says approval normally takes a
couple of days.

The Gmail API and Gmail MCP API are separate services. Both must be enabled
in the project that owns the OAuth client. With the Google Cloud CLI:

```sh
gcloud services enable gmail.googleapis.com \
  gmailmcp.googleapis.com --project PROJECT_ID
```

The same services can be enabled in the Google Cloud console. Enabling only
the Gmail API permits OAuth login but causes every MCP tool call to fail with
HTTP 403. Google may take a few minutes to propagate a newly enabled service.
See Google's [Gmail MCP configuration guide][gmail-mcp-setup].

Prefer the least-privileged Gmail scope. A server that only reads messages
should not request mail modification or full mailbox access. Google's current
scope list is documented in [Choose Gmail API scopes][gmail-scopes].

Public applications using sensitive or restricted Gmail scopes may require
Google verification. Applications that store or transmit restricted Gmail
data through a server may also require a security assessment. See
[Google's restricted-scope verification guide][restricted-verification].

## Import and configure the client

Choose a short server name. Import the downloaded JSON:

```sh
./build/fyai mcp oauth import-client google-mail client_secret.json \
  --endpoint https://gmailmcp.googleapis.com/mcp/v1 \
  --scope https://www.googleapis.com/auth/gmail.readonly
```

This stores only the client secret in fyai's machine-local secret backend.
The source JSON is not retained or modified. It may be removed after import.

To keep the secret in an environment variable instead:

```sh
./build/fyai mcp oauth import-client google-mail client_secret.json \
  --endpoint https://gmailmcp.googleapis.com/mcp/v1 \
  --scope https://www.googleapis.com/auth/gmail.readonly \
  --secret-env GOOGLE_GMAIL_MCP_CLIENT_SECRET
```

The environment form records only the variable name. It does not set or verify
the variable. Export it before starting fyai.

Review the resulting non-secret configuration, then explicitly enable browser
authorization and MCP:

```sh
./build/fyai config get mcp/servers/google-mail
./build/fyai config set \
  mcp/servers/google-mail/auth/allow_browser true
./build/fyai config set mcp/enabled true
```

In an interactive session, inspect and control the live endpoint with:

```text
/mcp
/mcp login google-mail
/mcp logout google-mail
```

The status table reports the connection and OAuth states, discovered tool
count, token expiry, endpoint, and the last endpoint error. Login runs
asynchronously on the application event loop. Logout removes access and
refresh tokens without deleting the imported client secret.

## First login

Start with a read-only request:

```sh
./build/fyai -m gpt-5.4 \
  "Using Google Mail MCP, list the subjects and senders of my five newest \
unread messages. Do not modify anything."
```

On the first connection, `fyai`:

1. Sends the MCP `initialize` request.
2. Receives an HTTP `401` challenge.
3. Fetches protected-resource and authorization-server metadata.
4. Uses the imported client with a PKCE loopback callback.
5. Prints the authorization URL and opens it in the desktop browser.
6. Exchanges the returned authorization code for tokens.
7. Stores access and refresh tokens privately.
8. Retries MCP initialization with the access token.
9. Discovers the Gmail tools and continues the model turn.

Review the account, application identity, and requested Gmail permissions in
the browser before approving them. If Google shows an unverified-application
warning, confirm that the deployment is an expected development or personal
instance before proceeding.

## Credential storage and refresh

MCP OAuth credentials are stored outside the content-addressed arena:

```text
$XDG_STATE_HOME/fyai/mcp-auth-google-mail.json
```

When `XDG_STATE_HOME` is unset, the default is:

```text
~/.local/state/fyai/mcp-auth-google-mail.json
```

The file is mode `0600`, is replaced atomically, and is guarded by the shared
fyai authentication lock. Configured client secrets are not copied into it.

`fyai` refreshes MCP access tokens:

- Before use when a stored token is near expiry.
- Five minutes before expiry while the application event loop is running.
- Once after an authenticated MCP request returns HTTP `401`.

Rotated refresh tokens are saved immediately. A failed background refresh is
retried after 60 seconds. If refresh is permanently rejected, a later
connection requires browser authorization again.

## Verify read-only operation

After login, try requests whose intent is easy to inspect:

```sh
./build/fyai -m gpt-5.4 \
  "Use Google Mail MCP to count unread messages. Do not mark them read."
```

```sh
./build/fyai -m gpt-5.4 \
  "Use Google Mail MCP to find messages from example.com received this week. \
Return subject, sender, and date only."
```

Internally, discovered tools are namespaced with the configured server name:

```text
mcp__google-mail__search_messages
mcp__google-mail__get_message
```

The exact suffixes depend on the Gmail MCP server. Prompts normally do not need
to mention the internal names.

Enable send, modify, delete, or label-management tools only after read-only
access works and the Google scopes match the intended operations.

## Logging and troubleshooting

Enable sanitized MCP lifecycle logging:

```sh
./build/fyai config set logging/mcp true
```

Logs are written under `.fyai/logs/mcp.yaml`. Authorization headers, tokens,
tool arguments, and response bodies are not logged.

### `resource_metadata` is missing

The MCP endpoint returned `401` without an MCP OAuth protected-resource
challenge. Confirm that the configured URL is the Streamable HTTP MCP endpoint,
not a Gmail API URL or an ordinary web page.

For an explicit `/mcp login`, fyai derives the protected-resource metadata URL
from the full resource path. For Google's hosted endpoint this is:

```text
https://gmailmcp.googleapis.com/.well-known/oauth-protected-resource/mcp/v1
```

### Authorization metadata lacks required endpoints

The authorization-server metadata is incomplete. It must advertise
authorization and token endpoints. Google's hosted server uses the client
imported above and does not advertise a dynamic registration endpoint.

### Browser login needs `auth.allow_browser=true`

Browser authorization is disabled. Review the endpoint and then enable it:

```sh
./build/fyai config set \
  mcp/servers/google-mail/auth/allow_browser true
```

Do not enable this setting in an unattended environment unless opening a
browser is intended.

### Google blocks the consent request

Check the Gmail MCP server's Google Cloud project:

- The Gmail API is enabled.
- The Gmail MCP API (`gmailmcp.googleapis.com`) is enabled.
- The account is an allowed test user.
- The consent-screen audience is correct.
- Requested scopes are declared.
- Workspace administrator policy permits the application.
- Required sensitive or restricted-scope verification is complete.

### Login succeeds but every tool call returns HTTP 403

OAuth login does not prove that the Gmail MCP API is enabled. If the detailed
tool error says that `gmailmcp.googleapis.com` has not been used in the project
or is disabled, enable the Gmail MCP API in the project that owns the OAuth
client:

```sh
gcloud services enable gmailmcp.googleapis.com --project PROJECT_ID
```

Alternatively, follow the console link included in Google's error response.
Wait a few minutes for service activation to propagate, then retry the tool
call. The existing OAuth grant should remain valid; logging in again is not
normally necessary.

### Tool calls say `The caller does not have permission`

First confirm that the saved token has the requested Gmail scope. If it does,
this error normally means that the account or Cloud project has not been
allowlisted for the Google Workspace Developer Preview Program. Gmail MCP is
still a preview feature even though its service can be enabled in an ordinary
Cloud project.

Apply to the [Developer Preview Program][workspace-preview] with:

- An email address in a Google Workspace domain.
- The numeric Cloud project number that owns the OAuth client.

Wait for Google's final confirmation before retrying. A personal `@gmail.com`
account cannot currently be enrolled. In that case, use a qualifying Workspace
account or wait for Gmail MCP to become generally available.

### Login succeeds but refresh later fails

The Google or MCP refresh token may have expired or been revoked. Remove the
local MCP credential file and run a new request to repeat authorization:

```sh
rm "${XDG_STATE_HOME:-$HOME/.local/state}/fyai/mcp-auth-google-mail.json"
```

This removes only fyai's local MCP credential. It does not revoke the grant at
Google or at the MCP server. Revoke the application separately from the
relevant account-security or server-administration interface when access must
be terminated.

## Disable the server

Keep the configuration but stop connecting:

```sh
./build/fyai config set mcp/servers/google-mail/enabled false
```

Disable all MCP servers:

```sh
./build/fyai config set mcp/enabled false
```

Disabling a server does not delete its stored OAuth credential.

## Protocol references

- [MCP authorization specification][mcp-authorization]
- [MCP authorization overview][mcp-authorization-overview]
- [Google OAuth policies][google-oauth-policies]
- [Gmail OAuth scopes][gmail-scopes]

[mcp-authorization]:
  https://modelcontextprotocol.io/specification/draft/basic/authorization
[mcp-authorization-overview]:
  https://modelcontextprotocol.io/docs/tutorials/security/authorization
[google-oauth-policies]:
  https://developers.google.com/identity/protocols/oauth2/policies
[gmail-scopes]:
  https://developers.google.com/workspace/gmail/api/auth/scopes
[gmail-mcp-setup]:
  https://developers.google.com/workspace/gmail/api/guides/configure-mcp-server
[workspace-preview]:
  https://developers.google.com/workspace/preview
[restricted-verification]:
  https://developers.google.com/identity/protocols/oauth2/policies
[gmail-web-auth]:
  https://developers.google.com/workspace/gmail/api/auth/web-server
