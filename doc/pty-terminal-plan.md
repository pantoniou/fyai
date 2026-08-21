# Plan: the PTY terminal session

This plan takes the PTY work from the single `tty` boolean that `tools: add
PTY-backed shell execution` adds. It makes a terminal subsystem that a model
can use. It records the order of the work, the decisions that are made, and the
tests for each step.

Refer to `doc/tool-gap-analysis.md` section 3 for the comparison with
`claude_code`, `codex` and `opencode`. This plan is the answer to section 3.5.

## 1. The state after the first series

`shell` accepts `tty: true`. The command then runs on a 24x80 pseudo-terminal.
`libfyvterm` interprets the byte stream, and the tool result is the visible
screen.

The parts that are correct and stay:

- `openpty()`, `setsid()`, `TIOCSCTTY` and `TIOCSWINSZ` in the child.
- `fyai_close_fds_from(3)` and `fyai_env_sanitize()` before `exec`.
- `chdir` before the sandbox, and the group terminate on the deadline.

The parts that are not correct are in section 2. They are folded into the
commit that introduced them and not added as later fixes.

## 2. Step 1 - make the PTY path equal to the pipe path

The `tty` branch of `fyai_run_shell_command()` returns before the result
pipeline. Thus it loses behaviour that the pipe path supplies. Each item below
is a defect against the current tool contract.

**Scrollback.** `tty_snapshot()` reads the 24 visible rows. A command that
prints 400 lines returns the last 24 and loses the rest. Take the `sb_pushline`
callback of `struct fyvt_screen_callbacks`, and keep the lines that leave the
top of the screen. Put them before the visible screen in the result. The
retained line count is the output limit, so a program that does not stop still
uses bounded memory.

**The output limit.** The branch does not call `fyai_shell_output_bytes()`.
Apply the same budget to the assembled text, with `fyai_shell_bound_alloc()`,
so that the `max_output_tokens` equivalent holds for both paths.

**Live output.** The branch supplies no `shell_output_fn`, so the user sees
an empty band while the command runs. Raw PTY bytes cannot go to the display,
because they hold cursor movements. Send a line to `fyai_shell_output()` when
the line leaves the screen, which is the same `sb_pushline` moment. Send the
final screen when the command ends. The user then sees interpreted text and
never an escape sequence. Close the live region with
`fyai_shell_live_close()`.

**Binary output.** Apply `data_is_binary()` to the assembled text and keep the
`binary output: N bytes` summary.

**The result shape.** A native `shell_call` result must carry
`{type: timeout, timeout_ms}` and keep the captured output. The branch writes
`tool error: command timed out` instead. Report a workdir failure as status
125 and an interrupt as an interrupt, as the pipe path does.

**The alternate screen.** A program that enters the alternate screen and exits
restores the primary screen, thus the result is usually empty. Keep the last
alternate screen and supply it when the command ended on it. `sb_pushline` is
not called for the alternate screen, which makes the two cases separable.

**The grace period.** `tty_deadline()` sends `SIGTERM` and `SIGKILL` with no
interval. Use `fyai_event_add_child_terminate_group()`, which already supplies
both delays, so that a program can restore the terminal.

**Style.** Use kernel C style, as `CLAUDE.md` states: one statement to a
line, no declaration inside a loop, no operation inside an error-check
predicate, and no empty `if` body. Use the `libfyaml` UTF-8 encoder in place of
the local one.

**Tests.** Extend `tests/cases/shell_tty.sh` with four commands: one that
prints more rows than the screen, one that ends on the alternate screen, one
that exceeds the output budget, and one that times out. Add unit cases for the
screen assembly in `tests/fyai_vterm_test.c`.

## 3. Step 2 - the terminal size and SIGWINCH

A fixed 24x80 is wrong in both directions: it is smaller than the window the
user has, and it does not follow a change.

**Where the size comes from.** In this order:

1. the `rows` and `cols` arguments of the tool call, if the model gives them;
2. `shell/tty_rows` and `shell/tty_cols` in the configuration; and
3. the size of the real terminal, from `markdown_render_height()` and
   `markdown_render_width()`, when standard output is a terminal.

A non-interactive invocation has no terminal, thus the last item gives 24x80.
Record the chosen size in the tool header row beside `workdir` and `timeout`.

**Why a signal cannot arrive at the session.** The tool child calls `setsid()`.
Thus it leaves the session of the real terminal and the kernel does not send it
`SIGWINCH`. The signal reaches the parent, which is the process in the
foreground process group.

