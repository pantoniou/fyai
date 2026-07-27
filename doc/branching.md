# Branching

`fyai` keeps more than one line of work in one arena. Each line of work is a
**branch**. A branch holds its own conversation and its own configuration.

## 1. Why branches

An arena holds one conversation chain today. To try a different approach, you
must clear the conversation or make a second project directory. Both methods
lose work.

A branch keeps each line of work separate. You can start a new approach, go back
to the first one, and compare the two.

`fyai` is not `git`, but the two are near. `git` must write files to the disk
when you change branch. `fyai` has no files to write. A branch change is only a
change of one reference. The arena keeps the data of all branches in one
content-addressed store, thus a new branch uses almost no space.

There are three important differences from `git`:

- **The configuration is part of the branch.** In `git`, the configuration is a
  property of the repository. In `fyai`, each branch has its own model, API
  mode, reasoning options and tool settings. Two branches are fully
  independent.
- **There are no numeric references.** You always identify state by name. Refer
  to section 5.
- **Branches nest.** A name is a path, thus a branch can hold branches below
  it. Sub-agent runs will use this. Refer to section 7.

## 2. Concepts

| `git` term | Meaning in `fyai` |
| --- | --- |
| Branch | A named conversation with its own configuration |
| `HEAD` | The name of the branch that the invocation uses |
| Commit | A turn: one model call and its tool calls |
| Commit chain | The turn chain, linked through `previous` |
| Start point | The turn at which a new branch starts |
| Reflog | The history of the tip of a branch |
| Working tree | Not applicable. `fyai` has no files to synchronize |

The default branch is `main`. `fyai init` makes it.

## 3. Selection of the branch

`fyai` finds the branch of an invocation in this sequence. The first item that
gives a name wins.

1. The `--branch NAME` or `-b NAME` option.
2. The `FYAI_BRANCH` environment variable.
3. The `/branch NAME` command, for the remainder of an interactive session.
4. The `HEAD` value in the arena.
5. `main`.

If you give a name that does not exist, `fyai` makes that branch. The new branch
starts with no turns and takes a copy of the configuration of the current
branch.

## 4. Names of branches

A branch name is a path. Use the `/` character to make a hierarchy:

```
main
main/experiment
main/explore-1
main/explore-1/grep-1
```

A branch is the parent of a second branch when its name is a prefix of the name
of the second branch. There is no other structure.

A name must obey these rules:

- The name is not empty.
- The name does not start or end with `/`, and does not contain `//`.
- The name does not contain a space or a control character.
- The name does not contain `~`, `^`, `@`, `:`, `?`, `*`, `[` or `\`.
- No component of the name is `.` or `..`.
- The name is not `HEAD`.

## 5. References

A reference identifies a turn. All references are symbolic.

| Form | Meaning |
| --- | --- |
| `<branch>` | The tip of the branch |
| `<branch>~N` | N turns before the tip |
| `<branch>^` | One turn before the tip; `^^` is two, and so on |
| `<branch>@{N}` | The tip as it was N entries before, in the reflog |
| `HEAD`, `HEAD~N`, `HEAD@{N}` | The same, for the current branch |

Examples:

```
main            # the newest turn on main
main~3          # three turns before that
main@{1}        # the tip of main before the last change
```

**A turn is one message, not one exchange.** `fyai list turns` shows the unit:
a question and its answer are two turns, and the initial system message is one
more. Thus, to go back one full exchange, use `~2`. Use `fyai list turns` to
count before you make a branch.

There are no numeric or hexadecimal references. This is deliberate. The `gc`
command moves objects in the arena and writes new addresses, thus an address is
not stable. A name stays correct after `gc`. If you give a numeric reference,
`fyai` refuses it and shows the symbolic forms.

### 5.1 The ref log

Every operation on a branch appends an entry to that branch's own ref log. An
entry keeps the head, the configuration, the time, the operation that made it
and, for a rename, the name the branch had before.

```sh
fyai list reflog            # the ref log of the current branch
fyai -b exp list reflog     # the ref log of another branch
```

```
 Index │ Ref       │ Kind     │ From │ Model │ When
───────┼───────────┼──────────┼──────┼───────┼──────────────────────
     0 │ flank@{0} │ rename   │ side │ m1    │ 2026-07-27T10:06:31Z
     1 │ flank@{1} │ describe │      │ m1    │ 2026-07-27T10:06:31Z
     2 │ flank@{2} │ checkout │      │ m1    │ 2026-07-27T10:06:31Z
     3 │ flank@{3} │ create   │      │ m1    │ 2026-07-27T10:06:31Z
