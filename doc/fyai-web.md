# fyai web

Status: the page for the bridge is done and the page for the session protocol
is not. W1 to W8 and W12 have landed (the browser test of the page against the mock
provider covers the main paths), W9 is dropped, and W10 and W11 wait for `fyai serve`. The
sections "Architecture", "Backends", "Changes on top of llama.cpp" and
"Upstream surface that remains" describe the tree as it is; the work items say
what each step did and what stays open.

Related design: `doc/remote-sessions-sdd.md` (the session protocol that the
page will use), `doc/claude-sdk-compat.md` (the protocol that the page uses
now), `web/README.fyai.md` (how to build, run and package the page).

## Purpose

fyai web is a browser and desktop interface for fyai. It is a fork of the web
UI of llama.cpp (`tools/ui`, MIT license, `web/NOTICE.md`). The fork supplies
the chat shell, the streaming Markdown renderer, the attachments, the
conversation list and the PWA assets. fyai supplies the model, the tools, the
conversation state and the branches.

This document records:

1. what the fork is and how it talks to fyai;
2. what fyai changed on top of the upstream tree;
3. which upstream parts still remain, and how each fits fyai;
4. what to change so that the page matches the model of fyai; and
5. how the fork stays a clean patch series on top of upstream.

`web/README.md` is the operator manual. This document is the design.

## Terms

- **Page**: the SvelteKit single-page application in `web/src`, built into
  `web/dist`. It is installable as a PWA.
- **Bridge**: `web/server/bridge.mjs`, a Node program with no dependencies. It
  serves the page and runs fyai.
- **Desktop app**: `web/electron/main.cjs`, an Electron app. Each window is
  one project and runs a bridge of its own as a child.
- **Backend**: the server that the page talks to, and the implementation of
  the seam in `src/lib/services/backend/` for it. Upstream, the server is
  llama-server (kind `llama`). In fyai web it is the bridge (`fyai-bridge`)
  or, later, `fyai serve` (`fyai-remote`).
- **Upstream**: the llama.cpp `tools/ui` tree. The import is llama.cpp commit
  1801b7cd, and the fyai commit that holds it is d7ac555e.

## Architecture

```text
browser or Electron window (SvelteKit SPA, PWA)
        │  POST /api/fyai/chat        SSE: SDK frames and bridge/* frames
        │  GET  /api/fyai/commands    slash commands from fyai schema
        │  GET  /v1/models, /props, /slots, /models/sse   router emulation
        ▼
bridge (web/server/bridge.mjs)
        │  one process for each turn; stdin/stdout JSON lines
        ▼
fyai --branch NAME claude-sdk   ──►  the branch NAME, as in the CLI
```

### The bridge

The bridge holds no conversation state. A turn starts one `fyai claude-sdk`
process (`spawnTurn`). The arguments are `--input-format stream-json
--output-format stream-json --verbose --include-partial-messages
--permission-mode M`, after the global option `--branch NAME`. The bridge writes one `user` frame, forwards each
output frame to the page, and closes standard input when the `result` frame
arrives. The user frame holds the text of the prompt and one `image` block for
each image (W8). fyai asks the bridge for no decision: its boundary is the
sandbox profile, so the bridge runs it in `bypassPermissions`, which only tells
the SDK front not to ask (W6).

The conversation ID of the page is the name of a branch, and the branch names
are those of the CLI. A new conversation is `session/YYYYMMDDTHHMMSS.uuuuuu`
(`newSessionBranch`, the form of `fyai_branch_session_name()`), which the bridge
gives at `POST /api/fyai/conversations`. The branch exists when its first
exchange is stored. A branch that exists continues, so a session of the CLI is
a conversation of the page, and a conversation of the page can be continued in
the CLI. `claude-sdk` serves the branch that `--branch` names, whatever its
name (`doc/claude-sdk-compat.md`). Thus a turn sends only the new message, and
a conversation survives a restart of the bridge. Before it runs a turn the
bridge cuts the branch back when the turn follows an earlier message of the page
(W5). An aborted request stops the child.

Every API request needs the token of the bridge (W1). The routes:

