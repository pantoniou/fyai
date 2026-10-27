# fyai web UI

A browser chat interface for `fyai`, installable as a PWA. It is a fork of
the llama.cpp web UI (`tools/ui`), which supplies the chat shell, streaming
Markdown rendering, attachments, conversation store and the PWA assets. The
transport is replaced: instead of llama-server, the page talks to a local
bridge that runs `fyai claude-sdk` and streams the Claude Agent SDK protocol.

```text
browser (SvelteKit SPA, PWA)
        │  POST /api/fyai/chat      SSE of SDK frames
        ▼
bridge (web/server/bridge.mjs, Node, no dependencies)
        │  stdin/stdout JSON lines
        ▼
fyai claude-sdk  ──►  provider, tools, branches, arena
```

## Why a bridge

The browser cannot start a process and fyai has no daemon. The bridge is a
small program that you run yourself; it holds no conversation state. Each
turn starts one `fyai claude-sdk` process with the session of the
conversation, and fyai stores the turn in the branch of the conversation. The
conversation ID is the name of the branch, in the form that the CLI uses
(`session/TIME`), so a turn only sends the new message, the conversation
survives a bridge restart and a session of the CLI is a conversation of the
page.

## Choosing the model

fyai serves every model of the project catalogue, so the bridge answers the
page as a llama.cpp _router_: the picker at the composer lists the catalogue,
and the model a conversation picks travels with each turn. fyai has no load
step, so every model is reported as loaded.

- Only the models of a provider that has a key are listed
  (`fyai catalog list models --available`): the key is in the environment of
  the bridge, in the keyring, or behind a credential transport. When no
  provider has one, the whole catalogue is listed, so a key can be entered in
  the settings.
- The model of the project (`fyai --get model`) is the first row, and the one
  a new conversation starts with.
- `/model NAME` selects a model as well. Typed in full it is resolved by the
  page; when the prompt reaches fyai instead (pick the command from the list,
  or press Escape and then Enter), its output is shown as a message and the
  page takes the model it names.
- An answer records the model that produced it, so a conversation reopened
  from history returns the picker to that model.
- The catalogue says what each model takes and can do (`fyai catalog get
  models`): the picker and `/props` report the input types, so the composer
  accepts an image only for a model that takes one, and the abilities of the
  model, such as reasoning and tool use.

## Requirements

- Node 20 or later.
- `fyai` on `PATH`, or `FYAI_BIN` set to its path.
- A provider and model configured for the project where fyai runs
  (`fyai model`, `fyai auth`, or the environment).

## Run

There are two scripts. Both take the fyai program from `--fyai PATH`, else
`$FYAI_BIN`, else `../build/fyai` (the project above this one), else `PATH`,
and both install the page's dependencies on the first run. Each run prints
the program it chose, so there is no doubt which fyai the page drives.

**`scripts/run.sh`** serves the built page: it builds when `dist/` is missing,
then starts the bridge.

```sh
cd web
./scripts/run.sh                     # http://127.0.0.1:9331
./scripts/run.sh --cwd ~/project     # another project for fyai
./scripts/run.sh --fyai /usr/local/bin/fyai
./scripts/run.sh --port 8080 --open  # another port, and a browser
./scripts/run.sh --build             # build the page again first
```

**`scripts/dev.sh`** is the edit loop: the bridge runs fyai and Vite serves
the page in front of it, forwarding `/api`, `/v1`, `/props` and `/slots`.
A change to a source file reloads the page and leaves the bridge alone.

```sh
cd web
./scripts/dev.sh                     # page on http://127.0.0.1:5173
./scripts/dev.sh --bridge-port 9440 --cwd ~/project
./scripts/dev.sh --fyai ~/work/fyai/build/fyai
```

Ctrl-C stops both. `npm start` and `npm run dev` run these scripts.

fyai works in the fyai project above `web/`. A copy of the page elsewhere
works in the directory the script was started from. A project with no
`.fyai/arena` gets a warning, and its model list is empty until `fyai init`
runs there.

A bridge started by hand has the same choice as `--bin`:

```sh
node server/bridge.mjs --bin /usr/local/bin/fyai --cwd ~/project
```

## Desktop app

`scripts/desktop.sh` opens the page in an Electron window. The bridge runs
as a child of the app, so nothing else has to be started:

```sh
cd web
./scripts/desktop.sh                     # the repo build, or fyai on PATH
./scripts/desktop.sh --fyai ~/bin/fyai   # another fyai
./scripts/desktop.sh --cwd ~/project     # another project
```

Each window is one project. A directory on the command line is the first
project (`fyai-desktop ~/project`). Without one, the `FYAI_BRIDGE_CWD`
directory wins, then the directory the app was started in. File > Open Project
(Ctrl or Cmd+O) opens another project in a new window, and File > Open Recent
lists the last ten. A project that is open gets its window back, and a second
launch of the app opens its project in a new window. Each window has a bridge
of its own, which stops when the window closes. The port is one that the system gives;
the conversations and the settings are not in the window, so they are there at
the next launch. A launcher starts
it in the bundle, in `/` or in the home directory, which is no place for an
arena, so the app then uses a directory of its own under the data directory of
the platform (`~/.config/fyai/project` on Linux, `~/Library/Application
Support/fyai/project` on macOS). A directory with no project gets one from
`fyai init`.

The sidebar of the page names the project of the window: the bridge reports
its directory, its name and whether an arena exists for it (`project` of
`/api/fyai/info`). In the app, the name opens a menu with Open Project and the
recent projects, which act as the entries of the File menu. A browser tab shows
the project but cannot open another.

The window loads the built page, so run `npm run build` after you change it.
Electron needs Node 20 or later, and a graphical session.

The window asks the bridge for a free loopback port and reads the address it
reports, so a second window, a development bridge on 9331, or a bridge left
behind by an earlier run does not stop it. `FYAI_BRIDGE_PORT` pins the port
when something else, such as a browser, has to reach the same bridge.

The bridge stops with the app: the window tells it to quit, and it also
watches the app's process and its standard input, so an app that is killed
rather than closed still leaves nothing behind. A turn that is running when
that happens is stopped too, with the tools it started.

### Open the macOS app

`scripts/package.sh` signs the macOS app ad hoc, so that its signature verifies.
Without that step the packager leaves a signature that no longer matches the
bundle, and macOS reports a downloaded app as damaged. An ad hoc signature is
not a Developer ID, and the app is not notarized, so the first start of a
downloaded app is refused with a message that the developer cannot be
verified. Open it with a right click and Open, or with System Settings,
Privacy & Security, Open Anyway, or remove the quarantine mark:

```sh
xattr -dr com.apple.quarantine fyai.app
```

### Bundle it with fyai

`scripts/package.sh` writes a self-contained app to `dist-app/fyai-linux-<arch>/`.
With a standalone fyai in `fyai-bundle/fyai` it travels inside the bundle and
the app looks for it before the project build and `PATH`:

```sh
scripts/package.sh --bundle-fyai   # build a portable fyai first
scripts/package.sh                 # package the page as it stands
```

`scripts/bundle-fyai.sh` makes that binary for the platform that runs it. The
default mode is `mostly`: the fyai dependencies are static and only the system
libraries are needed (libSystem and the frameworks on macOS, the glibc on
Linux). It runs `scripts/build-static-native.sh` of the fyai repository, the
build that the release workflow and the Docker image also use. `--mode musl`
gives a fully static Linux binary for a musl or an older machine, built with
Docker.

```sh
scripts/bundle-fyai.sh                 # native build, mode mostly
scripts/bundle-fyai.sh --mode musl     # fully static, with Docker
scripts/bundle-fyai.sh --from PATH     # copy a binary you built already
scripts/bundle-fyai.sh --check         # what a bundle would carry
```

The build builds the fyai repository above `web/`; set `FYAI_SRC` for another
checkout. Its files go to `fyai-static-build/` (`FYAI_STATIC_BUILD`).

## Build system

The fyai build has targets for this tree. They build nothing unless you ask
for them, and `FYAI_WEB=ON` makes the page part of a plain `ninja`:

| Target                 | What it does                          |
| ---------------------- | ------------------------------------- |
| `fyai-web-install`     | `npm ci`                              |
| `fyai-web`             | builds the page into `web/dist`       |
| `fyai-web-dev`         | the bridge and Vite, for editing      |
| `fyai-web-test`        | the unit tests                        |
| `fyai-web-turn`        | one turn against fyai's mock provider |
| `fyai-web-e2e`         | the page in a browser against the bridge, fyai and the mock provider |
| `fyai-desktop`         | opens the desktop window              |
| `fyai-desktop-package` | `dist-app/`, with a bundled fyai      |

```sh
ninja -C build fyai-web            # build the page
ninja -C build fyai-web-test       # its tests
ctest --test-dir build -R fyai/web-test
cmake -S . -B build -DFYAI_WEB=ON  # or make it part of the build
```

The targets never write the source tree, which can be read-only. They link
`web/` into `<build>/web` (`fyai-web-stage`, which needs `rsync`) and run npm,
Vite and the packager there. The page is in `<build>/web/dist`, the bundled
fyai in `<build>/web/fyai-bundle`, and the app in `<build>/web/dist-app`. Files are
hard links to the source when both are on one filesystem, else copies. The
links are made again by each target, so an edit of `web/` takes on the next
build, and `fyai-web-dev` serves the copy: restart it after an edit.

The unit tests are registered with CTest only when `FYAI_WEB=ON`, and they
carry the label `web`, so a C-only run stays quick:
`ctest --test-dir build -LE web`.

With `FYAI_WEB=ON`, `make install` also installs the built page and the
bridge under `share/fyai/web`, and a `fyai-web` launcher in `bin`. The launcher
serves the page on `http://127.0.0.1:9331` and runs fyai in the current
directory; it needs only `node` at run time. The page needs node 20 or later
to build: with an older node, `FYAI_WEB=ON` leaves the page out and says so.
Build first and install second (`make && sudo make install`): the page and
`node_modules` are rebuilt only when a source is newer, so an up-to-date build
runs no `npm` as root. When `npm` does run, it runs with the node that
configured the build in front of the `PATH`, whatever `PATH` a root shell has.

`FYAI_DESKTOP=ON` (Linux, with `FYAI_WEB=ON`) also packages the desktop app
and installs it under `share/fyai/desktop`, with a `fyai-desktop` launcher, a
menu entry and an icon. The app is about 350 MB and `npm` downloads Electron
to build it. It carries no fyai: the launcher points it at the fyai installed
beside it. `chrome-sandbox` is installed setuid root, as Electron expects
where the kernel gives a user no namespace of its own. The macOS app is built
by `fyai-desktop-package` and the release workflow, and is not installed.

## Bridge options

| Option              | Environment                   | Default             | Meaning                                 |
| ------------------- | ----------------------------- | ------------------- | --------------------------------------- |
| `--port`            | `FYAI_BRIDGE_PORT`            | `9331`              | listen port; 0 picks a free one         |
| `--host`            | `FYAI_BRIDGE_HOST`            | `127.0.0.1`         | listen address                          |
| `--bin`             | `FYAI_BIN`                    | `fyai`              | the fyai program (`--fyai` on a script) |
| `--cwd`             | `FYAI_BRIDGE_CWD`             | current directory   | project fyai works in                   |
| `--model`           | `FYAI_BRIDGE_MODEL`           |                     | default model of a session              |
| `--parent-pid`      | `FYAI_BRIDGE_PARENT_PID`      |                     | stop when this process stops            |
| `--permission-mode` | `FYAI_PERMISSION_MODE`        | `bypassPermissions` | SDK permission mode; the sandbox confines the tools |
| `--extra-arg`       | `FYAI_BRIDGE_ARGS`            |                     | extra arguments for `fyai claude-sdk`   |
| `--turn-timeout-ms` | `FYAI_BRIDGE_TURN_TIMEOUT_MS` | `0`                 | stop a turn after this; 0 is no limit   |
| `--token`           | `FYAI_BRIDGE_TOKEN`           | random at each start | the token an API request needs          |
| `--allow-origin`    | `FYAI_BRIDGE_ALLOW_ORIGIN`    |                     | other origins that may call it, by comma |
| `--allow-host`      | `FYAI_BRIDGE_ALLOW_HOST`      |                     | other host names a loopback bridge answers |
| `--static`          | `FYAI_BRIDGE_STATIC`          | `dist`              | built page to serve                     |
| `--verbose`         | `FYAI_BRIDGE_VERBOSE`         | off                 | log each process and request            |

