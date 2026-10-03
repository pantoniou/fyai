# fyai command reference

Generated from `data/commands.yaml` by `fyai help --markdown`.
Do not edit; run `ninja docs-commands` to write it again.

## Global options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show the help, as fyai help does with the words that follow |
| `--version` | print the fyai version |
| `-C`, `--config FILE` | load an explicit configuration file |
| `-e`, `--env FILE` | source a .env file (only the variables that are used) |
| `-m`, `--model MODEL` | model, optionally as provider/model |
| `-b`, `--branch BRANCH` | work on this branch (else $FYAI_BRANCH, else HEAD) |
| `--root HANDLE` | read one exact state by root handle (read-only) |
| `--set KEY=VALUE` | set a configuration key for this run and store it |
| `--get KEY` | print a configuration key as one-line flow |
| `--delete KEY` | delete a configuration key |
| `--transient` | keep the edits and the state of this run in memory only |
| `-k`, `--api-key KEY` | API key (else the PROVIDER_API_KEY variable) |
| `--new` | start a new conversation |
| `--sandbox` | confine shell tools with Landlock (Linux) |
| `--color VALUE` | colour output (auto, off, on) |
| `--theme THEME` | Markdown theme |
| `-i`, `--interactive` | interactive prompt loop |
| `--answer TEXT` | supply an ask_user answer in advance |
| `-d`, `--debug` | increase the debug verbosity |

# Verbs

## fyai help

describe commands and topics

**Usage:** `fyai help [--markdown] [--man] [TOPIC...]`

With no argument, list the commands. With a command path, describe
that command. With a topic name, show the topic.


### Arguments

| Argument | Description |
| --- | --- |
| `TOPIC` | a command path or a topic name |

### Options

| Option | Description |
| --- | --- |
| `--markdown` | write the whole reference as Markdown |
| `--man` | write the whole reference as the manual page fyai(1) |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai help branch new

describe the command that creates a branch

    fyai help refs

explain the reference syntax


## fyai render

render Markdown in the terminal

**Usage:** `fyai render [FILE]`

Read Markdown from a file, or from standard input when no file is
given or the file is `-`, and write it with the renderer of fyai: the
theme, the tables, the fenced code, and the `mermaid` diagrams of the
display configuration. With `display/markdown` off, the source is
written as it is.


### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the Markdown file (default standard input) |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai render README.md

show a file as the conversation shows an answer

    fyai render - <<< '# Title'

render Markdown from standard input


## fyai branch

list, create, and manage branches

**Usage:** `fyai branch {list|new|delete|rename|show|describe} ...`

A branch holds one conversation and the configuration that goes with
it. See `help refs` for the reference syntax.


### Commands

| Command | Description |
| --- | --- |
| `list` | list the branches |
| `new`, `create` | create a branch |
| `delete`, `rm` | delete a branch |
| `rename`, `mv` | rename a branch |
| `show` | show the details of a branch |
| `describe` | set the description of a branch |

With no command, `list` runs.

**See also:** `fyai help checkout`, `fyai help refs`

## fyai branch list

list the branches

**Usage:** `fyai branch list [-a] [BRANCH]`

List the branches. With a name, list only the branches below it.
Sub-agent branches are hidden unless `--all` is given.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | list only the branches below this name |

### Options

| Option | Description |
| --- | --- |
| `-a`, `--all` | include sub-agent branches |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai branch new

create a branch

**Usage:** `fyai branch new NAME [REF]`

**Aliases:** `create`

Create a branch at a reference. The new branch takes the
conversation and the configuration of that reference. In a
session, the session then switches to the new branch.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | name of the new branch |
| `REF` | start point (default the current head) |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai branch new feature main~2

start a branch two turns before the head of main


## fyai branch delete

delete a branch

**Usage:** `fyai branch delete [-f] BRANCH`

**Aliases:** `rm`

Delete a branch. The current branch cannot be deleted. A branch
with branches below it needs `--force`.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch to delete |

### Options

| Option | Description |
| --- | --- |
| `-f`, `--force` | also delete a branch that has branches below it |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai branch rename

rename a branch

**Usage:** `fyai branch rename BRANCH NEW`

**Aliases:** `mv`

Give a branch a new name. Its ref log goes with it.

### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch to rename |
| `NEW` | the new name |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai branch show

show the details of a branch

**Usage:** `fyai branch show [BRANCH]`

Show the details of a branch, by default the current one.

### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch (default the current branch) |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai branch describe

set the description of a branch

**Usage:** `fyai branch describe BRANCH [TEXT...]`

Set the description of a branch. With no text, remove it.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch |
| `TEXT` | the description |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai checkout

switch to a branch or fork a reference

**Usage:** `fyai checkout [-b] BRANCH [START]`

Select a branch and move the stored `HEAD` to it. With `-b`, create
the branch first, at START or at the current head.

In a session, a reference to an earlier state (`main~2`,
`main@{1}`) is not moved: the session starts a new `session/` branch
at that state and switches to it.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch, or a reference in a session |
| `START` | the start point of a branch that -b creates |

### Options

| Option | Description |
| --- | --- |
| `-b`, `--create` | create the branch TARGET first |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai checkout -b fix main@{1}

create the branch fix at the state before the last change


**See also:** `fyai help branch`, `fyai help refs`

## fyai model

show or select the model

**Usage:** `fyai model [MODEL]`

With no name, show the model, its provider, the API grammar, and the
context window. With a name, select that model and store it in the
configuration of the branch. `provider/model` selects the offering of
one provider.


### Arguments

| Argument | Description |
| --- | --- |
| `MODEL` | the model to select |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

**See also:** `fyai help config-paths`

## fyai init

create the arena in this directory

**Usage:** `fyai init [-f] [FILE]`

Create ./.fyai/arena/ and publish its first root. FILE, or
config.yaml when it exists, is stored as the configuration; `--force`
stores it over an existing configuration.


### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the configuration file |

### Options

| Option | Description |
| --- | --- |
| `-f`, `--force` | store the configuration over an existing one |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai dump

write the stored conversation as YAML

**Usage:** `fyai dump [--first N] [--last N] [--range A,B] [--decorate] [WHAT]`

Write the stored conversation as YAML: `state`, the canonical and
provider-independent conversation; `anchors`, the whole turn graph;
or `providers`, the wire streams of each turn. `--first`, `--last`,
and `--range` select turns (from 0, inclusive).


### Arguments

| Argument | Description |
| --- | --- |
| `WHAT` | what to write (state, anchors, providers); default state |

### Options

| Option | Description |
| --- | --- |
| `--first N` | the first N turns |
| `--last N` | the last N turns |
| `--range A,B` | the turns A to B |
| `--decorate` | add comments that name each part |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai import

read a conversation into the branch

**Usage:** `fyai import [-i FILE] [--ignore-compact] [--from SOURCE] [--source-root DIR] [--session ID] [--dry-run] [--list] [--all] [--json]`

Read a textual export from standard input or `--input`, and replay its
publish boundaries into the branch. `--branch` selects an empty
destination. Compaction markers make provider requests;
`--ignore-compact` skips them.

`--from` reads the sessions of another program: `auto`,
`claude-code`, or `codex`. `--list` finds its sessions (`--all` in
every directory), `--session ID` imports one, and `--dry-run` checks a
file without storing it. `--source-root` names the state directory of
a concrete source.


### Options

| Option | Description |
| --- | --- |
| `-i`, `--input FILE` | the input file (default standard input) |
| `--ignore-compact` | skip the compaction markers |
| `--from SOURCE` | the program that wrote the input (fyai, auto, claude-code, codex) |
| `--source-root DIR` | the state directory of the source |
| `--session ID` | the ID of the session to import |
| `--dry-run` | check the input and store nothing |
| `--list` | list the sessions of the source |
| `--all` | list the sessions of every directory |
| `--json` | write the list or the check as JSON |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

**See also:** `fyai help export`

## fyai replay

send the user turns of the branch again

**Usage:** `fyai replay [--ignore-compact]`

Send the user turns of the branch again, against the configuration
and the tools as they are now.