| Route                          | Handler                     | Result                                                           |
| ------------------------------ | --------------------------- | ---------------------------------------------------------------- |
| `GET /api/fyai/login?token=T`  | the server                  | sets the cookie and goes on to `/`; no token needed              |
| `GET /api/fyai/info`           | the server                  | `{backend, protocol, authenticated}`; with the token also the binary, directory, model and models |
| `POST /api/fyai/chat`          | `handleChat`                | SSE of SDK frames between `bridge/start` and `bridge/done`       |
| `GET /api/fyai/commands`       | `listCommands`              | the commands of `fyai schema commands`, cached for 60 s          |
| `GET\|PUT\|DELETE /api/fyai/keys[/PROVIDER]` | `handleKeys` | the provider keys in the secret store of fyai; an answer says only whether a key is set |
| `GET\|PUT /api/fyai/settings`  | `handleSettings`            | the settings of the page, without any key, in a file of the user |
| `GET /api/fyai/conversations`, `GET\|PATCH\|DELETE .../ID`, `GET .../ID/messages` | `handleConversations` | the branches of the sessions, their messages, the title, the delete (W4) |
| `GET /v1/models`               | `modelsPayload`             | the catalogue in the shape of a llama.cpp router, with the input types and abilities of each model |
| `GET /props`                   | `propsPayload`              | a router `props`; `n_ctx` and the modalities of the model from the catalogue |
| `GET /models/sse`, `POST /models/load\|unload` | the server       | a status map in memory; fyai has no load step                    |
| `GET /slots`, `GET /tools`, `GET /v1/streams/lookup` | the server  | constant empty answers                                           |
| `/v1/chat/completions`         | the server                  | 501                                                              |
| other paths                    | `serveStatic`               | the built page, with a fallback to `index.html`; no token needed |

A prompt that starts with `/` does not go to the model. The bridge runs it as
an fyai verb on the branch of the conversation (`runSlashCommand`), with the
words as `argv` and no shell (`slashCommandArgs`). The output arrives as one
`bridge/command` frame, in a text fence.

The page holds no key and sends none. fyai reads the key of a provider from its
own configuration (`api_key: {type: env, ...}`), from `<PROVIDER>_API_KEY` in
the environment of the bridge, or from its secret store. The bridge writes no
key to a file, and it removes its own token from the environment that its
children get. The Provider keys panel of the settings stores a key with
`fyai secret set api-key/PROVIDER --stdin`: the value goes through standard
input, in no argument and no file, and the bridge never returns it. On macOS
the store is the Keychain. On Linux it is the kernel keyring of the user,
which does not survive a reboot, so the panel then shows the provider as not
set. A key in the environment of the bridge wins over the stored key. A remote
server keeps its keys at the server, stored securely there, through the same
routes, and the page does not carry them.

### The desktop app

`electron/main.cjs` finds fyai in `FYAI_BIN`, then in `resources/fyai` of the
bundle, then in the build tree, then on `PATH`.

**One window is one project.** A project is a directory that fyai uses as its
project root. The app starts with the project that a command line argument
names (`fyai-desktop DIR`), else `FYAI_BRIDGE_CWD`, else the start directory,
else a directory under the data directory of the platform. File > Open Project
(Ctrl or Cmd+O) asks for a directory and File > Open Recent lists the last ten
(`recent-projects.json` in the data directory). A project that is open already
gets its window back, so a project never has two bridges. A second launch of
the app opens the project it names in a new window, or shows a window that
exists. `fyai init` runs where a directory has no arena. The title of a window
is the name of its directory.

Each window has its own bridge, started with `--cwd DIR`, `ELECTRON_RUN_AS_NODE=1`,
`--parent-pid` and the port of the project (`--port`, below). The app reads its
address from the `listening on` line of its standard error. `FYAI_BRIDGE_PORT` fixes the port of the first
window only. The window has context isolation, no Node integration, no preload
script and no IPC. Closing a window stops its bridge, and quitting stops all of
them. `electron/projects.cjs` holds the choices that need no Electron, and a
unit test covers it.

The bridge of a window listens on a port that the system gives. The page keeps
nothing in the browser that must survive: the conversations are branches of
fyai, and the settings are in a file of the user (`GET` and `PUT
/api/fyai/settings`, below). So the address of a window can change at each
launch.

## Backends

The page is one build for three servers. At start it asks each registered
backend whether it is the server behind the page (`probe`), in registration
order, and takes the first that answers. A server that answers none is
llama-server. A kind can be forced: the `backend` query parameter of the address (`?backend=fyai-bridge`), the `fyai-backend` entry of the local storage, or the build setting `VITE_PUBLIC_BACKEND`, in that order. A kind that is not registered is ignored and the probes run. A page served from a host that has no bridge is forced this way.

| Kind           | Server                       | State                         |
| -------------- | ---------------------------- | ----------------------------- |
| `llama`        | llama-server                 | upstream behaviour, unchanged |
| `fyai-bridge`  | the bridge (`/api/fyai/info` answers `backend: "fyai"`) | implemented |
| `fyai-remote`  | `fyai serve` over a WebSocket | planned (W10)                |

The seam (`src/lib/services/backend/`) holds the capabilities of a kind, an
optional `runTurn`, `loadCommands`, `handleLine` and `titleFor`, and its
`probe`. `backendStore` (`src/lib/stores/backend.svelte.ts`) keeps the active
backend and answers `backendIs(kind)` and `supports(capability)`. The fyai
bridge backend registers itself in `src/lib/stores/fyai-backend.svelte.ts`.

A part of the page that a server lacks is hidden by a capability, and it is
kept, unchanged, for a server that has it:

| Capability     | The part of the page                                   | fyai bridge |
| -------------- | ------------------------------------------------------ | ----------- |
| `agenticLoop`  | the tool loop in the page; the Agentic settings        | no          |
| `browserTools` | the JavaScript sandbox, browser tools; the Tools settings and menu | no |
| `browserMcp`   | MCP servers connected from the browser                 | no          |
| `modelManager` | Hugging Face discovery and the models manager          | no          |
| `sampling`     | sampling and penalties; their settings section         | no          |

A settings field can name the kinds that show it (`backends`), as the Provider
API keys field does. A backend whose `runTurn` returns `skip` for a message
that has no text gets an error for the user when it has no `agenticLoop`,
because the generic request paths speak to llama-server only. The sidebar
entries of the models manager and of the MCP client, and the manage models
item of the model selector, are left out for a backend that lacks them.

### Deployments

| Use                     | Page served by             | Backend        | Authentication            |
| ----------------------- | -------------------------- | -------------- | ------------------------- |
| Desktop app             | its bridge, loopback       | `fyai-bridge`  | token of the app, login link |
| Browser on one machine  | the bridge, loopback       | `fyai-bridge`  | token, login link         |
| Remote session          | `fyai serve`, or a static host | `fyai-remote` | pairing and device token (SDD 6) |
| Hosted page on a cloud service | a static host       | `fyai-remote`  | pairing and device token  |
| Hosted page, fyai in the cloud | the same service    | `fyai-remote`  | the service signs the user in |

A hosted page is the same build as every other. It has no bridge, so its
backend is chosen by the address it is given, not probed on its own origin.
Two designs stay open, and the page supports both. In the first, the page is
static and connects to the `fyai serve` of the user over a WebSocket on a
network the user provides. That needs an origin list in `fyai serve`, because
SDD 6.5 allows only the origin of the server that serves the page. In the
second, the service runs fyai for each user and the page is served beside it.
Both use the protocol of `doc/remote-sessions-sdd.md`, so the page differs only
in the address and in how it gets its token.

## Changes on top of llama.cpp

`git diff d7ac555e HEAD -- web/` holds the complete delta.

### Transport

| Change                                   | Files                                                  |
| ---------------------------------------- | ------------------------------------------------------ |
| Turn transport over SSE from the bridge  | `src/lib/services/fyai.service.ts` (new)               |
| SDK frames to chat messages              | `src/lib/services/fyai.turn.ts` (new)                  |
| The backend seam and its store           | `src/lib/services/backend/*`, `stores/backend.svelte.ts` |
| The fyai bridge as a backend             | `src/lib/stores/fyai-backend.svelte.ts`                |
| The chat store calls the seam            | `src/lib/stores/chat/index.svelte.ts`                  |
| Bridge and its decisions                 | `server/bridge.mjs`, `server/protocol.mjs` (new)       |
| Dev proxy to the bridge                  | `vite.config.ts`, `.env.example`                       |

`buildFyaiPrompt` (`fyai.service.ts:194`) sends only the last user message and
its text attachments. fyai holds the earlier messages. The page sends no
system message and no sampling parameter.

`FyaiTurnAssembler` (`fyai.turn.ts:64`) maps the frames:

| Frame                                 | Result in the page                                 |
| ------------------------------------- | -------------------------------------------------- |
| `system`/`init`                       | the model of the answer                            |
| `stream_event` `text_delta`           | text added to the open assistant message           |
| `assistant`                           | the message ends; each `tool_use` becomes a call   |
| `user` with `tool_result`             | the result row of that call                        |
| `result`                              | input and output token counts; an error mark       |
| `bridge/command`                      | one message with the fenced output of the command  |
| `bridge/error`                        | the turn fails                                     |

Each assistant message after the first becomes a new message row. Thus the
tool loop of one turn shows as several messages with the results between
them.

### Chat store

- The chat store calls `backendStore.active.runTurn` before the agentic and
  plain paths. The fyai bridge backend runs the whole turn there for a
  message with text.
- `handleLine` takes `/clear`: it starts a new conversation and sends nothing.
- `titleFor` makes the title: the first line of the first message. No model
  request makes a title.

### Models

- The bridge acts as a llama.cpp router. The picker lists the catalogue, and
  each model reports as loaded.
- The list holds only the models of a provider that has a key
  (`fyai catalog list models --available`). With no key, it holds the whole
  catalogue, so that the user can enter a key.
- The model of the project is the first row.
- `modelKey()` (`stores/models/index.svelte.ts:41`) matches a model name
  without its provider prefix.
- `/model NAME` selects a model in the page.

### Commands

- `backendStore.loadCommands()` loads the commands of the backend one time;
  the bridge backend reads them from the bridge.
- `commandsFromSchema` (`protocol.mjs:29`) keeps a command that has the surfaces
  `cli` and `session` and is not `interactive`.