```

The operation is **stored, not calculated**. A comparison of the head of an
entry with the head of the entry before it cannot tell you what happened: a
reset moves the head backwards and looks the same as a turn, and a rename does
not move the head at all and looks the same as a configuration change.

**The index is a position, not a name.** `@{0}` is always the newest entry,
thus every index moves by one each time an operation adds an entry. Use an
index immediately. To keep a point permanently, make a branch at it:

```sh
fyai branch create keepme "main@{3}"
```

A branch name in a reference is resolved when you give it. No reference is kept
in the arena, thus a rename cannot make a stored reference wrong. Refer to
section 9.

## 6. Commands

### 6.1 The `branch` verb

```sh
fyai branch                          # list the branches
fyai branch --all                    # also list the sub-agent branches
fyai branch create <name> [<start>]  # make a branch
fyai branch delete <name>            # delete a branch
fyai branch rename <old> <new>       # rename a branch
fyai branch show [<name>]            # show the details of a branch
fyai branch describe <name> [<text>] # set or clear the description
```

`fyai branch` shows the name, the number of turns, the model, the time of the
last change, and a mark on the current branch.

`fyai branch create` uses the start point for the **whole** state: the new
branch gets the conversation *and* the configuration that were in force there,
as a start point does in `git`. Without a start point the current branch is the
start point, which is what `git branch <new>` does with `HEAD`.

For `<branch>@{N}` the configuration comes from that ref-log entry, thus a
branch made at a ref-log entry restores the settings of that moment as well as
the turns.

`fyai branch delete` refuses to delete the current branch, and refuses to delete
the last branch. Use `--force` to delete a branch that has turns. Its children
move to its parent; children of a top-level branch move to the empty root.
Deletion fails if reparenting would overwrite another branch.

`fyai branch rename` changes the names of the children of the branch. If you
rename the current branch, `fyai` also changes `HEAD`. A renamed or reparented
branch gets a ref-log entry that keeps the name it had before.

Nothing in the arena holds a branch name except `HEAD` and the keys of the
`branches` mapping, and a rename writes both. A ref-log entry holds the head,
the configuration and the entry before it, all as direct references. Thus a
rename cannot leave a stale reference, and the full ref log of a branch stays
usable under the new name.

`fyai branch describe` sets a free-text line that says what the branch is for.
`git` has no place to keep this. `fyai` has one arena and one atomic reference,
thus the description is part of the branch. Give no text to clear it.

### 6.2 The `reset` verb

```sh
fyai reset HEAD^        # one turn back
fyai reset HEAD~4       # four turns back
fyai reset main@{2}     # to what the head was two operations ago
fyai reset other        # to the tip of another branch
```

`reset` moves the head of the current branch. **Nothing is discarded.** The head
that was there stays in the ref log of the branch, thus `<branch>@{1}`
immediately after a reset gives it back:

```sh
fyai reset HEAD^^       # went back too far
fyai reset main@{1}     # undo that
```

Only `gc --keep-reflogs N` finally removes an entry that is out of the window,
and with it any turn that no branch and no kept entry holds.

### 6.3 The `checkout` verb

```sh
fyai checkout <name>                # change to a branch
fyai checkout -b <name>             # make a branch and change to it
fyai checkout -b <name> <start>     # make it at a start point, and change to it
```

The form with a start point is one step, as `git checkout -b <new> <start>` is.
It is the same as `fyai branch create <name> <start>` and then
`fyai checkout <name>`.

A change of branch writes the new name to `HEAD` in the arena. The next
invocation uses that branch.

### 6.4 The `/branch` command

In an interactive session:

```
/branch                        list the branches
/branch <name>                 change to a branch, and make it if necessary
/branch new <name> [<start>]   make a branch
/branch delete <name>          delete a branch
/branch rename <old> <new>     rename a branch
/branch describe <name> <text> set the description
```

A change of branch in a session applies the configuration of the new branch
immediately. `fyai` resolves the model, the API mode and the API key again. The
banner shows the name of the current branch.

### 6.5 Views

`--branch` is a global option, thus it goes before the verb. Use it to examine
a branch that is not the current branch, without a change of `HEAD`.

```sh
fyai --branch exp transcript
fyai -b exp dump state
fyai -b exp list reflog
fyai -b exp config get model
```

`list reflog` shows the ref log of the selected branch. Each row gives a
`<branch>@{N}` reference that you can use as a start point.

## 7. Sub-agent branches

**This function is not available yet.** The design is given here because the
name format and the storage format are already in place, and the branch entry
has the `agent` field that holds the data.

The plan is that each sub-agent call makes a branch below the branch that
started it:

```
main
main/explore-1
main/explore-1/grep-1
main/plan-2
```

The name of the branch contains the name of the agent and a number.

**The model selects the name of the agent, thus it is arbitrary text and cannot
be used as it stands.** `fyai` reduces the name to one safe path component: it
makes the name lower case, changes each other character to `-`, joins and
removes the runs of `-`, and limits the length. If nothing usable stays, the
component becomes `agent`. Thus a name such as `Code Reviewer` becomes
`code-reviewer`, and a name of punctuation only cannot make an invalid branch
name or add a level to the path.

A number is then added, the smallest that is free below that parent, thus two
calls of the same agent cannot collide.

The current sub-agent cannot delegate to another sub-agent. Therefore, an
agent branch has one component below the branch that started it. The
`agent/max_branch_depth` configuration key remains a defensive limit for
callers of the branch allocator and for a future recursive implementation.

The branch holds the full conversation of the sub-agent, and stays in the arena
after the agent stops, thus you can examine what the agent did:

```sh
fyai branch --all
fyai transcript --branch main/explore-1
```

The work that remains is at the process boundary. A sub-agent runs in a forked
child process that speaks JSON-RPC to its parent (refer to
`doc/agent-protocol.md`). The child must not write to the durable arena: the
arena is a shared mapping across the `fork`, thus a commit from the child is
not safe. The conversation must therefore go back to the parent through the
protocol, and the parent must commit it. This is a change to the protocol and
is done separately.

Until then, a sub-agent conversation is not kept, and `fyai branch --all` shows
the same list as `fyai branch`.

## 8. Examples

Try a different model on the same problem:

```sh
fyai "refactor the parser"
fyai branch create alt main        # fork at the newest turn
fyai checkout alt
fyai --set model=claude-opus-5     # only the alt branch changes
fyai "try again, but keep the old API"
```

Recover the state at a ref-log entry as a new branch:

```sh
$ fyai list reflog          # find the entry
 Index │ Ref      │ Kind   │ Model │ When