**The propagation path.** The parent already owns a JSON-RPC control channel to
the tool child on descriptors 3 and 4.

1. The parent registers `SIGWINCH` on its event loop with
`fyai_event_add_signal()`. The handler records the new size and wakes the loop.
It does no work in signal context.
2. The parent reads the new size with `TIOCGWINSZ`. It sends a `tty/resize`
notification with `{rows, cols}` to the child of each active job.
3. The child applies `TIOCSWINSZ` to the PTY master, which makes the kernel
send `SIGWINCH` to the program. It calls `fyvt_set_size()`, so that the
interpretation has the size that the program now draws.

A job that has no PTY discards the notification. A size that did not change
sends nothing. Coalesce a burst of changes, because a window drag sends many.

**What resizing costs.** `fyvt_screen_enable_reflow()` enables reflow, so a
line that a narrower screen cannot hold is laid out again over more rows. A row
that leaves the top during the reflow arrives through the scrollback callback
as a continuation. It is joined to the line that it belongs to, so the retained
text stays correct. The alternate screen never reflows,
which libfyvterm carries as a fix from Neovim.

**Tests.** Add a PTY functional case. It runs a program that reports `tput
cols` when it receives `SIGWINCH`. It then resizes the outer PTY and confirms
that the new width appears in the result. Add a unit case for `fyvt_set_size()`
with
scrollback retention. `CLAUDE.md` requires a resize test under a PTY, so it
belongs with the other libfyvterm cell tests.

## 4. Step 3 - the session that stays open - done

A session is one program on a terminal, addressed by name.

**A name, not a number.** `shell` takes `name` beside `description`, the way
`agent` does. A name makes the shell addressable; without one the call is the
one-shot of step 1, unchanged. The name is reserved in the parent before the
process is spawned and is unique among the live sessions of the branch. A name
that is taken is refused, so that the model chooses another one instead of
addressing the wrong shell. `codex` uses a number for this; a name says what
the shell is for and survives being read by a person.

**Each shell is a process.** The tool child that starts a session stays alive
as its driver. It opens the pseudo-terminal, starts the program, and moves
bytes; it interprets nothing and links no libfyvterm. Thus the confinement, the
environment, the descriptors and the process group are exactly those of any
other command this program runs.

**The parent renders.** The terminal state of each session is a
`fyai_terminal_view` in the parent, fed by each byte that the driver sends. The
last screen and the log therefore stay readable after the process stops. A
model needs them when a program ends while a call drives it.

**The wire carries text when it is text.** Output goes up as a
`shell/output` notification. It is `{text}` when the bytes are valid UTF-8 with
no control character other than tab, newline and return. It is `{data}` with
base64 when they are not. A program that writes lines is therefore readable on
the wire and in a trace, and a program that draws still crosses it whole. The
test is made for each chunk, so a character that is split between two reads
sends only that chunk as base64.

**The tools.**

| Tool | What it does |
| --- | --- |
| `shell` with `name` | Opens the session. It answers when the session exists. |
| `shell_input` | Types into it. The text is keystrokes, so `0x03` and escape work. `enter` adds the return key. |
| `shell_output` | Reads it: `new`, `screen`, `all` or `region`. It types nothing, so a read of a build costs nothing. |
| `shell_close` | Ends it. The program is asked to stop first, and killed if it does not stop. |

The three reading tools run in the parent, where the terminal state is. None of
them waits for a program, thus nothing is serialised for long, and the sessions
themselves run in parallel.

**What ends a session.** The program ends, `shell_close` ends it, the idle
limit expires, or the invocation ends. `shell/session_timeout_ms` is idle time
and not total time: a session that is being driven does not die, and one the
model forgot does. An interrupted turn ends every session at once, because a
session is a program the user is watching.

A signal reaches the driver and not the program, because the program leads
its own group. The driver therefore stops the program when a call asks it to
stop. Without that the parent waits for a child that does not stop.

## 5. Step 4 - what is left

- `shell` and `login`, from `codex.exec_command`. We run `/bin/sh -c`. Most
  interactive programs expect a login shell.
- Escape as the stop that waits, apart from the interrupt that does not.

The rendering work is done. One screen row is not one line, because the
screen stores a long line as several rows. `sb_pushline4` reports each row
after the first as a continuation, so the rows are joined again and the result
stays searchable. `fyvt_state_get_lineinfo()` does the same for the visible
screen. A cell carries a base character and its combining characters, and a
double-width glyph leaves the next cell unused. That filler cell is stepped
over and does not become a space. `fyvt_set_utf8()` makes all of this correct.
`fyvt_create()` starts in 8-bit mode, where each byte above 0x7f is a separate
character and all text outside ASCII is mangled.