### Options

| Option | Description |
| --- | --- |
| `--ignore-compact` | skip the compaction markers |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai tool

run one built-in tool

**Usage:** `fyai tool NAME [JSON...]`

Run one built-in tool, such as `shell` or `read_file`, in a sandboxed
child and write its result. JSON is the object of arguments; without
it, the object is read from standard input.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the tool |
| `JSON` | the arguments, as a JSON object |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai term

run a terminal that fyai draws

**Usage:** `fyai term [-c COMMAND] [--shell PATH] [-l] [--hold] [--rows N] [--cols N] [--screen FILE]`

Run a shell, or COMMAND, in a full-screen terminal that fyai draws.
`^\ q` leaves it.


### Options

| Option | Description |
| --- | --- |
| `-c`, `--command COMMAND` | the command, as `sh -c` takes it |
| `--shell PATH` | the shell (default $SHELL) |
| `-l`, `--login` | run the shell as a login shell |
| `--hold` | keep the last screen until a key |
| `--rows N` | the rows, for a run with no terminal |
| `--cols N` | the columns, for a run with no terminal |
| `--screen FILE` | write the last screen as text to FILE |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai reset

move the head of the branch to a reference

**Usage:** `fyai reset REF`

**Aliases:** `rewind`

Move the head of the current branch to REF. The previous head stays
in the ref log as `<branch>@{1}`.


### Arguments

| Argument | Description |
| --- | --- |
| `REF` | the new head |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai reset HEAD~2

drop the last exchange


**See also:** `fyai help refs`, `fyai help checkout`

## fyai clear

start a fresh conversation

**Usage:** `fyai clear`

Publish an empty head on the current branch. The earlier turns stay
in the ref log.


### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai api

show or select the API grammar

**Usage:** `fyai api [MODE]`

With no mode, show the API grammar, the model, the provider, the URL,
and max_tokens. With a mode, select that grammar of the provider and
store it in the configuration.


### Arguments

| Argument | Description |
| --- | --- |
| `MODE` | the API grammar (responses, chat-completions, chat, messages) |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai context

show the context fill and the token estimate

**Usage:** `fyai context`

Show the context window of the model and the projected fill of the
next request: the larger of the last measured input and an estimate
of the prompt, and the output allowance.


### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai stats

show the token use and the cost

**Usage:** `fyai stats [--raw]`

Show the token use and the cost, summed over the stored turns of the
branch.


### Options

| Option | Description |
| --- | --- |
| `--raw` | write the Markdown source, not the rendering |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai list

list providers, models, turns, exchanges, or the ref log

**Usage:** `fyai list [--full] [--raw] [WHAT]`

List the configured providers, the models of the catalogue, the
stored turns, the exchanges, or the ref log of the branch.


### Arguments

| Argument | Description |
| --- | --- |
| `WHAT` | what to list (providers, models, turns, exchanges, reflog); default providers |

### Options

| Option | Description |
| --- | --- |
| `--full` | include the details of each item |
| `--raw` | write the Markdown source, not the rendering |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai diff

compare two ref-log entries

**Usage:** `fyai diff [-u] [FROM] [TO]`

Compare the exports of two ref-log entries, by default `HEAD@{1}`
and `HEAD`: the change that the last operation made.


### Arguments

| Argument | Description |
| --- | --- |
| `FROM` | the older entry; default HEAD@{1} |
| `TO` | the newer entry; default HEAD |

### Options

| Option | Description |
| --- | --- |
| `-u`, `--unified` | write unified rows, coloured on a terminal |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

**See also:** `fyai help refs`

## fyai root

print the arena root, a handle for one exact state

**Usage:** `fyai root {print|show} ...`

Print the handle of the current arena root, or of the root that REF
was published in. `--root HANDLE` reads that exact state.


### Commands

| Command | Description |
| --- | --- |
| `print` | print the root handle |
| `show` | show the root and what it holds |

With no command, `print` runs.

## fyai root print

print the root handle

**Usage:** `fyai root print [REF]`

Print the root handle.

### Arguments

| Argument | Description |
| --- | --- |
| `REF` | a ref-log entry |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai root show

show the root and what it holds

**Usage:** `fyai root show [REF]`

Show the root handle and the state it holds.

### Arguments

| Argument | Description |
| --- | --- |
| `REF` | a ref-log entry |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai rebase

put the exchanges of this branch after those of another

**Usage:** `fyai rebase [--allow-unrelated] BRANCH`

Append the exchanges of BRANCH to this branch, then the exchanges of
this branch after them.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the other branch |

### Options

| Option | Description |
| --- | --- |
| `--allow-unrelated` | join branches that have no common base |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

**See also:** `fyai help merge`

## fyai merge

interleave the exchanges of two branches by time

**Usage:** `fyai merge [--allow-unrelated] BRANCH`

Interleave the exchanges of BRANCH with the exchanges of this branch,
in the order in which they happened.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the other branch |

### Options

| Option | Description |
| --- | --- |
| `--allow-unrelated` | join branches that have no common base |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

**See also:** `fyai help rebase`

## fyai gc

collect the garbage of the arena

**Usage:** `fyai gc [--keep-reflogs N]`

Compact the arena. `--keep-reflogs N` also cuts the ref log to the
last N entries.


### Options

| Option | Description |
| --- | --- |
| `--keep-reflogs N` | keep only the last N ref-log entries |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai export

write the conversation in the textual export format

**Usage:** `fyai export [-o FILE] [REF]`

Write the branch in the textual export format that `import` reads,
to standard output or to FILE. REF exports the branch as it was at
that ref-log entry.


### Arguments

| Argument | Description |
| --- | --- |
| `REF` | a ref-log entry |

### Options

| Option | Description |
| --- | --- |
| `-o`, `--file FILE` | the output file |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

**See also:** `fyai help refs`

## fyai log

control the trace logs

**Usage:** `fyai log [TARGET] {show|start|stop|clear|view} ...`

**Aliases:** `logging`

Show, start, stop, clear, or view the logs under .fyai/logs. TARGET is
wire, stream, conversation, mcp, transport, or all. The logs never hold a
credential: the values of key headers, bearer tokens, and the key itself
are whited out when they are written.


### Commands

| Command | Description |
| --- | --- |
| `show` | show which logs are on |
| `start`, `on` | start logging |
| `stop`, `off` | stop logging |
| `clear` | empty the log |
| `view` | view the log |

### Arguments

| Argument | Description |
| --- | --- |
| `TARGET` | the log (wire, stream, conversation, mcp, transport, all); default all |

With no command, `show` runs.

## fyai log show

show which logs are on

**Usage:** `fyai log show`

Show which logs are on.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai log start

start logging

**Usage:** `fyai log start`

**Aliases:** `on`

Start writing the log.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai log stop

stop logging

**Usage:** `fyai log stop`

**Aliases:** `off`

Stop writing the log.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai log clear

empty the log

**Usage:** `fyai log clear`

Remove the content of the log.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai log view

view the log

**Usage:** `fyai log view`

Open the log in a viewer.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai secret

manage secrets without showing their values

**Usage:** `fyai secret {status|set|delete} ...`

Manage logical secrets in the machine-local secret store. A provider
API key is conventionally named `api-key/<provider>`. A value is read
from the terminal with no echo, or from standard input with `--stdin`;
never from the command line.


### Commands

| Command | Description |
| --- | --- |
| `status` | show the backend, or whether a secret exists |
| `set` | store a secret |
| `delete` | remove a secret |

With no command, `status` runs.

**See also:** `fyai help api-key`

## fyai secret status

show the backend, or whether a secret exists

**Usage:** `fyai secret status [NAME]`

Show the secret backend, or whether NAME exists.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the logical name |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai secret set

store a secret

**Usage:** `fyai secret set [--stdin] NAME`

Store the value of NAME, read from the terminal.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the logical name |

### Options

| Option | Description |
| --- | --- |
| `--stdin` | read the value from standard input |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai secret delete

remove a secret

**Usage:** `fyai secret delete NAME`

Remove NAME from the secret store.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the logical name |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai compact