───────┼──────────┼────────┼───────┼─────────────────────────────
     0 │ main@{0} │ config │ m3    │ ...
     1 │ main@{1} │ config │ m2    │ ...
$ fyai checkout -b recovered main@{1}
```

The new branch has the conversation *and* the configuration of that entry.

**Read the index and use it immediately.** Every operation adds an entry, and
`branch create` is an operation, thus a second `main@{1}` after the first one
does not mean the same entry. To keep a point, make a branch at it.

Go back three turns and take a different direction:

```sh
fyai branch create retry main~3
fyai checkout retry
```

Make a branch that the arena does not keep:

```sh
fyai --transient -b scratch "what does this macro do?"
```

Examine what a sub-agent did:

```sh
fyai branch --all
fyai transcript --branch main/explore-1
```

## 9. Storage format

The arena root is a container mapping. Version 2 adds the branches:

```yaml
fyai: 2
catalog: <mapping|null>       # for all branches
HEAD: main                    # the name of the current branch
branches:
  main:
    config: <mapping|null>
    head:   <turn|null>
    created: <timestamp>
    description: <string|null>
    op:     <string>              # what made this entry
    from:   <string|null>         # the previous name, on a rename
    prev:   <branch-entry|null>
  main/explore-1:
    config: <mapping|null>
    head:   <turn|null>
    created: <timestamp>
    agent:  { persona: <string>, parent_turn: <turn> }
    op:     <string>
    prev:   <branch-entry|null>
prev: <root|null>             # the reflog of the arena
```

The `config` and `head` keys are in the branch entry. The catalogue stays at the
root level, because it is an immutable copy of provider data and not a statement
of intent.

There are two reflog chains. The `prev` key of the root links to the previous
root, and gives the history of the full arena. The `prev` key of a branch entry
links to the previous entry of that same branch, and gives the history of that
branch. The `<branch>@{N}` reference uses the second chain.

An entry holds no name. This is what makes a rename safe: to move a branch,
`fyai` changes the key in the `branches` mapping and, if necessary, `HEAD`. The
chain of entries below it is not touched and stays correct. The `op` and `from`
keys are a record of what happened, and are not used to find anything.

Each publish operation makes a new branch entry for one branch only, and keeps
the other branches by reference. Thus a change to one branch does not touch the
data of the other branches.

## 10. Compatibility

Version 2 of the root is not compatible with version 1. An older arena does not
open. Use `fyai init` to make a new arena.

An older `fyai` program cannot read a version 2 arena.

## 11. Limits

These functions are not available now:

- Sub-agent branches. Refer to section 7. The name rules are in place, but the
  conversation of a sub-agent is not yet kept.
- Automatic tracking of the branch of a `git` repository.
- A merge or a rebase of conversations.
- Synchronization with a remote arena.