- The picker lists them after `prompt`, `cwd` and `model`
  (`utils/chat-commands.ts`, `hooks/use-chat-form-pickers.svelte.ts`).

### Settings

- **API Key** shows for llama-server only. fyai keeps its keys itself, so the
  `fyai-bridge` page has no key field. The Provider API keys field was removed.
- With the `fyai-bridge` backend the settings are not in the local storage. The
  bridge keeps them in `web-settings.json` in the configuration directory of the
  user (`$XDG_CONFIG_HOME/fyai`, mode 0600), so every window, project and
  browser has the same ones. The page loads them at start
  (`settingsStore.useRemote`) and writes a change after 400 ms. A server with no
  file takes the settings that the page held, which moves the ones of an earlier
  visit. The bridge drops `apiKey` and `providerApiKeys` from what it is sent.

### Desktop, packaging and build

| Change                                         | Files                                                   |
| ---------------------------------------------- | ------------------------------------------------------- |
| Electron window, bridge child, project choice  | `electron/main.cjs`                                     |
| Run and edit scripts                           | `scripts/run.sh`, `scripts/dev.sh`, `scripts/desktop.sh`|
| App packaging, ad hoc macOS signature          | `scripts/package.sh`                                    |
| Static fyai inside the app                     | `scripts/bundle-fyai.sh`                                |
| Launchers and menu entry                       | `scripts/fyai-web.in`, `fyai-desktop.in`, `fyai.desktop.in` |
| CMake targets `fyai-web*`, `fyai-desktop*`     | `cmake/web.cmake`, `CMakeLists.txt` (`FYAI_WEB`, `FYAI_DESKTOP`) |
| Release of the desktop app                     | `.github/workflows/web-release.yaml`                    |
| llama-server embedding removed                 | `CMakeLists.txt`, `sources.cmake`, `ui.cpp.in`, `ui.h.in` |

### Tests

| Test                                             | What it checks                                         |
| ------------------------------------------------ | ------------------------------------------------------ |
| `tests/unit/fyai-bridge.test.ts`                 | all decisions of `protocol.mjs`                        |
| `tests/unit/fyai-bridge-keys.test.ts`            | the provider key routes: set, replace, remove, no value in an answer |
| `tests/unit/fyai-prompt.test.ts`                 | the prompt and the key lines                           |
| `tests/unit/fyai-turn.test.ts`                   | frame assembly                                         |
| `tests/unit/chat-commands.test.ts`               | the fyai commands in the picker                        |
| `tests/client/chat-form-slash-commands.svelte.test.ts` | `/model NAME`                                    |
| `tests/unit/fyai-bridge-*.test.ts`               | the bridge as a process: access, conversations, the cut of a branch, images, against a stand-in for the verbs of fyai (`tests/fixtures/fake-fyai.mjs`) |
| `tests/unit/fyai-conversations.test.ts`, `fyai-tools.test.ts`, `backend-*.test.ts`, `electron-projects.test.ts` | the rows of a stored conversation, the cards of the tools, the backend seam, the projects of the desktop app |
| `server/e2e-mock.mjs` (`fyai-web-e2e`)           | the page in a browser, against the bridge, the real fyai and the mock provider, with no key: a turn, the next turn, regenerate, edit, the task list and shell cards, an image, and a conversation that comes back from fyai |
| `server/test-turn.sh`                            | one turn through the bridge against the mock provider  |
| `server/e2e.mjs`, `e2e-electron.mjs` | the built page and the window, in Chrome, with a real provider and key |

CTest runs `npm run test:unit` as `fyai/web-test` when `FYAI_WEB=ON`. The
browser test needs a Chrome or the Chromium of Playwright and the built page,
so it is the target `fyai-web-e2e` and not a CTest test. It uses the scenario
`tests/scenarios/web_chat.json`, which answers "Mock answer N" to the N-th
request, so each step knows the answer it waits for, and it asserts on the
requests that the mock got as well as on the page. Use it rather than the
scripts that need a key: it is hermetic, with private directories and no key
in the environment. Electron has no test.

## Upstream surface that remains

The fork keeps most of the upstream tree. Each part is in one of three
classes.

### Parts that fit fyai

| Part                                   | Files                                                    |
| -------------------------------------- | -------------------------------------------------------- |
| Chat screen and streaming Markdown     | `components/app/chat/*`                                  |
| Tool call cards; an edit diff renderer | `ChatMessageToolCallBlock*.svelte`, `utils/compute-line-diff.ts` |
| Model picker in router mode            | `components/app/models/ModelsSelector/*`                 |
| Conversation list and tabs             | `SidebarNavigation/*`, `stores/tabs.svelte.ts`           |
| Settings dialog                        | `constants/settings.constants.ts`, `SettingsChat/*`      |
| Text and PDF attachments, and images   | `utils/process-uploaded-files.ts`, `fyai.service.ts`     |
| Stop                                   | the abort controller; the bridge stops the child         |