`run_in_background` from `claude_code.Bash` stays out. It needs a job that a
later invocation finds, which is the second row of section 3.4 and needs an
arena decision first. Model control of the sandbox also stays out: the SRD
makes command admission a matter of policy.

## 6. The terminal for the user - done

`fyai term` is the same view, drawn for a user and not for a model, and
libfytimui draws it. The verb owns no part of the terminal. It publishes cells
to a surface, and the library composes that surface with the transcript, the
work bands and the prompt.

- `fyai_ui_surface_*` is the only path from fyai to a surface, because
`src/fyai_ui.c` is the file that speaks to the library. `fyai_term.c` owns the
pseudo-terminal, the event sources and the keys, and it writes to no descriptor
of its own. `tests/sink-only-allow.txt` therefore needs no entry for it.
- The program has the rows that the band granted, read back with
`fytim_surface_granted_rows()`. A screen larger than the surface would keep its
first rows out of sight.
- The keys belong to the surface while the program runs. libfytimui encodes
them into the bytes that a terminal sends and delivers them as an event.
Escape and ^C go to the program, and `^\` is the key of fyai.

This is the path a watched sub-agent will use. The only difference for an
agent is that its surface does not hold the keys.

The library gained four things for this, with its own tests in that tree: a
cell surface, keys handed to a surface, the commit, and the shedding of grid
rows before chrome. A defect in the core came out of it as well. The text path
dropped a combining mark everywhere, and not only on a surface.

### The window changes size

Nothing here watches `SIGWINCH`. Standard output is a spool pipe while the
display is open, so the verb cannot read the size from it. The display samples
the terminal at each frame. `fyai_ui_size()` reports what it found.

The rows need both readings, and they must not chase each other:

- The height of the window gives the growth. The verb asks for it one time
for each size that the window takes, because a surface never receives more rows
than its grid has. To follow the grant alone would never grow the surface.
- The grant of the band is the ceiling, and the verb believes it only after a
frame is drawn at the new size. A grant that is read before that would say
"shrink back" immediately, and the size would never change.

A terminal is a live view. The loop waits for a bounded time, so it follows
a window that changes while no byte arrives and no key is pressed. A publish
asks for a frame with `fyai_ui_wake()`, and no render path pumps the
display.

### Leaving

The verb asks the program to stop, and then makes it stop. It sends SIGHUP
first, because that is the end of a terminal, and an interactive shell stops on
it. Such a shell ignores SIGTERM, which is what the leave key meets in normal
use. It sends SIGTERM next, and SIGKILL after the grace time, for a program
that reads neither.

The loop keeps running until the program stops, because the timer that makes
it stop lives on that loop. To end the loop at the key press leaves a wait for
a child that does not stop, and that wait holds the whole process. The end is
the reaping of the program and never the end of the stream. A descendant of the
program can hold the terminal open.

### The keys arrive in the order they were typed

A frame of the display answered only one question: was this key pressed. One
field kept the last key of the frame, and typed text was collected apart from
it. Both results reached this verb. `^\ q` typed as one burst sent the `q` to
the program and kept the `^\`. Two presses of one key inside a frame became one
press.

The core now keeps the input of a frame in the order of arrival, and the
encoder walks that log. The program therefore receives what the user typed. The
aggregates that a widget reads do not change.

## 7. State

Steps 1 to 3 are done.

A command uses a terminal in one of two ways, and the reading follows what it
did and not what it was called:

- it writes text and newlines, and a caller reads it as whole lines that are
not wrapped; or
- it draws, and a caller reads it as the screen that it drew.

The `fyai_terminal_view` keeps both readings from the same bytes and enters
screen mode the first time the program addresses the screen. `fyvt_set_utf8()`
is what makes any of it correct: `fyvt_create()` starts in 8-bit mode, where
every byte above 0x7f is a separate character.

One item stays open in the tests: the `tty/resize` notification to a forked
tool child. The resize case covers the session that runs in the parent, which
the `fyai tool shell` verb uses.

## 8. Order and the review

Each step is one patch series in the order implementation, tests,
documentation. Step 1 folds into the existing PTY commit, because it repairs
that commit. Steps 2 to 4 are new commits. Run the normal suite after each test
patch and the complete ASAN suite on the final patch of each step.
