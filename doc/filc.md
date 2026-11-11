# Building and running fyai with Fil-C

[Fil-C](https://fil-c.org/) is a memory-safe C implementation. fyai builds and
runs with it. It needs a Fil-C toolchain with `zgc_reserve_at()`, which places
the durable arenas at their configured base as natively (see
`doc/filc-support.md` in libfyaml).

## Building

Everything that fyai links must be built with Fil-C. Host libraries cannot be
used:

- Build zlib and OpenSSL with the Fil-C compiler into a private prefix. The
  Fil-C source tree has the patched OpenSSL (`sarcastic-openssl`).
- Hide the host packages from CMake. It then builds libfyaml, the other libfy*
  libraries and curl from source:

```sh
PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig cmake -S . -B build-filc -G Ninja \
    -DCMAKE_C_COMPILER=$FILC/build/bin/clang \
    -DCMAKE_CXX_COMPILER=$FILC/build/bin/clang++ \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-O1 \
    -DCMAKE_PREFIX_PATH=$PREFIX "-DCMAKE_IGNORE_PREFIX_PATH=/usr/local;/usr" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DOPENSSL_ROOT_DIR=$PREFIX \
    -DCMAKE_BUILD_RPATH=$PREFIX/lib
```

Run with `LD_LIBRARY_PATH=$FILC/pizfix/lib`. Do not put the Fil-C OpenSSL on
`LD_LIBRARY_PATH`, as it breaks the host tools (`ctest`); it is in the rpath.
Run the tests with few jobs (`ctest -j2`), as each Fil-C process reserves large
address ranges.

## What is different under Fil-C

- The libc has no `statx()`. `src/fyai_filc_compat.h` takes `struct statx` from
  the kernel headers and issues the system call.
- `close_range()` is not used; the fallback closes the descriptors.
- The kernel keyring secret store is not built: its pointer arguments pass
  through `syscall()` as integers, which Fil-C rejects.
- The crash handler is not installed: the runtime refuses `sigaltstack()` and
  reports its own safety stops.
- `unshare()`, `setns()`, `syncfs()` and `pidfd_send_signal()` need a runtime
  that dispatches them. A user namespace also needs one that runs the program
  on its initial thread. Older runtimes stop the program.

## Reopening the arena in a sub-agent

A sub-agent reopens the arena at the address of the old one. Under Fil-C the
address is free again only when nothing points into the old arena, and the
frames of the running functions do, through their locals and the pointers they
passed to calls. A sub-agent child therefore runs its call with
`zcall_on_fresh_stack()`, so no frame of the serve loop is alive, and
`fyai_agent_reopen_for_call()` reopens the arena first, before `tool_agent()`
and `fyai_execute_tool_call()` hold strings of the call. A reopen deeper in the
stack does not get the address back.

`zdump_heap(fd)` lists every object with what reaches it, where a source of
`thread` is a frame. Call it where a reopen fails to find what keeps the old
arena alive.

## PID namespaces

A filesystem view unshares the user, mount and PID namespaces, as it does
natively. After `unshare(CLONE_NEWPID)` the kernel refuses to create threads,
and a Fil-C process restarts its collector thread after every `fork()`. Under
Fil-C the view therefore unshares the user and mount namespaces and creates the
PID namespace with the `clone()` that makes its init:
`syscall(SYS_clone, SIGCHLD | CLONE_NEWPID, 0, 0, 0, 0)`. Fil-C supports this
one form, a fork that creates the child in new namespaces. The parent stays in
its own namespace and keeps its collector.

A process that joins the namespace with `setns()` has `pid_ns_for_children`
set until it forks, and cannot restart its collector. It runs without one,
reclaims no memory, and `zgc_wait()` returns at once. Such a process should
only fork, wait and exit.

## Not yet working

- `stackcheck/run`: an escaped scratch generic is not detected.
- `case_ui_page_tiles` needs a longer timeout (`FYAI_TIMEOUT_SCALE_DEFAULT=4`).
- Tests of the kernel keyring and of `close_range()`.