### Parts that do nothing against the bridge

| Part                                      | Why                                                        |
| ----------------------------------------- | ---------------------------------------------------------- |
| Stream resume (`stores/chat/streams.svelte.ts`) | `/v1/streams/lookup` is empty; `/v1/stream` is 404   |
| Pre-encode                                | only `onFlowComplete` starts it, and the fyai path does not call it |
| Continue generation (`flows.svelte.ts`)   | it calls `/v1/chat/completions`, which is 501              |
| Reasoning budget                          | no control endpoint; the reasoning blocks show the thinking |
| Server tools, `/cwd`, `@` mentions        | `/tools` is empty                                          |
| Sampling, penalties, parameter sync       | read from a constant `/props`, never sent                  |
| System message, model titles              | stored or shown, never sent                                |
| Model load and unload                     | a status map in the bridge; Vite does not forward `/models` |
| `/health`, `/metrics`, `/build.json`      | not served                                                 |
| API key splash (`ServerErrorSplash.svelte`) | the bridge answers 401 to a page with no token, and the splash then asks for an API key, which is not what is missing (open) |

### Parts that conflict with fyai

| Part                                       | Conflict                                                                                   |
| ------------------------------------------ | ------------------------------------------------------------------------------------------ |
| Conversation store (`database.service.ts`) | with `serverConversations`, the tables are in memory and fyai holds the conversations (W4). The page lists the branches at start and reads the messages of a conversation when it opens. The fork action is hidden, because a fork has no branch. |
| Regenerate, edit, message branches         | the tree of the page keeps every sibling and the branch of fyai is one line. W5 cuts the branch back before a turn; switching to another sibling moves it at the next turn, not at once. |
| Agentic loop (`stores/agentic/*`)          | hidden and never run for fyai; a message with nothing to send gets an error message.      |
| MCP client in the browser (`services/mcp.service.ts`) | the browser connects to MCP servers; fyai never gets their tools. The sidebar entry is hidden for fyai; the pickers in a chat are not. `/cors-proxy` is not served. |
| JavaScript sandbox and browser tools       | hidden for fyai (the Tools section and menu); they never run on its path.                 |
| Tool store and per-conversation tool choice | hidden for fyai with the Tools section; the bridge never passes `--tools`.              |
| Permission store and permission card       | fyai never asks the page: its boundary is the sandbox profile, not a prompt for each call. |
| Hugging Face discovery and model manager   | GGUF files, quantizations and downloads have no meaning for fyai; the entries are hidden. |
| Context gauge                              | `prompt_n` is the input of the whole tool loop; each message now stores the usage of its own frame (W7). |
| Conversation import and export             | move rows of the page only. An imported conversation has no branch.                       |
| Search (`routes/search`)                   | searches the conversations that the page loaded in this session, not all the branches.     |
| Audio and video                            | the composer takes them; fyai takes images and text files only, so the page says so (W8). |

### Defects in the current tree

- **The page does not show the sandbox profile.** fyai confines its tools with
  a sandbox profile (`doc/user-guide.md`, chapter 7), and the bridge runs fyai
  in the SDK mode `bypassPermissions`, which only means that fyai does not
  ask. A user of the page cannot see whether the tools run in `dev`,
  `lockdown` or no sandbox, and cannot change it (W6).
- The page shows neither the tool list of `system/init` nor the cost of
  `result`, and the assembler ignores `input_json_delta` (W7).
- About 85 upstream unit and client tests cover code that does not run against
  the bridge. The Storybook project calls `pnpm`.

## Plan

### Rules

1. **fyai is the source of truth.** A conversation is a branch. The page keeps
   a cache, not a history of its own.
2. **One build serves both backends.** The page detects the backend at start
   and enables the fyai parts at run time. The upstream parts stay for
   llama-server. Nothing upstream is deleted.
3. **The fork is a patch series.** Each patch is one logical change on a
   recorded upstream base, and it builds. A patch adds a module or a narrow
   hook. It does not rewrite an upstream file.
4. **The page uses a protocol, not a process.** The page talks to an fyai
   session endpoint (`doc/remote-sessions-sdd.md`). The bridge that starts a
   process for each turn is a step to that, not the destination.
5. **The page gets no credential that it does not need.** A provider key that
   the page holds goes into the environment of one fyai process. It never goes
   into the arena and never back to a page.

### Work items

Each item states what changes, why, and which fyai interface it uses.

**W1. Secure the bridge.** Done.

- The bridge has a token for each start: `--token`, `FYAI_BRIDGE_TOKEN`, or 256
  random bits. An API request needs it, as the `x-fyai-token` header or as the
  cookie `fyai_bridge` (`HttpOnly`, `SameSite=Strict`). The page files and a
  minimal `/api/fyai/info` need none, so the page loads before a login and
  can tell the user what to do.
