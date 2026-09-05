#!/usr/bin/env python3
"""Drive one fyai interactive session through a real PTY."""
import fcntl
import os
import select
import struct
import sys
import termios
import time


def input_row_holds(data, text, rows, cols, marker="❯"):
    """Whether the live input row holds @text.

    Byte counts cannot answer this: every earlier appearance of the
    text (typed echo, queued redisplay, transcript scrollback) is
    already in the capture. Only the screen model sees frames: replay
    the capture and read the row behind the prompt marker.

    An empty row behind the marker counts as not holding: the recall
    clears the input before it redisplays the text, so a gate that
    accepts the cleared frame submits into the same window the gate
    exists to close.
    """
    sys.path.insert(0, os.path.join(
        os.path.dirname(os.path.abspath(__file__))))
    from screen import Screen
    screen = Screen(rows, cols)
    screen.feed(data)
    held = False
    for row in screen.display():
        if not row.startswith(marker):
            continue
        # The input row is modeled; an empty one is the cleared frame
        # between the interrupt and the redisplay, never the commit.
        if not row[len(marker):].strip():
            return False
        held = text.decode() in row
    return held


def read_until_input(fd, data, text, rows, cols, deadline,
                     marker="❯"):
    """Read until the live input row holds @text.

    The marker defaults to the built-in prompt; a case with a custom
    display/prompt passes its own.
    """
    while (not input_row_holds(data, text, rows, cols, marker) and
            time.monotonic() < deadline):
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    if not input_row_holds(data, text, rows, cols, marker):
        raise RuntimeError(
            "PTY input never redisplayed %r; tail=%r" %
            (text, data[-2000:]))
    return data


def read_until(fd, data, needle, deadline):
    while needle not in data and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    if needle not in data:
        raise RuntimeError("PTY output never contained %r; tail=%r" %
                           (needle, data[-2000:]))
    return data


def read_until_count(fd, data, needle, count, deadline):
    while data.count(needle) < count and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        data += chunk
    if data.count(needle) < count:
        # Carry what arrived: a caller that retries the submit on a
        # miss must keep the interim bytes in the capture, not drop
        # them with the exception.
        err = RuntimeError(
            "PTY output showed %r %d of %d times; tail=%r; "
            "full capture in the output file" %
            (needle, data.count(needle), count, data[-2000:]))
        err.partial = data
        raise err
    return data


def timeout_scale():
    """How much longer every deadline waits.

    Deadlines only: a value that paces the driver, such as how long it waits
    before it types, is a step of the case and not a budget for one.

    A deadline is a guess about a machine. A sanitized build, or a suite run
    with many jobs at once, takes several times as long for the same work, and
    a case that waits for output must wait that much longer rather than report
    a failure that says nothing about the code. $FYAI_TIMEOUT_SCALE carries it,
    and CMake sets $FYAI_TIMEOUT_SCALE_DEFAULT for the sanitized tree.
    """
    try:
        scale = float(os.environ.get(
            "FYAI_TIMEOUT_SCALE",
            os.environ.get("FYAI_TIMEOUT_SCALE_DEFAULT", "1")))
    except ValueError:
        return 1.0
    return scale if scale > 0 else 1.0


def check_after_script(after_script, snapshot):
    """Reject a malformed AFTER script before the session is forked.

    A typo here otherwise fails minutes later, mid-case, with a capture
    that describes the symptom and not the misspelled step. Worse, a
    dropped step silently changes what the case proves.
    """
    kinds = ("send", "raw", "resize", "wait", "wait-frame", "drain", "settle", "snapshot")
    for step in after_script:
        kind, _, value = step.partition(":")
        if kind not in kinds:
            raise RuntimeError("unknown FYAI_PTY_AFTER step: %r" % step)
        if kind in ("send", "wait", "wait-frame") and not value:
            raise RuntimeError(
                "FYAI_PTY_AFTER step %r needs a value" % step)
        if kind == "raw":
            try:
                bytes.fromhex(value)
            except ValueError:
                raise RuntimeError(
                    "FYAI_PTY_AFTER step %r is not hex" % step)
        if kind == "resize":
            dims = value.split("x") if "x" in value else [value]
            try:
                if (len(dims) > 2 or
                        any(int(d) <= 0 for d in dims)):
                    raise ValueError
            except ValueError:
                raise RuntimeError(
                    "FYAI_PTY_AFTER step %r is not COLS or ROWSxCOLS" %
                    step)
        if kind in ("drain", "settle"):
            try:
                if float(value) < 0:
                    raise ValueError
            except ValueError:
                raise RuntimeError(
                    "FYAI_PTY_AFTER step %r is not seconds" % step)
        if kind == "snapshot" and not snapshot:
            raise RuntimeError(
                "FYAI_PTY_AFTER snapshot step without FYAI_PTY_SNAPSHOT")