summarize the conversation into a fresh chain

**Usage:** `fyai compact [HINT...]`

Compact the conversation and start a new chain; the old head is kept
as `compacted_from`. The Responses grammar can compact at the server;
the other grammars summarize with one model request. HINT guides the
summary.


### Arguments

| Argument | Description |
| --- | --- |
| `HINT` | what the summary must keep |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config

inspect or change the configuration of the branch

**Usage:** `fyai config {show|effective|get|set|delete|import|export|edit|validate|schema|describe} ...`

The configuration is a document stored with each branch. Paths are
slash-separated keys, and values are YAML flow documents; see
`help config-paths`.


### Commands

| Command | Description |
| --- | --- |
| `show` | write the stored configuration |
| `effective` | write the merged configuration |
| `get` | write the value of a key |
| `set` | set the value of a key |
| `delete` | remove a key |
| `import` | store a configuration file |
| `export` | write the configuration to a file |
| `edit` | edit the configuration in an editor |
| `validate` | check the configuration against the schema |
| `schema` | write the configuration schema |
| `describe` | describe the keys of the configuration |

With no command, `show` runs.

**See also:** `fyai help config-paths`, `fyai help model`

## fyai config show

write the stored configuration

**Usage:** `fyai config show`

Write the configuration document of the branch as YAML.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config effective

write the merged configuration

**Usage:** `fyai config effective`

Write the configuration that this run uses: the branch
configuration, then `--config`, then `--set`. Derived values, such
as the endpoint, are not in it.


### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config get

write the value of a key

**Usage:** `fyai config get KEY`

Write the value of a key as one line of YAML flow.

### Arguments

| Argument | Description |
| --- | --- |
| `KEY` | the configuration path |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config set

set the value of a key

**Usage:** `fyai config set KEY VALUE...`

Set a key to a value. The value is parsed as a YAML flow
document and checked against the configuration schema.


### Arguments

| Argument | Description |
| --- | --- |
| `KEY` | the configuration path |
| `VALUE` | the value, as a YAML flow document |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai config set display/theme 'ember:auto'

select a palette theme


## fyai config delete

remove a key

**Usage:** `fyai config delete KEY`

Remove a key from the configuration of the branch.

### Arguments

| Argument | Description |
| --- | --- |
| `KEY` | the configuration path |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config import

store a configuration file

**Usage:** `fyai config import FILE`

Check a YAML configuration file against the schema and store it as
the configuration of the branch.


### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the YAML file |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config export

write the configuration to a file

**Usage:** `fyai config export [FILE]`

Write the configuration of the branch as YAML to FILE, or to
standard output.


### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the output file |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config edit

edit the configuration in an editor

**Usage:** `fyai config edit`

Edit the configuration with $VISUAL or $EDITOR. A session runs the
editor in the work pane and applies the edit when it ends.


### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config validate

check the configuration against the schema

**Usage:** `fyai config validate`

Check the stored configuration against its JSON Schema.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config schema

write the configuration schema

**Usage:** `fyai config schema`

Write the JSON Schema of the configuration document.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai config describe

describe the keys of the configuration

**Usage:** `fyai config describe [PATH]`

Show a table of the keys below PATH: the type, the constraints,
the default, and the description. With no path, describe the whole
document.


### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the configuration path |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view

create and enter separate project filesystem views

**Usage:** `fyai view {create|list|show|update|mount|unmount|enter} ...`

Capture a project into immutable CAS objects and use a separate writable
OverlayFS view. Commands in a view do not apply changes to the host.
The initial Linux implementation materializes an ordinary-file baseline.


### Commands

| Command | Description |
| --- | --- |
| `create` | capture a project and create a named view |
| `list` | list the filesystem views of the active branch |
| `show` | inspect a named filesystem view |
| `update` | replace a view with a fresh host snapshot |
| `mount` | mount a view for read-only inspection |
| `unmount` | unmount the recorded inspection view |
| `enter` | enter a private filesystem view |

With no command, `list` runs.

## fyai view create

capture a project and create a named view

**Usage:** `fyai view create [--copy-git-objects] [--verify] NAME [PROJECT]`

Capture PROJECT, excluding its reserved .fyai directory, and record
a named view on the active branch. Reject unsupported metadata and
observed concurrent changes. No provider credential is required.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the name of the separate view |
| `PROJECT` | the project directory to capture; default . |

### Options

| Option | Description |
| --- | --- |
| `--copy-git-objects` | copy Git object bytes instead of borrowing host inodes; default false |
| `--verify` | independently verify copied bytes and hashes serially before publication; default false |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view list

list the filesystem views of the active branch

**Usage:** `fyai view list`

Show the project, baseline, result, and state of each view.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view show

inspect a named filesystem view

**Usage:** `fyai view show NAME`

Show a view's baseline and latest recorded result identity.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the name of the view to inspect |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view update

replace a view with a fresh host snapshot

**Usage:** `fyai view update [--copy-git-objects] [--verify] NAME`

Capture the host project again and replace the named view with a fresh
baseline and empty upper. Discard the view's changes without changing
the host project. Refuse the update while the view is in use.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the name of the view to replace |

### Options

| Option | Description |
| --- | --- |
| `--copy-git-objects` | copy Git object bytes instead of borrowing host inodes; default false |
| `--verify` | independently verify copied bytes and hashes serially before publication; default false |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view mount

mount a view for read-only inspection

**Usage:** `fyai view mount NAME PATH`

Mount the named view at an empty directory for inspection and comparison.
The read-only mount persists in the current mount namespace until unmount.
Mount privileges are required; rootless inspection can use an explicitly
started unshare -Urnm sh session. Unmount before entering or updating.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the view to inspect |
| `PATH` | an empty mount directory, created when absent |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view unmount

unmount the recorded inspection view

**Usage:** `fyai view unmount NAME`

Verify the recorded namespace, mount IDs, and source before releasing
the inspection mount. Busy mounts are retained. Run in the namespace
that owns the mount and unmount before leaving a rootless inspection shell.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the mounted view to release |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai view enter

enter a private filesystem view

**Usage:** `fyai view enter [--verify] NAME [COMMAND...]`

Mount the named view in private namespaces and start a shell in its project
directory, or execute the remaining command arguments directly. Standard
input, output, and error are inherited. Record the result in CAS on exit.
The host project stays separate. This operation requires Linux user
namespaces, OverlayFS, and Landlock.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the name of the view to enter |
| `COMMAND` | the command and arguments to execute; omit to start a shell |

### Options

| Option | Description |
| --- | --- |
| `--verify` | independently verify copied bytes and hashes serially before publication; default false |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai sandbox

show or set the stored sandbox policy

**Usage:** `fyai sandbox {show|on|off|edit} ...`

Show or change the `sandbox` key of the configuration. `on` stores a
policy that denies secrets and ~/.ssh and allows port 443; `edit`
edits the whole configuration. In a session, `/sandbox` is the setting
of the session instead.


### Commands

| Command | Description |
| --- | --- |
| `show`, `get` | write the stored sandbox value |
| `on`, `enable` | store the default sandbox policy |
| `off`, `disable` | store no sandbox |
| `edit` | edit the configuration in an editor |

With no command, `show` runs.

**See also:** `fyai help config`

## fyai sandbox show

write the stored sandbox value

**Usage:** `fyai sandbox show`

**Aliases:** `get`

Write the stored sandbox value as YAML.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai sandbox on

store the default sandbox policy

**Usage:** `fyai sandbox on`

**Aliases:** `enable`

Store the default sandbox policy.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai sandbox off

store no sandbox

**Usage:** `fyai sandbox off`

**Aliases:** `disable`

Store `sandbox false`.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai sandbox edit

edit the configuration in an editor

**Usage:** `fyai sandbox edit`

Edit the configuration with $VISUAL or $EDITOR.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog

inspect or change the model catalogue of the branch

**Usage:** `fyai catalog {show|list|tools|get|set|delete|import|export|edit|validate|schema|reset|update} ...`

The catalogue lists the providers, their endpoints, and the models
they offer. Each branch owns its catalogue. A path is slash-separated;
an item of a list is named by its name, such as
`providers/openai/models/gpt-5`.