- The bridge prints a login link, `/api/fyai/login?token=T`. It sets the
  cookie and goes on to `/`. `next` must be a path of the bridge. The desktop
  app makes its own token and opens that link; `run.sh` and `dev.sh` print it.
- A request whose `Origin` is not the bridge itself and not in `--allow-origin`
  gets 403, and no answer carries `access-control-allow-origin: *`. A listed
  origin gets its own origin, with credentials, never `*`.
- A bridge on a loopback address answers loopback host names only (and
  `--allow-host`), which stops DNS rebinding.
- The token is the boundary of the bridge, and the sandbox profile of fyai is
  the boundary of the tools. A client that holds the token runs tools as the
  user does in a terminal, in the profile of the project.
- Open: TLS for a non-loopback address (use a tunnel or a proxy until then),
  and a token for each device (SDD 6).

**W2. Detect the backend at run time.** Done.

- `GET /api/fyai/info` returns `{backend: "fyai", protocol, extensions}`. The
  page asks it at start. A 404 means llama-server.
- `src/lib/services/backend/` holds the seam: one interface for a turn, the
  models, the commands and the conversations, with an upstream implementation
  and an fyai implementation.
- A setting can force the backend kind.
- The fyai parts register through this seam. Upstream files call the seam at
  a small number of hook points, and `web/NOTICE.md` lists each hook point.

**W3. Gate the llama.cpp surface.** Done for the settings sections, the tools menu, and the entries of the models manager and the MCP client. The MCP controls inside a chat (the resource and prompt pickers) are not gated; they list nothing when no browser MCP server is set. With the fyai backend, hide the parts in
"Parts that do nothing" and the following parts of "Parts that conflict":

- Hugging Face discovery and the model manager
- sampling and parameter sync
- the JavaScript sandbox and browser tools
- the agentic loop
- the MCP client in the browser

Keep each part, unchanged, for llama-server. The gate is one predicate,
`backendIs('fyai')`, at the hook points of W2.

**W4. Conversations are branches.** Done: fyai holds the conversations and the
browser keeps none. The browser test checks a turn, a conversation coming back
from fyai after a reload with no database in the browser, the name of the
branch, the tool cards and an image.

- The bridge serves `GET /api/fyai/conversations` (the branches that hold an
  exchange, from `branch list --output json`, newest first; a branch with no
  description is titled by its first question, `titleFromHistory`),
  `POST /api/fyai/conversations` (the name of a new session), `GET .../ID/messages`
  (`history --output json`), `PATCH .../ID` (`branch describe`, so the title is
  the description of the branch) and `DELETE .../ID` (`branch delete --force`).
  The ID is the name of the branch, URL-encoded because it holds slashes
  (`conversationBranch` refuses a name that fyai could read as an option). A
  change waits for a turn that runs (409). The sub-agent branches (`agent:`) are
  listed too, for now.
- With the capability `serverConversations` the tables of the page are in
  memory (`DatabaseService.useMemory()`, Dexie over `fake-indexeddb`), so the
  page makes and reads no IndexedDB database and the migration of the old one is
  skipped. At start the page lists the branches as conversations
  (`syncConversations`); the messages of one are read when it opens
  (`hydrateConversation`: `fyaiConversationRows`, a root and a chain, a call on
  the assistant message before it, its result as a tool message). The siblings
  of an edit or a regenerate live in memory for the session; fyai holds the one
  line. A list that cannot be read changes nothing. A second browser, or a
  reload, therefore sees the conversations of the project and of the CLI.
- Delete and rename in the page reach the branch through the one place that
  tells the page of a deletion (`notifyConversationsDeleted`) and through
  `updateConversationName`.
- An image of a stored user message comes back as an attachment: `history`
  writes its bytes as JSON text, a high byte as a lone surrogate, and
  `storedImageBytes` reads them back.
- Open: the reasoning of a stored message is not restored; search and the
  export and import of the page see only the conversations that it loaded; the
  list is read once at start, so a change made elsewhere shows at the next
  load; a fork needs `branch new` at a turn of the branch.

**W5. Edit and regenerate move the branch.** Done. The browser test regenerates and edits and
checks the requests that the mock got.

The page keeps a tree of messages and can start a turn from any node of it,
while a branch of fyai is one line. So each turn carries an anchor: the last
user message that it follows (`fyaiAnchor`: the prompt as it was sent, and the
count of equal prompts up to it). Before it runs the turn, the bridge cuts the
branch back to the end of the exchange of the anchor (`rewindSession`):

- the history of the branch is read and `rewindPlan` decides: nothing, when
  the branch ends there already or the anchor is not in it; delete the branch,
  when the turn follows no message and the branch holds some (the first message
  was edited or regenerated, and the turn starts a new session); or reset,
  to the message before the next user message;
- fyai counts turns, not messages, and `reset` cannot go to a message, so a
  copy of the branch is stepped back one turn at a time, with
  `view/track_project` off, until its history has the length that the plan
  names. The branch is then reset by that many turns, and the copy is
  deleted. With `view/track_project` on, that last reset also rewinds the
  project files, as `reset` does;