The bridge listens on loopback only by default, and it runs tools in the
project without asking. Every API request needs the token of the bridge, which
it prints as a login link when it starts:
`http://127.0.0.1:9331/api/fyai/login?token=...`. Open that link once; the
browser keeps the token as a cookie, and the page loads without it only to say
what is missing. A program sends the token in the `x-fyai-token` header.
`scripts/run.sh --open` and the desktop app open the link for you, and
`scripts/dev.sh` prints the link of the dev server. A request from another
origin or host name is refused. Put a tunnel or a TLS proxy in front of a
bridge on a non-loopback `--host`, because the bridge speaks plain HTTP.

## How a turn becomes a page

| SDK frame                            | Shown as                                    |
| ------------------------------------ | ------------------------------------------- |
| `system`/`init`                      | the model name                              |
| `stream_event` `content_block_delta` | assistant text and thinking, appended live  |
| `assistant` with `text`              | the finished assistant message              |
| `assistant` with `tool_use`          | a tool call card; `Bash`, `Read`, `Write` and `Edit` use the card of the like tool |
| `user` with `tool_result`            | the result of that card; an error shows as a failure |
| `result`                             | token counts and the end of the turn        |

fyai runs the whole tool loop of a turn itself, so one HTTP response carries
several assistant messages. Each becomes its own chat message, and the tool
results sit between them, which is why the page does not run an agent loop of
its own.

## Conversations

fyai holds each conversation as a branch of the project, and the browser keeps
none: the page holds them in memory, lists the branches when it starts and reads
the messages of a conversation when it opens. A conversation of the CLI shows
there too. Deleting or renaming one in the page deletes or renames the branch.
The bridge serves this at `/api/fyai/conversations`. Editing or regenerating a message cuts the
branch back to the message before it runs the new turn, so fyai answers from
the state that the page shows. Forking a conversation is not offered, because a fork has no branch.

## Provider keys

The page holds no key and sends none. fyai reads the key of a provider from its
own configuration (`api_key: {type: env, value: NAME}`), from
`<PROVIDER>_API_KEY` in the environment of the bridge, or from its secret
store. Start the bridge with the variable set:

```sh
ANTHROPIC_API_KEY=sk-ant-... npm run bridge
```

You can also enter a key in Settings, General, Provider keys. The bridge gives
it to `fyai secret set` and keeps nothing. The macOS Keychain keeps it; on
Linux the kernel keyring keeps it until the machine restarts, and a key in the
environment is used before it.

The bridge puts no key in a file, and it takes its own token out of the
environment of the processes it starts. A conversation keeps working when you
switch the model to another provider whose key fyai can find. A remote server
keeps its keys at the server.

The page settings are not keys. The bridge keeps them in
`$XDG_CONFIG_HOME/fyai/web-settings.json` (mode 0600), so a new window, project
or browser has the same ones, and drops any key it is sent.

## Slash commands

fyai's `claude-sdk` sends a `/` line to the model as text, except `/clear`.
This page therefore runs a command itself, as an fyai verb on the
conversation's session branch. It keeps no list of commands: the bridge reads
them from `fyai schema commands --output json` and serves them at
`/api/fyai/commands`, and the `/` picker of the page offers that list. A
command qualifies when it exists both as a verb and as a slash command and
needs no terminal. `/model` and `/cwd` keep the pickers of the page, and
`/clear` starts a new conversation, which is a new fyai session.

The output arrives as one fenced block in a message row, so a table keeps its
columns. A command runs as a child process with the words as argv; no shell
sees them. Anything else that starts with `/` is answered as unknown and does
not reach the model. An fyai without `schema` exports no commands, and the
page then serves none of them.