### Commands

| Command | Description |
| --- | --- |
| `show` | summarize the catalogue |
| `list` | list the models or the providers |
| `tools` | list the tools of a coding agent |
| `get` | write the value at a path |
| `set` | set the value at a path |
| `delete` | remove the value at a path |
| `import` | store a catalogue file |
| `export` | write the catalogue to a file |
| `edit` | edit the catalogue in an editor |
| `validate` | check the catalogue against the schema |
| `schema` | write the catalogue schema |
| `reset` | return to the embedded catalogue |
| `update` | update the catalogue from the providers |

With no command, `show` runs.

**See also:** `fyai help model`, `fyai help config`

## fyai catalog show

summarize the catalogue

**Usage:** `fyai catalog show`

Show a summary of the catalogue of the branch.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog list

list the models or the providers

**Usage:** `fyai catalog list [WHAT]`

List the models, or the providers, of the catalogue.

### Arguments

| Argument | Description |
| --- | --- |
| `WHAT` | what to list (models, providers); default models |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog tools

list the tools of a coding agent

**Usage:** `fyai catalog tools [--full] [AGENT]`

List the tools of the coding agents of the catalogue.

### Arguments

| Argument | Description |
| --- | --- |
| `AGENT` | the agent |

### Options

| Option | Description |
| --- | --- |
| `--full` | show the complete tool descriptions |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog get

write the value at a path

**Usage:** `fyai catalog get PATH`

Write the value at a path of the catalogue as YAML.

### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the catalogue path |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog set

set the value at a path

**Usage:** `fyai catalog set PATH VALUE...`

Set the value at a path. The value is a YAML flow document; the
catalogue is checked against its schema before it is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the catalogue path |
| `VALUE` | the value, as a YAML flow document |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog delete

remove the value at a path

**Usage:** `fyai catalog delete PATH`

Remove the value at a path of the catalogue.

### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the catalogue path |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog import

store a catalogue file

**Usage:** `fyai catalog import FILE`

Check a catalogue file against the schema and store it.

### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the catalogue file |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog export

write the catalogue to a file

**Usage:** `fyai catalog export [FILE]`

Write the catalogue as YAML to FILE, or to standard output.

### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the output file |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog edit

edit the catalogue in an editor

**Usage:** `fyai catalog edit`

Edit the catalogue with $VISUAL or $EDITOR. A session runs the
editor in the work pane and applies the edit when it ends.


### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog validate

check the catalogue against the schema

**Usage:** `fyai catalog validate`

Check the catalogue of the branch against its schema.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog schema

write the catalogue schema

**Usage:** `fyai catalog schema`

Write the JSON Schema of the catalogue.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog reset

return to the embedded catalogue

**Usage:** `fyai catalog reset`

Remove the catalogue of the branch; the embedded one applies.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai catalog update

update the catalogue from the providers

**Usage:** `fyai catalog update [--curated] [--provider NAME] [PROVIDER...]`

Run `catalog_update/command` and store its output. The program
keeps only the credentials that `catalog_update/credentials`
names. A session runs it in the work pane.


### Arguments

| Argument | Description |
| --- | --- |
| `PROVIDER` | update only these providers |

### Options

| Option | Description |
| --- | --- |
| `--curated` | keep only the curated models |
| `--provider NAME` | update only this provider |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai history

show the conversation of the branch

**Usage:** `fyai history [--first N] [--last N] [--range A,B] [--raw] [--tool-detail MODE] [SELECTION...]`

**Aliases:** `transcript`, `display`

Render the stored conversation: the exchanges, the tool calls, and
their results. Select the exchanges with `--first N`, `--last N`, or
`--range A,B` (from 0, inclusive), or in a session with `all`,
`first N`, `last N`, or `range A,B`.

With `--output json` or `--output yaml`, write each stored message as
one document, as it streams.


### Arguments

| Argument | Description |
| --- | --- |
| `SELECTION` | all, first N, last N, or range A,B |

### Options

| Option | Description |
| --- | --- |
| `--first N` | the first N exchanges |
| `--last N` | the last N exchanges |
| `--range A,B` | the exchanges A to B, from 0, inclusive |
| `--raw` | write the Markdown source, not the rendering |
| `--tool-detail MODE` | how much of each tool result to show (none, brief, default, full) |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

### Examples

    fyai history --last 2

show the last two exchanges

    fyai history --output json

write the messages as JSON Lines


**See also:** `fyai help output`

## fyai auth

manage the subscription login of a provider

**Usage:** `fyai auth [PROVIDER] {status|info|usage|login|accounts|logout} ...`

Manage the machine-local ChatGPT subscription credentials. PROVIDER
names the subscription provider; only `openai` is supported.


### Commands

| Command | Description |
| --- | --- |
| `status` | show the login and the health of the credentials |
| `info` | show the subscription and the account |
| `usage` | show recorded usage and subscription settings |
| `login` | sign in to the subscription |
| `accounts` | list saved ChatGPT registrations |
| `logout` | sign out and remove the credentials |

### Arguments

| Argument | Description |
| --- | --- |
| `PROVIDER` | the subscription provider; default openai |

With no command, `status` runs.

**See also:** `fyai help api-key`

## fyai auth status

show the login and the health of the credentials

**Usage:** `fyai auth status`

Show the login and the health of the stored credentials.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai auth info

show the subscription and the account

**Usage:** `fyai auth info`

Show the details of the subscription and the account.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai auth usage

show recorded usage and subscription settings

**Usage:** `fyai auth usage`

Show selected-conversation token totals and the ChatGPT settings link for account limits and credit permissions.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai auth login

sign in to the subscription

**Usage:** `fyai auth login [--device-code] [--account CLIENT_ID] [--new-account] [--no-browser] [--manual]`

Register fyai and authorize ChatGPT plan usage with a browser and
a loopback callback. Reuse the active registration, select one with
`--account`, or add one with `--new-account`. `--no-browser` writes
the URL; `--manual` reads a complete pasted redirect URL. Device-code
registration is not supported. `^C` or Escape cancels.


### Options

| Option | Description |
| --- | --- |
| `--device-code` | reject the unsupported legacy device-code flow |
| `--account CLIENT_ID` | reuse the issued client ID of a saved account |
| `--new-account` | register another ChatGPT account or workspace |
| `--no-browser` | write the URL and start no browser |
| `--manual` | paste the complete redirect URL back without a local browser |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai auth accounts

list saved ChatGPT registrations

**Usage:** `fyai auth accounts`

List saved accounts and workspace registrations without tokens.

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai auth logout

sign out and remove the credentials

**Usage:** `fyai auth logout`

Revoke the login when the provider allows it, and remove the local
tokens. Keep the client registration and host ID for later sign-in.


### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai mcp

inspect and control the MCP servers

**Usage:** `fyai mcp {oauth} ...`

In a session, show the MCP servers, sign in to or out of one, or turn
them all on or off. A verb imports the OAuth client of a server.


### Commands

| Command | Description |
| --- | --- |
| `oauth` | manage the OAuth clients of MCP servers |

## fyai mcp oauth

manage the OAuth clients of MCP servers

**Usage:** `fyai mcp oauth {import-client} ...`

Manage the pre-registered OAuth clients of MCP servers.

### Commands

| Command | Description |
| --- | --- |
| `import-client` | import a pre-registered OAuth client |

## fyai mcp oauth import-client

import a pre-registered OAuth client

**Usage:** `fyai mcp oauth import-client --endpoint URL --scope SCOPE [--secret-env NAME] [--force] NAME FILE`

Import the OAuth client of the server NAME from FILE. The
client secret goes to the machine-local secret store;
`--secret-env` stores a reference to a variable instead.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the server |
| `FILE` | the client file |

### Options

| Option | Description |
| --- | --- |
| `--endpoint URL` | the endpoint of the server |
| `--scope SCOPE` | an OAuth scope to request |
| `--secret-env NAME` | store a reference to this variable |
| `--force` | replace an existing server |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai resume

resume another session

**Usage:** `fyai resume [--all] [--last] [SESSION]`

