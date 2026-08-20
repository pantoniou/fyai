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

## 4. Step 3 - the session that stays open, and a way to write

This is the item that section 3.5 of the gap analysis names as necessary: a
terminal with no way to write to it is worse than no terminal, because a
program that finds a terminal waits for input that never arrives.

**The lifetime.** One invocation. Section 3.4 of the gap analysis states the
rule: a session that lives inside one tool-call loop needs no arena change and
does not break the stateless design. Release every session when the invocation
ends.

**The register.** `struct fyai_tool_job` already holds the context, the control
channel, the process, the descriptors, the event source and the group. Add a
table of live PTY sessions to the context, keyed by a small integer. A `shell`
call with `tty: true` that does not finish inside its yield time returns the
identifier and the output so far, and leaves the session running.

**The tools.** Add `write_stdin` with `session_id`, `chars` and
`yield_time_ms`, as `codex` records it. An empty `chars` polls. Add a
`yield_time_ms` argument to `shell` so that a call can return while the program
continues.

**The keys.** `chars` is bytes and not key names. A model that must send a
control character sends the byte: `` for an interrupt and `` for
escape. Document this, because a model cannot press a key.

## 5. Step 4 - the remaining parity items and the rendering

Parity:

- `shell` and `login`, from `codex.exec_command`. We run `/bin/sh -c`. Most
  interactive programs expect a login shell.

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

## 6. State

Steps 1 and 2 are done. `shell` takes `tty`, `rows` and `cols`; the result is
the scrollback and the visible screen, bounded by `shell/max_output_tokens`;
each line that leaves the screen also reaches the live display; and the size
follows `SIGWINCH`, through the control channel for a forked tool child.

Two items stay open in the tests. The resize case covers the session that runs
in the parent, which the `fyai tool shell` verb uses. The `tty/resize`
notification to a forked tool child needs a case that runs a complete turn with
the mock provider. A `write_stdin` case cannot exist before step 3.

## 7. Order and the review

Each step is one patch series in the order implementation, tests,
documentation. Step 1 folds into the existing PTY commit, because it repairs
that commit. Steps 2 to 4 are new commits. Run the normal suite after each test
patch and the complete ASAN suite on the final patch of each step.