def main():
    output, *argv = sys.argv[1:]
    scale = timeout_scale()
    prompt = os.environ.get("FYAI_PTY_INPUT", "hello").encode()
    needle = os.environ.get(
        "FYAI_PTY_NEEDLE", "Streaming hello from the mock.").encode()
    needle_count = int(os.environ.get("FYAI_PTY_NEEDLE_COUNT", "1"))
    progress_needle = os.environ.get("FYAI_PTY_PROGRESS_NEEDLE", "").encode()
    progress_timeout = float(
        os.environ.get("FYAI_PTY_PROGRESS_TIMEOUT", "1.5")) * scale
    mid_needle = os.environ.get("FYAI_PTY_MID_NEEDLE", "").encode()
    mid_timeout = float(os.environ.get("FYAI_PTY_MID_TIMEOUT", "3")) * scale
    interrupt_after_progress = os.environ.get(
        "FYAI_PTY_INTERRUPT_AFTER_PROGRESS", "0") in ("1", "true", "yes")
    interrupt_key = os.environ.get("FYAI_PTY_INTERRUPT_KEY", "escape")
    during_input = os.environ.get("FYAI_PTY_DURING_INPUT", "").encode()
    during_delay = float(os.environ.get("FYAI_PTY_DURING_DELAY", "0.2"))
    during_submit = os.environ.get(
        "FYAI_PTY_DURING_SUBMIT", "1") not in ("0", "false", "no")
    interrupt_after_during = os.environ.get(
        "FYAI_PTY_INTERRUPT_AFTER_DURING", "0") in ("1", "true", "yes")
    interrupt_delay = float(
        os.environ.get("FYAI_PTY_INTERRUPT_DELAY", "0.2"))
    submit_recalled = os.environ.get(
        "FYAI_PTY_SUBMIT_RECALLED", "0") in ("1", "true", "yes")
    interrupt_settled_needle = os.environ.get(
        "FYAI_PTY_INTERRUPT_SETTLED_NEEDLE", "").encode()
    clear_before_exit = os.environ.get(
        "FYAI_PTY_CLEAR_BEFORE_EXIT", "0") in ("1", "true", "yes")
    # A file to poll before sending the interrupt below: the PTY shows
    # the turn is busy but not that the call being interrupted started.
    # An MCP handshake outlasts the fixed interrupt delay, so the
    # interrupt lands pre-dispatch and nothing recalls. The mock log
    # records tools/call when the call is truly in flight.
    wait_file = os.environ.get("FYAI_PTY_WAIT_FILE", "")
    wait_file_needle = os.environ.get("FYAI_PTY_WAIT_FILE_NEEDLE", "")
    resize_cols = int(os.environ.get("FYAI_PTY_RESIZE_COLS", "0"))
    edit_input = os.environ.get(
        "FYAI_PTY_EDIT_INPUT", "0") in ("1", "true", "yes")
    edit_needle = os.environ.get(
        "FYAI_PTY_EDIT_NEEDLE", "edited prompt").encode()
    rows = int(os.environ.get("FYAI_PTY_ROWS", "30"))
    cols = int(os.environ.get("FYAI_PTY_COLS", "100"))
    session_timeout = float(os.environ.get("FYAI_PTY_TIMEOUT", "15")) * scale
    expected_status = int(os.environ.get("FYAI_PTY_EXIT_STATUS", "0"))
    # Post-turn PTY actions, separated by "|":
    #
    #   send:TEXT   type TEXT and submit it
    #   raw:HEX     write these bytes and submit nothing (a control key)
    #   resize:N    make the window N columns wide
    #   wait:TEXT   read until TEXT is on the capture
    #   wait-frame:TEXT  require TEXT after the last action and a frame end
    #   drain:SEC   keep reading for SEC seconds
    #   settle:SEC  keep reading until no output arrives for SEC seconds
    #               (quiescence); the wait deadline still bounds it
    #
    # Scale wait deadlines, but not explicit action pauses.
    after_script = [step for step in
                    os.environ.get("FYAI_PTY_AFTER", "").split("|") if step]
    after_timeout = float(os.environ.get("FYAI_PTY_AFTER_TIMEOUT", "5")) * scale
    after_pause = float(os.environ.get("FYAI_PTY_AFTER_PAUSE", "0.3"))
    snapshot = os.environ.get("FYAI_PTY_SNAPSHOT", "")
    snapshot_taken = False
    # Fail on a misspelled script before the session starts: a dropped
    # step otherwise runs the whole case with no error and proves less
    # than the case claims, or fails after the session budget describing
    # only the symptom.
    check_after_script(after_script, snapshot)
    master, child = os.openpty()
    # The last size asserted through the AFTER script. A wait step below
    # re-asserts it while it polls, so one lost SIGWINCH on a slow runner
    # does not fail the case.
    pending_resize = None
    fcntl.ioctl(child, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    pid = os.fork()
    if pid == 0:
        # Create a controlling terminal with a foreground process group.
        os.setsid()
        fcntl.ioctl(child, termios.TIOCSCTTY, 0)
        os.dup2(child, 0)
        os.dup2(child, 1)
        os.dup2(child, 2)
        os.close(master)
        os.environ.setdefault("TERM", "xterm-256color")
        os.execv(argv[0], argv)
    os.close(child)
    data = b""
    reaped = False
    deadline = time.monotonic() + session_timeout
    try:
        # Wait until the initial synchronized update has made the input cursor
        # visible. Fixed sleeps race ASAN and slower CI runners, causing input
        # to be echoed by the tty before fytimui enters raw mode.
        data = read_until(master, data, b"\x1b[?25h", deadline)
        if edit_input:
            os.write(master, prompt + b"\x07")
            data = read_until(master, data, edit_needle,
                              time.monotonic() + progress_timeout)
            os.write(master, b"\n")
        else:
            os.write(master, prompt + b"\n")
        if during_input:
            time.sleep(during_delay)
            os.write(master, during_input + (b"\n" if during_submit else b""))
        if interrupt_after_during:
            time.sleep(interrupt_delay)
            data = read_until(master, data, during_input,
                              time.monotonic() + progress_timeout)
            if wait_file:
                # The call the interrupt must land in is running only
                # once its mock record exists. Poll the file; the
                # progress budget bounds it like any other wait.
                file_deadline = time.monotonic() + progress_timeout
                found = False
                while time.monotonic() < file_deadline:
                    try:
                        with open(wait_file, "rb") as fp:
                            if wait_file_needle.encode() in fp.read():
                                found = True
                                break
                    except OSError:
                        pass
                    ready, _, _ = select.select([master], [], [], 0.1)
                    if ready:
                        try:
                            chunk = os.read(master, 65536)
                        except OSError:
                            chunk = b""
                        if chunk:
                            data += chunk
                if not found:
                    raise RuntimeError(
                        "file %r never contained %r" %
                        (wait_file, wait_file_needle))
            occurrences = data.count(during_input)
            # One Escape: the input parser holds a lone ESC for 50ms to
            # tell it apart from an Alt chord, and a second ESC inside
            # that window is an Alt modifier for whatever follows it.
            # The old double ESC risked turning the resubmit below into
            # an Alt-Enter newline instead of a submit: the recalled
            # text then leaves the input for the transcript without
            # ever becoming a turn, and the completion never prints.
            os.write(master, b"\x1b")
            data = read_until_count(master, data, during_input,
                                    occurrences + 1,
                                    time.monotonic() + progress_timeout)
            if submit_recalled:
                if interrupt_settled_needle:
                    data = read_until(master, data, interrupt_settled_needle,
                                      deadline)
                # The recalled line must be back in the input row before
                # the submit lands: the settled needle only proves the
                # error band painted, while the recall redisplay runs a
                # frame behind it. A submit sent into that window hits
                # the session before the input layer holds the text, so
                # the turn never runs and the wait below burns the
                # session budget on a request that was never sent. The
                # count is a trap here, not a gate: every earlier
                # appearance of the text (typed echo, queued redisplay)
                # is already in the capture, and the pre-interrupt wait
                # above already consumed one more. Only a new input-row
                # frame proves the recall landed, and only the screen
                # model sees frames: redisplay the input row through it
                # and wait until the row holds the recalled text.
                data = read_until_input(master, data, during_input,
                                        rows, cols, deadline)
                # The resubmitted turn must print its own completion.
                # Count only what arrives after the recall is sent:
                # the capture holds the transcript behind the prompt,
                # and an earlier leg's completion sits in it. Waiting
                # for one more bare occurrence then waits for a turn
                # that already ran, while sending the recall must move
                # the count. Without the baseline a stale occurrence
                # satisfies the wait before the resubmission runs, and
                # the wrong request is asserted below.
                baseline = data.count(needle)
                tries = 3
                while True:
                    os.write(master, b"\n")
                    try:
                        data = read_until_count(master, data, needle,
                                                baseline + 1, deadline)
                    except RuntimeError as exc:
                        # Keep the interim bytes: the retry below must
                        # not lose what the capture already holds.
                        data = exc.partial
                        tries -= 1
                        # The submit above found no turn: nothing was
                        # sent, so no request reached the mock and the
                        # needle cannot arrive. The recall may have
                        # been consumed without starting one (the
                        # cleared input row in the capture proves the
                        # line left the input), so re-establish the
                        # gate before sending again: wait until the
                        # recalled text is back in the input row. The
                        # resubmit is only valid from a held input.
                        if tries <= 0:
                            raise
                        try:
                            data = read_until_input(
                                master, data, during_input, rows, cols,
                                deadline)
                        except RuntimeError:
                            pass
                        continue
                    break
                needle_count = data.count(needle)
        if progress_needle:
            data = read_until(master, data, progress_needle,
                              time.monotonic() + progress_timeout)
            release = os.environ.get("FYAI_PTY_PROGRESS_RELEASE", "")
            if release:
                start = data.find(progress_needle)
                suffix = read_until(master, data[start:], b"\x1b[?2026l",
                                    deadline)
                data = data[:start] + suffix
                with open(release, "wb"):
                    pass
            if interrupt_after_progress:
                os.write(master, b"\x03" if interrupt_key == "ctrl-c"
                         else b"\x1b")
            if resize_cols:
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 30, resize_cols, 0, 0))
        if mid_needle:
            data = read_until(master, data, mid_needle,
                              time.monotonic() + mid_timeout)
            # Resize while the answer is streaming.
            if resize_cols and not progress_needle:
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", rows, resize_cols, 0, 0))
        data = read_until_count(master, data, needle, needle_count, deadline)
        action_start = len(data)
        for step in after_script:
            kind, _, value = step.partition(":")
            if kind in ("send", "raw", "resize"):
                action_start = len(data)
            if kind == "send":
                os.write(master, value.encode() + b"\n")
                time.sleep(after_pause)
            elif kind == "raw":
                os.write(master, bytes.fromhex(value))
                time.sleep(after_pause)
            elif kind == "resize":
                # Resize an idle session.
                if "x" in value:
                    resize_rows, resize_cols = map(int, value.split("x", 1))
                else:
                    resize_rows, resize_cols = rows, int(value)
                fcntl.ioctl(master, termios.TIOCSWINSZ,
                            struct.pack("HHHH", resize_rows, resize_cols,
                                        0, 0))
                pending_resize = (resize_rows, resize_cols)
                time.sleep(after_pause)
            elif kind == "snapshot":
                # Capture the screen before subsequent actions modify it.
                with open(snapshot, "wb") as fp:
                    fp.write(data)
                snapshot_taken = True
            elif kind in ("wait", "wait-frame"):
                needle = value.encode()
                def matched():
                    if kind == "wait":
                        return needle in data
                    start = data.find(needle, action_start)
                    return start >= 0 and data.find(b"\x1b[?2026l", start) >= 0
                wait_deadline = time.monotonic() + after_timeout
                reassert_at = time.monotonic()
                while not matched() and time.monotonic() < wait_deadline:
                    if (pending_resize is not None and
                            time.monotonic() - reassert_at >= 1.0):
                        fcntl.ioctl(master, termios.TIOCSWINSZ,
                                    struct.pack("HHHH", *pending_resize,
                                                0, 0))
                        reassert_at = time.monotonic()
                    ready, _, _ = select.select([master], [], [], 0.1)
                    if not ready:
                        continue
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    data += chunk
                if not matched():
                    raise RuntimeError(
                        "PTY output never contained %r; tail=%r" %
                        (needle, data[-2000:]))
            elif kind == "settle":
                # Quiescence: return once nothing arrives for the given
                # seconds. A fixed drain keeps a fast runner waiting and
                # starves a slow one; a wait needs a needle that may not
                # exist, such as a commit that paints bytes the live tile
                # already showed. The wait deadline still bounds it.
                # Valid only while no tool band is live and no turn is
                # busy: ui_activity_refresh() repaints the activity cell
                # every interval while either holds, so a live tile is
                # never quiet and the deadline always wins. Settle an
                # idle session, never a live tile.
                quiet = float(value)
                settle_deadline = time.monotonic() + after_timeout
                silent_since = time.monotonic()
                reassert_at = silent_since
                while time.monotonic() < settle_deadline:
                    if (pending_resize is not None and
                            time.monotonic() - reassert_at >= 1.0):
                        fcntl.ioctl(master, termios.TIOCSWINSZ,
                                    struct.pack("HHHH", *pending_resize,
                                                0, 0))
                        reassert_at = time.monotonic()
                    ready, _, _ = select.select([master], [], [], 0.1)
                    if not ready:
                        if time.monotonic() - silent_since >= quiet:
                            break
                        continue
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    data += chunk
                    silent_since = time.monotonic()
            elif kind == "drain":
                drain_deadline = time.monotonic() + float(value)
                while time.monotonic() < drain_deadline:
                    ready, _, _ = select.select([master], [], [], 0.1)
                    if not ready:
                        continue
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    data += chunk
            else:
                raise RuntimeError("unknown FYAI_PTY_AFTER step: %r" % step)
        if snapshot and not snapshot_taken:
            with open(snapshot, "wb") as fp:
                fp.write(data)
        if clear_before_exit:
            os.write(master, b"\x15")
        os.write(master, b"/exit\n")
        eof = False
        while time.monotonic() < deadline:
            ready, _, _ = select.select([] if eof else [master], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    eof = True
                data += chunk
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                reaped = True
                if (os.WIFEXITED(status) and
                        os.WEXITSTATUS(status) == expected_status):
                    break
                if os.WIFSIGNALED(status):
                    raise RuntimeError(
                        "fyai died from signal %d" % os.WTERMSIG(status))
                raise RuntimeError(
                    "fyai exited with status %d" %
                    (os.WEXITSTATUS(status) if os.WIFEXITED(status)
                     else status))
        else:
            raise RuntimeError("fyai did not exit")
    finally:
        with open(output, "wb") as fp:
            fp.write(data)
        if not reaped:
            try:
                os.kill(pid, 9)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
        os.close(master)


if __name__ == "__main__":
    main()