**Aliases:** `switch`

Continue a stored session, selected for this invocation: HEAD does
not move, and the working directory does not change. SESSION names
it; with none, the picker selects. `--last` resumes the session
updated last. The sessions that started in this directory are
offered; `--all` offers every one.


### Arguments

| Argument | Description |
| --- | --- |
| `SESSION` | the session |

### Options

| Option | Description |
| --- | --- |
| `--all` | offer the sessions of every directory |
| `--last` | resume the session updated last |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai page

show and review the page of the screen

**Usage:** `fyai page {review} ...`

Show the page that draws the screen, or paint and name its areas to
see which part of the page document draws which rows.


### Commands

| Command | Description |
| --- | --- |
| `review` | draw a sample page with its areas painted and named |

## fyai page review

draw a sample page with its areas painted and named

**Usage:** `fyai page review [--width N] [--height N] [--page FILE] [SAMPLE]`

Render the page document with the state SAMPLE, give each area a
colour and its name, and list the areas. The slots are empty: a
sample has no transcript, tiles, or prompt to draw in them. The
document is the embedded one, or the file of --page, which must
load as display/page does.


### Arguments

| Argument | Description |
| --- | --- |
| `SAMPLE` | the state of the page (prompt, inline, question, popup); default prompt |

### Options

| Option | Description |
| --- | --- |
| `--width N` | the columns of the page; default 80 |
| `--height N` | the rows of the page; default 24 |
| `--page FILE` | the page document to review, in place of the embedded one |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai agent

run one sub-agent on a task

**Usage:** `fyai agent [--rpc] [TASK...]`

Run one autonomous sub-agent on TASK and write its final report. The
agent runs its own tool loop with the built-in persona and tools; it
cannot ask questions or start further agents. It shares the workspace,
and stores its run on the branch unless `--transient` is given.

With `--rpc` it takes no task and serves the sub-agent control
protocol, JSON-RPC 2.0 framed by lines, on standard input and output:
initialize, agent/run, and shutdown. See doc/agent-protocol.md.


### Arguments

| Argument | Description |
| --- | --- |
| `TASK` | the task |

### Options

| Option | Description |
| --- | --- |
| `--rpc` | serve the sub-agent control protocol |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai transport

serve model requests for isolated agents

**Usage:** `fyai transport --control-fd FD [--arena DIR]`

Run the credential transport. It holds the provider credentials and sends
the requests of agents that hold none. A supervisor starts it, and it
takes its orders on a control channel: a socket that it inherits on
descriptor `--control-fd`, framed as one JSON message for each datagram.
The protocol is in doc/agent-transport-isolation-sdd.md. It ends when the
channel closes. It sets itself not dumpable, closes every other
descriptor, and leaves the terminal session. A person does not normally run
it.


### Options

| Option | Description |
| --- | --- |
| `--control-fd FD` | the descriptor of the control channel |
| `--arena DIR` | the arena whose logs directory receives the transport log |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

## fyai completion

write a shell completion script

**Usage:** `fyai completion SHELL`

Write a completion script for the shell to standard output. See
`help completion` to load it.


### Arguments

| Argument | Description |
| --- | --- |
| `SHELL` | the shell (bash, zsh, fish) |

### Options

| Option | Description |
| --- | --- |
| `--output FORMAT` | write the result as markdown, json, or yaml; see `help output` |
| `-h`, `--help` | show this help |

# Slash commands

## /help

describe commands and topics

**Usage:** `/help [TOPIC...]`

With no argument, list the commands. With a command path, describe
that command. With a topic name, show the topic.


### Arguments

| Argument | Description |
| --- | --- |
| `TOPIC` | a command path or a topic name |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

### Examples

    /help branch new

describe the command that creates a branch

    /help refs

explain the reference syntax


## /branch

list, create, and manage branches

**Usage:** `/branch {list|new|delete|rename|show|describe|switch|attach|detach} ...`

A branch holds one conversation and the configuration that goes with
it. See `help refs` for the reference syntax.


### Commands

| Command | Description |
| --- | --- |
| `list` | list the branches |
| `new`, `create` | create a branch |
| `delete`, `rm` | delete a branch |
| `rename`, `mv` | rename a branch |
| `show` | show the details of a branch |
| `describe` | set the description of a branch |
| `switch` | switch the session to a branch |
| `attach` | show the screen of a live sub-agent |
| `detach` | leave the screen of a sub-agent |

With no command, `list` runs.

**See also:** `/help checkout`, `/help refs`

## /branch list

list the branches

**Usage:** `/branch list [-a] [BRANCH]`

List the branches. With a name, list only the branches below it.
Sub-agent branches are hidden unless `--all` is given.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | list only the branches below this name |

### Options

| Option | Description |
| --- | --- |
| `-a`, `--all` | include sub-agent branches |
| `-h`, `--help` | show this help |

## /branch new

create a branch

**Usage:** `/branch new NAME [REF]`

**Aliases:** `create`

Create a branch at a reference. The new branch takes the
conversation and the configuration of that reference. In a
session, the session then switches to the new branch.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | name of the new branch |
| `REF` | start point (default the current head) |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

### Examples

    /branch new feature main~2

start a branch two turns before the head of main


## /branch delete

delete a branch

**Usage:** `/branch delete [-f] BRANCH`

**Aliases:** `rm`

Delete a branch. The current branch cannot be deleted. A branch
with branches below it needs `--force`.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch to delete |

### Options

| Option | Description |
| --- | --- |
| `-f`, `--force` | also delete a branch that has branches below it |
| `-h`, `--help` | show this help |

## /branch rename

rename a branch

**Usage:** `/branch rename BRANCH NEW`

**Aliases:** `mv`

Give a branch a new name. Its ref log goes with it.

### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch to rename |
| `NEW` | the new name |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /branch show

show the details of a branch

**Usage:** `/branch show [BRANCH]`

Show the details of a branch, by default the current one.

### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch (default the current branch) |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /branch describe

set the description of a branch

**Usage:** `/branch describe BRANCH [TEXT...]`

Set the description of a branch. With no text, remove it.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch |
| `TEXT` | the description |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /branch switch

switch the session to a branch

**Usage:** `/branch switch BRANCH`

Switch the session to a branch, and create the branch when it does
not exist. The conversation and the configuration of the branch
replace those of the session. `/branch NAME` is the same command.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /branch attach

show the screen of a live sub-agent

**Usage:** `/branch attach AGENT`

Give the tile of a running sub-agent the pane and the keys.

### Arguments

| Argument | Description |
| --- | --- |
| `AGENT` | the branch of the sub-agent |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /branch detach

leave the screen of a sub-agent

**Usage:** `/branch detach`

Return the keys from an attached sub-agent to the prompt.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /checkout

switch to a branch or fork a reference

**Usage:** `/checkout [-b] BRANCH [START]`

Select a branch and move the stored `HEAD` to it. With `-b`, create
the branch first, at START or at the current head.

In a session, a reference to an earlier state (`main~2`,
`main@{1}`) is not moved: the session starts a new `session/` branch
at that state and switches to it.


### Arguments

| Argument | Description |
| --- | --- |
| `BRANCH` | the branch, or a reference in a session |
| `START` | the start point of a branch that -b creates |

### Options

| Option | Description |
| --- | --- |
| `-b`, `--create` | create the branch TARGET first |
| `-h`, `--help` | show this help |

### Examples

    /checkout -b fix main@{1}

create the branch fix at the state before the last change


**See also:** `/help branch`, `/help refs`

## /model

show or select the model

**Usage:** `/model [MODEL]`

With no name, show the model, its provider, the API grammar, and the
context window. With a name, select that model and store it in the
configuration of the branch. `provider/model` selects the offering of
one provider.


### Arguments

| Argument | Description |
| --- | --- |
| `MODEL` | the model to select |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

**See also:** `/help config-paths`

## /reset

move the head of the branch to a reference

**Usage:** `/reset REF`

**Aliases:** `rewind`

Move the head of the current branch to REF. The previous head stays
in the ref log as `<branch>@{1}`.


### Arguments