## What is not wired yet

- **Sandbox**: fyai confines its tools with a sandbox profile, not with a
  question for each call, so the bridge runs fyai in `bypassPermissions`
  and the page shows no prompt. The page does not show the profile yet; run
  `/session lockdown` or `/session profile NAME` (when the command picker lists
  them), or `fyai sandbox profiles` in the project, to change it.
- **Audio and video**: the composer accepts them, but fyai takes images (PNG,
  JPEG, GIF, WebP) and text files only, so the page refuses the message and
  says so. An image reaches a model that takes images; for any other model
  fyai refuses it with its cause.
- **Working directory per conversation**: a bridge works in one project, given
  by `--cwd`; the desktop app opens one window for each project. A conversation
  has no directory of its own.
- **Resume after reload**: a turn in flight is not reattached; the conversation
  is read from fyai again.
- **Fork, import and export of a conversation**: they work on the copy in the
  browser only, so a forked or imported conversation has no branch in fyai.
- **The tool list and the cost of a turn**, a card for `apply_patch`: not shown.
- **Keys of an MCP server**: only provider keys travel from the page. An MCP
  server that needs a token takes it from the fyai configuration.

## Test a turn

These need no provider key: they run fyai against its mock provider.

`server/test-turn.sh` starts the mock and the bridge, then sends one turn and
prints the frames. A prompt of `-` starts them without a turn.

```sh
bash server/test-turn.sh chat_stream.json "hello" conv-1
bash server/test-turn.sh chat_stream.json -
```

`server/e2e-mock.mjs` is the browser test to run: it needs no key and no
network, because the provider is fyai's mock. `ninja fyai-web-e2e` (or `cmake
--build build --target fyai-web-e2e`) builds the page and runs it; by hand,
`node server/e2e-mock.mjs --fyai ../build/fyai` with `FYAI_BRIDGE_STATIC` set to
the built page. `--chrome PATH` names a Chrome, and `--keep` keeps the
directories and the screenshots of a failure.

`server/e2e.mjs` drives the built page in Chrome against a real provider and
waits for the answer:

```sh
node server/e2e.mjs http://127.0.0.1:9331 "hello" "Streaming hello from the mock."
```

Arguments come in pairs of what to type and the text that must appear, so a
conversation is two pairs:

```sh
node server/e2e.mjs http://127.0.0.1:9331 "first" "Alpha beta." "second" "Gamma."
```

The unit tests cover the frame assembly, the prompt and the bridge's
decisions:

```sh
npx vitest --project=unit --run
```

## Layout

```text
web/
├── scripts/run.sh           build and serve the page
├── scripts/dev.sh           bridge and Vite, for editing
├── server/bridge.mjs        HTTP, SSE, fyai processes, catalogue, conversations
├── server/auth.mjs          the token, origin and host rules
├── server/protocol.mjs      branch names, slash commands, the cut of a branch, images
├── server/test-turn.sh      one turn against the mock provider
├── server/e2e-mock.mjs      the page in a browser against the mock provider
├── server/e2e.mjs           the built page in Chrome, with a real provider
├── electron/main.cjs        the desktop app: one window for each project
├── electron/projects.cjs    the project choices that need no Electron
├── tests/fixtures/fake-fyai.mjs   a stand-in for the branch verbs of fyai
├── src/lib/services/backend/   the backend seam and the llama-server backend
├── src/lib/stores/fyai-backend.svelte.ts  the fyai bridge as a backend
├── src/lib/services/fyai.service.ts   turn transport and prompt
├── src/lib/services/fyai.turn.ts      frames to messages
├── src/lib/services/fyai.tools.ts     the cards of the tools
├── src/lib/services/fyai.conversations.ts  the branches as conversations
└── src/…                    the fork of the llama.cpp UI
```

## Upstream

The interface is a fork of `tools/ui` in
[llama.cpp](https://github.com/ggml-org/llama.cpp), MIT licensed. See
`NOTICE.md`.