- a branch that ends at the last turn that this bridge saw complete is not
  read: the bridge remembers the prompt of that turn;
- an anchor that is not found is not an error, because the prompt that fyai
  stored may differ from the prompt of the page; the turn goes on as before.
  A reset that fails stops the turn with its cause.

The message tree of the page stays the page's own: a regenerated message is a
sibling in the tree, and the branch of fyai holds only the line that was run
last. Open: switching to another sibling in the page does not move the branch
until the next turn from it, and the bridge is sent no anchor by a client that
does not send one, which then checks nothing.

**W6. Show and choose the sandbox profile.** The first plan of this item was a
permission prompt for each tool call, as the Claude Agent SDK has. It is
dropped: fyai has no such prompt. A tool runs confined by the sandbox profile
(`dev`, `lockdown`, or none; `doc/user-guide.md`, chapter 7), and the
`bypassPermissions` mode of the bridge only tells the SDK front not to ask.

- The page shows the profile of the session: the bridge reads `sandbox_profile`
  and the sandbox state from fyai and puts them in `/api/fyai/info`.
- The user chooses the profile through the fyai commands that exist for it,
  `/session dev|lockdown|yolo` and `/session profile NAME`, which the command picker
  already offers where the schema lists them. The page does not keep a
  second list of profiles.
- A host that cannot make the sandbox warns once (`sandbox_on_failure`); the
  page shows that warning with the turn, as it shows any diagnostic.
- Open: what a remote client may change. A `config` key is `read`, `review`
  or `write` for the model, and the remote design needs the same split for a
  client (`x-fyai-remote`, SDD 11).

**W7. Tool cards from fyai names.** Done in part. The browser test checks the task list
and the shell card, live and from a restored conversation.

- `Bash`, `Read`, `Write` and `Edit` are shown under the name and arguments of
  the cards that fit them (`exec_shell_command`, `read_file`, `write_file`,
  `edit_file`), by `fyaiToolView` in `fyai.tools.ts`. Any other tool, such as
  `apply_patch`, keeps its name and the default card.
- A tool result that fyai marks `is_error` is stored as `{"error": TEXT}`, the
  form in which the cards show a failure.
- The thinking of the model, from `thinking_delta` and from `thinking` blocks,
  goes to the reasoning block of the message.
- Each message stores the token counts of its own `assistant` frame. The
  `result` frame still sets the counts of the turn for the context gauge.
- A call of the task list tool (`TodoWrite`, stored as `todo_write`) shows as a
  list with a mark for each state and the count that is done
  (`ChatMessageToolCallBlockTodo.svelte`).
- Open: a card for `apply_patch` (a patch is not an `edit_file`), the tool list
  of `system/init` and the cost of `result`, which the page shows nowhere, and
  `input_json_delta`, so a call shows only when its message is complete.

**W8. Attachments.** Done. The browser test attaches an image and checks the request.
It found that the bridge told the page every model took text only, which refused the
image; the bridge now reports the input types of each model.

- An image of the last user message goes to the bridge as base64 with its
  type, and the bridge makes it an `image` block of the user message, as the
  Claude Agent SDK does. This needed support in fyai: the SDK front took the
  text blocks only and left the others out (`doc/claude-sdk-compat.md`). It
  now attaches each image block to its message, so the session stores the
  bytes and the request carries them in the form of the API.
- A message of images alone is a prompt. The types are PNG, JPEG, GIF and
  WebP; the bridge refuses another type and data that is not base64.
- Text files and PDFs go as text, as before. Audio and video are refused with
  a message, not dropped.
- The anchor of W5 is the text of the message, so an edit or a regenerate of a
  message that has images finds its place by the text alone.
- Open: the page does not check the image modality of the model; fyai refuses
  an image for a model that takes none.

**W9. A working directory for each conversation.** Dropped. A project is the
unit of fyai: its arena, its configuration and its sandbox profile belong to
the directory. The desktop app has one window and one bridge for each project,
and a browser has one bridge for each `--cwd`, so a conversation needs no
directory of its own. A server that serves several projects is the job of
`fyai serve` (`doc/remote-sessions-sdd.md`), which names the project of each
session. The `/cwd` command of the page needs the file tools of llama-server and
is hidden with them.

**W10. Move to the session protocol.** This item depends on
`doc/remote-sessions-sdd.md`.

- The page connects to `fyai serve` over a WebSocket and speaks the Agent SDK
  protocol with the fyai extensions.
- The bridge becomes a static file server. In the desktop app, the main process
  starts `fyai serve` and connects to it.
- The page gets the parts that a process for each turn cannot give: a running
  turn that survives a reload, `ask_user`, the todo panel, sub-agents, the
  terminals of the work pane, and a mirror of the terminal screen.

**W11. Mobile.**