| Argument | Description |
| --- | --- |
| `REF` | the new head |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

### Examples

    /reset HEAD~2

drop the last exchange


**See also:** `/help refs`, `/help checkout`

## /clear

start a fresh conversation

**Usage:** `/clear`

Publish an empty head on the current branch. The earlier turns stay
in the ref log.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /api

show or select the API grammar

**Usage:** `/api [MODE]`

With no mode, show the API grammar, the model, the provider, the URL,
and max_tokens. With a mode, select that grammar of the provider and
store it in the configuration.


### Arguments

| Argument | Description |
| --- | --- |
| `MODE` | the API grammar (responses, chat-completions, chat, messages) |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /context

show the context fill and the token estimate

**Usage:** `/context`

Show the context window of the model and the projected fill of the
next request: the larger of the last measured input and an estimate
of the prompt, and the output allowance.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /stats

show the token use and the cost

**Usage:** `/stats`

Show the token use and the cost, summed over the stored turns of the
branch.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /list

list providers, models, turns, exchanges, or the ref log

**Usage:** `/list [--full] [WHAT]`

List the configured providers, the models of the catalogue, the
stored turns, the exchanges, or the ref log of the branch.


### Arguments

| Argument | Description |
| --- | --- |
| `WHAT` | what to list (providers, models, turns, exchanges, reflog); default providers |

### Options

| Option | Description |
| --- | --- |
| `--full` | include the details of each item |
| `-h`, `--help` | show this help |

## /diff

compare two ref-log entries

**Usage:** `/diff [-u] [FROM] [TO]`

Compare the exports of two ref-log entries, by default `HEAD@{1}`
and `HEAD`: the change that the last operation made.


### Arguments

| Argument | Description |
| --- | --- |
| `FROM` | the older entry; default HEAD@{1} |
| `TO` | the newer entry; default HEAD |

### Options

| Option | Description |
| --- | --- |
| `-u`, `--unified` | write unified rows, coloured on a terminal |
| `-h`, `--help` | show this help |

**See also:** `/help refs`

## /log

control the trace logs

**Usage:** `/log [TARGET] {show|start|stop|clear|view} ...`

**Aliases:** `logging`

Show, start, stop, clear, or view the logs under .fyai/logs. TARGET is
wire, stream, conversation, mcp, transport, or all. The logs never hold a
credential: the values of key headers, bearer tokens, and the key itself
are whited out when they are written.


### Commands

| Command | Description |
| --- | --- |
| `show` | show which logs are on |
| `start`, `on` | start logging |
| `stop`, `off` | stop logging |
| `clear` | empty the log |
| `view` | view the log |

### Arguments

| Argument | Description |
| --- | --- |
| `TARGET` | the log (wire, stream, conversation, mcp, transport, all); default all |

With no command, `show` runs.

## /log show

show which logs are on

**Usage:** `/log show`

Show which logs are on.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /log start

start logging

**Usage:** `/log start`

**Aliases:** `on`

Start writing the log.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /log stop

stop logging

**Usage:** `/log stop`

**Aliases:** `off`

Stop writing the log.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /log clear

empty the log

**Usage:** `/log clear`

Remove the content of the log.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /log view

view the log

**Usage:** `/log view`

Open the log in a viewer.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /secret

manage secrets without showing their values

**Usage:** `/secret {status|set|delete} ...`

Manage logical secrets in the machine-local secret store. A provider
API key is conventionally named `api-key/<provider>`. A value is read
from the terminal with no echo, or from standard input with `--stdin`;
never from the command line.


### Commands

| Command | Description |
| --- | --- |
| `status` | show the backend, or whether a secret exists |
| `set` | store a secret |
| `delete` | remove a secret |

With no command, `status` runs.

**See also:** `/help api-key`

## /secret status

show the backend, or whether a secret exists

**Usage:** `/secret status [NAME]`

Show the secret backend, or whether NAME exists.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the logical name |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /secret set

store a secret

**Usage:** `/secret set NAME`

Store the value of NAME, read from the terminal.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the logical name |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /secret delete

remove a secret

**Usage:** `/secret delete NAME`

Remove NAME from the secret store.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the logical name |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /compact

summarize the conversation into a fresh chain

**Usage:** `/compact [HINT...]`

Compact the conversation and start a new chain; the old head is kept
as `compacted_from`. The Responses grammar can compact at the server;
the other grammars summarize with one model request. HINT guides the
summary.


### Arguments

| Argument | Description |
| --- | --- |
| `HINT` | what the summary must keep |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config

inspect or change the configuration of the branch

**Usage:** `/config {show|effective|get|set|delete|import|export|edit|validate|schema|describe} ...`

The configuration is a document stored with each branch. Paths are
slash-separated keys, and values are YAML flow documents; see
`help config-paths`.


### Commands

| Command | Description |
| --- | --- |
| `show` | write the stored configuration |
| `effective` | write the merged configuration |
| `get` | write the value of a key |
| `set` | set the value of a key |
| `delete` | remove a key |
| `import` | store a configuration file |
| `export` | write the configuration to a file |
| `edit` | edit the configuration in an editor |
| `validate` | check the configuration against the schema |
| `schema` | write the configuration schema |
| `describe` | describe the keys of the configuration |

With no command, `show` runs.

**See also:** `/help config-paths`, `/help model`

## /config show

write the stored configuration

**Usage:** `/config show`

Write the configuration document of the branch as YAML.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config effective

write the merged configuration

**Usage:** `/config effective`

Write the configuration that this run uses: the branch
configuration, then `--config`, then `--set`. Derived values, such
as the endpoint, are not in it.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config get

write the value of a key

**Usage:** `/config get KEY`

Write the value of a key as one line of YAML flow.

### Arguments

| Argument | Description |
| --- | --- |
| `KEY` | the configuration path |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config set

set the value of a key

**Usage:** `/config set KEY VALUE...`

Set a key to a value. The value is parsed as a YAML flow
document and checked against the configuration schema.


### Arguments

| Argument | Description |
| --- | --- |
| `KEY` | the configuration path |
| `VALUE` | the value, as a YAML flow document |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

### Examples

    /config set display/theme 'ember:auto'

select a palette theme


## /config delete

remove a key

**Usage:** `/config delete KEY`

Remove a key from the configuration of the branch.

### Arguments

| Argument | Description |
| --- | --- |
| `KEY` | the configuration path |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config import

store a configuration file

**Usage:** `/config import FILE`

Check a YAML configuration file against the schema and store it as
the configuration of the branch.


### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the YAML file |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config export

write the configuration to a file

**Usage:** `/config export [FILE]`

Write the configuration of the branch as YAML to FILE, or to
standard output.


### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the output file |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config edit

edit the configuration in an editor

**Usage:** `/config edit`

Edit the configuration with $VISUAL or $EDITOR. A session runs the
editor in the work pane and applies the edit when it ends.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config validate

check the configuration against the schema

**Usage:** `/config validate`

Check the stored configuration against its JSON Schema.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config schema

write the configuration schema

**Usage:** `/config schema`

Write the JSON Schema of the configuration document.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /config describe

describe the keys of the configuration

**Usage:** `/config describe [PATH]`

Show a table of the keys below PATH: the type, the constraints,
the default, and the description. With no path, describe the whole
document.


### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the configuration path |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog

inspect or change the model catalogue of the branch

**Usage:** `/catalog {show|list|tools|get|set|delete|import|export|edit|validate|schema|reset|update} ...`

The catalogue lists the providers, their endpoints, and the models
they offer. Each branch owns its catalogue. A path is slash-separated;
an item of a list is named by its name, such as
`providers/openai/models/gpt-5`.


### Commands

| Command | Description |
| --- | --- |
| `show` | summarize the catalogue |
| `list` | list the models or the providers |
| `tools` | list the tools of a coding agent |
| `get` | write the value at a path |
| `set` | set the value at a path |
| `delete` | remove the value at a path |
| `import` | store a catalogue file |
| `export` | write the catalogue to a file |
| `edit` | edit the catalogue in an editor |
| `validate` | check the catalogue against the schema |
| `schema` | write the catalogue schema |
| `reset` | return to the embedded catalogue |
| `update` | update the catalogue from the providers |

With no command, `show` runs.

**See also:** `/help model`, `/help config`

## /catalog show

summarize the catalogue

**Usage:** `/catalog show`

Show a summary of the catalogue of the branch.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog list

list the models or the providers

**Usage:** `/catalog list [WHAT]`

List the models, or the providers, of the catalogue.

### Arguments

| Argument | Description |
| --- | --- |
| `WHAT` | what to list (models, providers); default models |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog tools

list the tools of a coding agent

**Usage:** `/catalog tools [--full] [AGENT]`

List the tools of the coding agents of the catalogue.

### Arguments

| Argument | Description |
| --- | --- |
| `AGENT` | the agent |

### Options

| Option | Description |
| --- | --- |
| `--full` | show the complete tool descriptions |
| `-h`, `--help` | show this help |

## /catalog get

write the value at a path

**Usage:** `/catalog get PATH`

Write the value at a path of the catalogue as YAML.

### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the catalogue path |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog set

set the value at a path

**Usage:** `/catalog set PATH VALUE...`

Set the value at a path. The value is a YAML flow document; the
catalogue is checked against its schema before it is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the catalogue path |
| `VALUE` | the value, as a YAML flow document |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog delete

remove the value at a path

**Usage:** `/catalog delete PATH`

Remove the value at a path of the catalogue.

### Arguments

| Argument | Description |
| --- | --- |
| `PATH` | the catalogue path |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog import

store a catalogue file

**Usage:** `/catalog import FILE`

Check a catalogue file against the schema and store it.

### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the catalogue file |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog export

write the catalogue to a file

**Usage:** `/catalog export [FILE]`

Write the catalogue as YAML to FILE, or to standard output.

### Arguments

| Argument | Description |
| --- | --- |
| `FILE` | the output file |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog edit

edit the catalogue in an editor

**Usage:** `/catalog edit`

Edit the catalogue with $VISUAL or $EDITOR. A session runs the
editor in the work pane and applies the edit when it ends.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog validate

check the catalogue against the schema

**Usage:** `/catalog validate`

Check the catalogue of the branch against its schema.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog schema

write the catalogue schema

**Usage:** `/catalog schema`

Write the JSON Schema of the catalogue.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog reset

return to the embedded catalogue

**Usage:** `/catalog reset`

Remove the catalogue of the branch; the embedded one applies.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /catalog update

update the catalogue from the providers

**Usage:** `/catalog update [--curated] [--provider NAME] [PROVIDER...]`

Run `catalog_update/command` and store its output. The program
keeps only the credentials that `catalog_update/credentials`
names. A session runs it in the work pane.


### Arguments

| Argument | Description |
| --- | --- |
| `PROVIDER` | update only these providers |

### Options

| Option | Description |
| --- | --- |
| `--curated` | keep only the curated models |
| `--provider NAME` | update only this provider |
| `-h`, `--help` | show this help |

## /history

show the conversation of the branch

**Usage:** `/history [--first N] [--last N] [--range A,B] [--tool-detail MODE] [SELECTION...]`

**Aliases:** `transcript`, `display`

Render the stored conversation: the exchanges, the tool calls, and
their results. Select the exchanges with `--first N`, `--last N`, or
`--range A,B` (from 0, inclusive), or in a session with `all`,
`first N`, `last N`, or `range A,B`.

With `--output json` or `--output yaml`, write each stored message as
one document, as it streams.


### Arguments

| Argument | Description |
| --- | --- |
| `SELECTION` | all, first N, last N, or range A,B |

### Options

| Option | Description |
| --- | --- |
| `--first N` | the first N exchanges |
| `--last N` | the last N exchanges |
| `--range A,B` | the exchanges A to B, from 0, inclusive |
| `--tool-detail MODE` | how much of each tool result to show (none, brief, default, full) |
| `-h`, `--help` | show this help |

### Examples

    /history --last 2

show the last two exchanges

    /history --output json

write the messages as JSON Lines


**See also:** `/help output`

## /auth

manage the subscription login of a provider

**Usage:** `/auth [PROVIDER] {status|info|usage|login|accounts|logout} ...`

Manage the machine-local ChatGPT subscription credentials. PROVIDER
names the subscription provider; only `openai` is supported.


### Commands

| Command | Description |
| --- | --- |
| `status` | show the login and the health of the credentials |
| `info` | show the subscription and the account |
| `usage` | show recorded usage and subscription settings |
| `login` | sign in to the subscription |
| `accounts` | list saved ChatGPT registrations |
| `logout` | sign out and remove the credentials |

### Arguments

| Argument | Description |
| --- | --- |
| `PROVIDER` | the subscription provider; default openai |

With no command, `status` runs.

**See also:** `/help api-key`

## /auth status

show the login and the health of the credentials

**Usage:** `/auth status`

Show the login and the health of the stored credentials.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /auth info

show the subscription and the account

**Usage:** `/auth info`

Show the details of the subscription and the account.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /auth usage

show recorded usage and subscription settings

**Usage:** `/auth usage`

Show selected-conversation token totals and the ChatGPT settings link for account limits and credit permissions.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /auth login

sign in to the subscription

**Usage:** `/auth login [--device-code] [--account CLIENT_ID] [--new-account] [--no-browser] [--manual]`

Register fyai and authorize ChatGPT plan usage with a browser and
a loopback callback. Reuse the active registration, select one with
`--account`, or add one with `--new-account`. `--no-browser` writes
the URL; `--manual` reads a complete pasted redirect URL. Device-code
registration is not supported. `^C` or Escape cancels.


### Options

| Option | Description |
| --- | --- |
| `--device-code` | reject the unsupported legacy device-code flow |
| `--account CLIENT_ID` | reuse the issued client ID of a saved account |
| `--new-account` | register another ChatGPT account or workspace |
| `--no-browser` | write the URL and start no browser |
| `--manual` | paste the complete redirect URL back without a local browser |
| `-h`, `--help` | show this help |

## /auth accounts

list saved ChatGPT registrations

**Usage:** `/auth accounts`

List saved accounts and workspace registrations without tokens.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /auth logout

sign out and remove the credentials

**Usage:** `/auth logout`

Revoke the login when the provider allows it, and remove the local
tokens. Keep the client registration and host ID for later sign-in.


### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /mcp

inspect and control the MCP servers

**Usage:** `/mcp {status|login|logout|on|off} ...`

In a session, show the MCP servers, sign in to or out of one, or turn
them all on or off. A verb imports the OAuth client of a server.


### Commands

| Command | Description |
| --- | --- |
| `status`, `show` | show the MCP servers and their state |
| `login` | sign in to an MCP server |
| `logout` | sign out of an MCP server |
| `on` | turn the MCP servers on |
| `off` | turn the MCP servers off |

With no command, `status` runs.

## /mcp status

show the MCP servers and their state

**Usage:** `/mcp status`

**Aliases:** `show`

Show the live MCP connections, or the configured servers.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /mcp login

sign in to an MCP server

**Usage:** `/mcp login NAME`

Start the OAuth sign-in of the server NAME.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the server |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /mcp logout

sign out of an MCP server

**Usage:** `/mcp logout NAME`

Remove the credentials of the server NAME.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the server |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /mcp on

turn the MCP servers on

**Usage:** `/mcp on`

Turn the MCP servers on and store `mcp/enabled`.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /mcp off

turn the MCP servers off

**Usage:** `/mcp off`

Turn the MCP servers off and store `mcp/enabled`.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /btw

ask a side question in a tile

**Usage:** `/btw QUESTION...`

Ask QUESTION on a side branch, `agent:btw-N`, forked from the head,
while a turn runs. The answer stays in its tile until Escape, the
close button, or `/kill`.


### Arguments

| Argument | Description |
| --- | --- |
| `QUESTION` | the question |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /branches

open the branch browser

**Usage:** `/branches`

Open the browser of the branches of the arena.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /resume

resume another session

**Usage:** `/resume [--all] [SESSION]`

**Aliases:** `switch`