- First, the page as an installed PWA over a paired connection (SDD,
  "Authentication and pairing").
- Then a Capacitor (or equivalent) wrapper for iOS and Android. It carries the
  same build, with secure storage for the device token, and a QR scanner for
  pairing.
- Without a relay there is no push notification. The app reconnects and
  resynchronizes when it comes to the foreground.

**W12. Restructure the existing patches.** Done: the upstream files were
restored to the import in one commit and the fyai changes were applied again as
the series below, behind the seam of W2. The old tip is tagged
`web-pre-reseries`.

### Patch series order

1. The upstream import, unchanged, with the base commit in `web/NOTICE.md`.
2. The backend seam (W2), with upstream as the only implementation. This patch
   could go upstream.
3. The bridge and the fyai backend.
4. Each later feature as its own patch: W1, W3 to W9.
5. The desktop app and packaging.
6. Tests, then documentation, as `CLAUDE.md` orders them.

A patch that could go upstream has no fyai name in it. It adds a generic
ability, such as a backend interface, a permission request from the server,
or a list of conversations from the server.

### Syncing with upstream

`web/` is the `tools/ui` directory of llama.cpp with a prefix, so a subtree
merge brings new upstream commits in. The fyai changes are small hooks and new
files (`web/NOTICE.md` lists each hook), so a merge conflicts only where an
upstream commit touches a hook. This procedure is designed and not yet run.

Once, to record the upstream base:

1. `git remote add llama-cpp https://github.com/ggml-org/llama.cpp.git`
   and `git fetch llama-cpp master`.
2. `git subtree split --prefix=tools/ui -b llama-ui llama-cpp/master` makes a
   branch that holds the history of `tools/ui` only, with its files at the
   root. Add `--onto` or a start point to cut the history short if the split
   is slow.
3. Find the commit of that branch whose tree is the import (llama.cpp
   1801b7cd): `git log llama-ui --format='%H %s' | ...` and check it with
   `git diff <commit> d7ac555e:web` (no difference).
4. Record it as the merge base with `git merge -s ours
   --allow-unrelated-histories <commit>`. The tree stays as it is and the
   history now says where `web/` came from.

At each sync:

1. `git fetch llama-cpp master` and `git subtree split --prefix=tools/ui -b
   llama-ui llama-cpp/master`.
2. `git merge -Xsubtree=web llama-ui`. The merge base is the commit that step
   4 recorded, or the last merged tip.
3. Resolve each conflict against the hook list of `web/NOTICE.md`. A file
   that is only an fyai file (`server/`, `electron/`, `README.fyai.md`) does
   not conflict.
4. Build the page, run `npm run test:unit` and `npm run test:client`, and run
   the bridge test and `server/e2e.mjs`. An upstream test for a gated part must
   still pass against the `llama` backend.
5. Update the commit in `web/NOTICE.md` and the Upstream term above.

`git subtree pull --prefix=web` is not used: it needs the subtree metadata
that a plain import does not have, and the explicit merge shows the same
history.

## Tests

| Work item | Tests                                                                                   |
| --------- | --------------------------------------------------------------------------------------- |
| W1        | the bridge refuses no token, another origin, another host, and a public host with the default mode (`fyai-bridge-access.test.ts`) |
| W2, W3    | the client tests run with the fyai backend and with a mock llama backend                |
| W4, W5    | a bridge test against the mock provider: list, rename, delete, regenerate, edit         |
| W6        | a bridge test: `info` reports the profile; a command changes it                         |
| W7        | assembler tests for each mapped tool, thinking, `is_error`, usage                       |
| W8        | an image turn against the mock provider                                                 |
| W10       | the protocol tests of the SDD; the page against `fyai serve`                            |

Each upstream test runs against the upstream backend, so a gated part keeps
its tests.

## Open questions

- Is the bridge still necessary after W10, or does `fyai serve` also serve the
  page files?
- Which wrapper for mobile: Capacitor, or a smaller WebView shell?
- Do provider keys stay in the page, or does the page only name a key that the
  host already has (`api_key: {type: env, ...}`)?

## What remains for the page

- The session protocol (W10): the `fyai-remote` backend over a WebSocket, a
  running turn that survives a reload, questions, the task panel, terminals and
  the screen mirror. The seam, the forced kind and the conversations as
  branches are in place for it.
- Mobile and the hosted page (W11), with pairing, TLS and a token for each
  device; the allowed-origins list of `fyai serve` for a static page on
  another origin.
- The sandbox profile in the page (W6).
- A card for `apply_patch`, the tool list and the cost of a turn, and
  `input_json_delta`, so a call shows while its arguments arrive (W7).
- The 401 screen of a page that was opened with no token, which asks for an API
  key; the MCP pickers inside a chat; switching to another sibling of the
  message tree moves the branch only at the next turn.
- A test of the desktop app itself; the browser test runs the page and the
  bridge but not Electron.