Continue a stored session, selected for this invocation: HEAD does
not move, and the working directory does not change. SESSION names
it; with none, the picker selects. `--last` resumes the session
updated last. The sessions that started in this directory are
offered; `--all` offers every one.


### Arguments

| Argument | Description |
| --- | --- |
| `SESSION` | the session |

### Options

| Option | Description |
| --- | --- |
| `--all` | offer the sessions of every directory |
| `-h`, `--help` | show this help |

## /zoom

give a live tile the pane and the keys

**Usage:** `/zoom [NAME]`

Give the tile NAME, or the newest tile, the pane and the keys, to
type into a shell or a sub-agent. `Ctrl-]` comes back; `/zoom off`
shows every tile again.


### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the tile, or off |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /page

show and review the page of the screen

**Usage:** `/page {show|review} ...`

Show the page that draws the screen, or paint and name its areas to
see which part of the page document draws which rows.


### Commands

| Command | Description |
| --- | --- |
| `show` | show the page of the screen and its state |
| `review` | paint and name the areas of the screen |

With no command, `show` runs.

## /page show

show the page of the screen and its state

**Usage:** `/page show`

Show the page document in use, and the state, source, and regions of the last frame.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /page review

paint and name the areas of the screen

**Usage:** `/page review [ON|OFF]`

Give each area of the live page a colour and write its name at its
top left: each slot, and each row by the flag that shows it or the
page it comes from. Without ON or OFF the review is turned over.


### Arguments

| Argument | Description |
| --- | --- |
| `ON|OFF` | turn the review on or off (on, off) |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /sessions

list the live shell sessions and sub-agents

**Usage:** `/sessions`

List the live shell sessions and sub-agents of the work pane.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /kill

stop a shell session or sub-agent

**Usage:** `/kill NAME`

Stop the live shell session or sub-agent NAME.

### Arguments

| Argument | Description |
| --- | --- |
| `NAME` | the session or sub-agent |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /status

show the model, provider, login, and use

**Usage:** `/status`

Show the model, the provider, the login, and the token use of the session.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /profiles

list the egress profiles of the credential transport

**Usage:** `/profiles [--agent AGENT]`

List the egress profiles of the credential transport and whether the
agent may use each one. The agent is the session itself, or with
`--agent` a sub-agent that it started, by the name it was delegated
under, its branch, or its transport execution number. A credential
source is named, never its value.


### Options

| Option | Description |
| --- | --- |
| `--agent AGENT` | list for this sub-agent instead of the session |
| `-h`, `--help` | show this help |

## /usage

show recorded usage and subscription settings

**Usage:** `/usage`

Show selected-conversation token totals and the ChatGPT settings link for account limits and credit permissions.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /tools

list the tools of a coding agent

**Usage:** `/tools [--full] [AGENT]`

List the tools of the coding agents of the catalogue; `/catalog tools` is the same.

### Arguments

| Argument | Description |
| --- | --- |
| `AGENT` | the agent |

### Options

| Option | Description |
| --- | --- |
| `--full` | show the complete tool descriptions |
| `-h`, `--help` | show this help |

## /exit

leave the session

**Usage:** `/exit`

**Aliases:** `quit`

Leave the session.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /reload

restart the current session

**Usage:** `/reload`

Commit the current branch and restart fyai on it. Live shells and sub-agents must finish or close first. A transient session and a pinned root cannot be reloaded.

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /reasoning-effort

the reasoning effort of the model

**Usage:** `/reasoning-effort [VALUE]`

**Aliases:** `effort`

With no value, show the reasoning effort of the model. With a value,
change it; the configuration schema gives the values. `reasoning/effort` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /reasoning-summary

the reasoning summary that the model writes

**Usage:** `/reasoning-summary [VALUE]`

**Aliases:** `summary`

With no value, show the reasoning summary that the model writes. With a value,
change it; the configuration schema gives the values. `reasoning/summary` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /temperature

the sampling temperature

**Usage:** `/temperature [VALUE]`

With no value, show the sampling temperature. With a value,
change it; the configuration schema gives the values. `temperature` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /layout

the layout of the page, or auto

**Usage:** `/layout [VALUE]`

With no value, show the layout of the page. With a value, change it:
a layout of the page document by name, or auto for the first that the
terminal is large enough for. `display/page_layout` is the key; a
session-scoped key changes for this session only, any other key is
stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /theme

the Markdown theme, name[:auto|dark|light]

**Usage:** `/theme [VALUE]`

With no value, show the Markdown theme, name[:auto|dark|light]. With a value,
change it; the configuration schema gives the values. `display/theme` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /tool-detail

how much of each tool result to show

**Usage:** `/tool-detail [VALUE]`

With no value, show how much of each tool result to show. With a value,
change it; the configuration schema gives the values. `display/tool_detail` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /transcript-system

show the system messages in transcripts

**Usage:** `/transcript-system [VALUE]`

With no value, show show the system messages in transcripts. With a value,
change it; the configuration schema gives the values. `display/transcript_system` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /markdown

render Markdown

**Usage:** `/markdown [VALUE]`

With no value, show render Markdown. With a value,
change it; the configuration schema gives the values. `display/markdown` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /stream

stream the response as it arrives

**Usage:** `/stream [VALUE]`

With no value, show stream the response as it arrives. With a value,
change it; the configuration schema gives the values. `display/stream` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /thinking

show the reasoning of the model

**Usage:** `/thinking [VALUE]`

With no value, show show the reasoning of the model. With a value,
change it; the configuration schema gives the values. `display/thinking` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /print-stats

show the token use at the end of a run

**Usage:** `/print-stats [VALUE]`

With no value, show show the token use at the end of a run. With a value,
change it; the configuration schema gives the values. `display/stats` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /sandbox

confine the shell tools

**Usage:** `/sandbox [VALUE]`

With no value, show confine the shell tools. With a value,
change it; the configuration schema gives the values. `sandbox/enabled` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

## /token-extents

record the extents of streamed tokens

**Usage:** `/token-extents [VALUE]`

With no value, show record the extents of streamed tokens. With a value,
change it; the configuration schema gives the values. `token_extents` is the
key; a session-scoped key changes for this session only, any other
key is stored.


### Arguments

| Argument | Description |
| --- | --- |
| `VALUE` | the new value |

### Options

| Option | Description |
| --- | --- |
| `-h`, `--help` | show this help |

# Topics

## refs

A reference names one stored state of a branch. Only symbolic
references exist:

- `<branch>`: the head of the branch. `HEAD` names the current branch.
- `<branch>~N`: N stored turns before the head. One exchange of a user
  and the assistant is normally two turns.
- `<branch>^`, `<branch>^^`: one caret for each turn back.
- `<branch>@{N}`: the Nth earlier entry of the ref log of the branch.
  `<branch>@{1}` is the state before the last change.

A branch name is slash-separated: `feature/x` is below `feature`.
Names that start with `session/` are interactive sessions; a name that
holds `agent:` is the conversation of a sub-agent.

## config-paths

A configuration path is a slash-separated list of keys, such as
`display/theme` or `reasoning/effort`. A value is a YAML flow
document: `on`, `42`, `'a string'`, `[a, b]`, `{k: v}`.

`data/config.schema.yaml` defines the keys and their types;
`fyai config describe [PATH]` shows them.

## api-key

fyai never stores a raw API key. The `api_key` setting names the
environment variable that holds the key:

    api_key: {type: env, value: OPENAI_API_KEY}

Without a setting, fyai reads `<PROVIDER>_API_KEY`. `--api-key` gives
a key for one invocation only.

## output

Each verb that is defined in the command registry accepts
`--output FORMAT`. `markdown`, the default, renders the result.
`json` writes it as one compact JSON document, and `yaml` as a YAML
document. A command that streams writes one document for each item:
JSON Lines, or a YAML stream.

## completion

`fyai completion bash`, `zsh`, or `fish` writes a script that
completes fyai in that shell. Load it from the start-up file:

    source <(fyai completion bash)      # ~/.bashrc
    source <(fyai completion zsh)       # ~/.zshrc
    fyai completion fish | source       # config.fish

The script asks `fyai __complete` for the candidates, so branches,
models, and configuration paths come from the arena.
