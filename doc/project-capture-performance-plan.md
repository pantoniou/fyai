# Project capture performance plan

## Problem

The diff view of a group of tool calls (`view/tool_diff`) and the commit of a
turn take a project state with `fyai_project_capture_manifest()`. Each capture
walks the whole project, reads and hashes every file, copies it into the object
store, and verifies the copy when the object exists. The cost depends on the
bytes of the project and not on the number of changes. A capture of a project
with nothing changed costs the same as the first one.

Measured with the capture alone, 13 workers, warm cache, one run of each:

| Tree | Capture | Walk and stat of every file |
|------|---------|-----------------------------|
| 2.5k files, 26 MB | 111 ms | about 10 ms |
| 16k files, 220 MB | 710 ms | about 40 ms |

A turn with N rounds of tool calls takes about 2N+1 captures: one before each
model request, one after each group that can change files, and one at the commit.

## Step 1: a stat cache for the invocation (done)

The invocation keeps, for each file that a capture read, the stat of the file and
the digest of its content. A later capture of the same project reads a file only
when its stat is not the cached one.

- The cache is memory of the invocation (`ctx->bstate`). It is not stored, it has
  no manifest field, and the format of a manifest does not change. The first
  capture of an invocation reads every file.
- An entry has the device, inode, size, mode, owner, mtime and ctime of the file,
  and the digest and size of its content. The ctime is set by the kernel at each
  change of content or metadata and a program cannot set it, so an entry that
  matches is the same file with the same content.
- A file whose mtime or ctime is less than two seconds older than the start of
  the capture that read it is not cached: a change in the same timestamp tick
  would keep the stat. The next capture reads it.
- An entry is used only when the object of its digest is in the object store, as a
  regular read-only file of the size of the blob. A garbage collection between
  two captures cannot make a manifest name a missing object.
- The cache is for one project directory and one object store, and it is dropped
  when either changes.
- A borrowed Git object, a capture that has a baseline directory, and an
  incremental capture do not use the cache.
- The capture reports the number of files that it did not read
  (`cached_files`).

The cost of a capture with nothing changed is then the walk, the stat of each
file, and one stat of its object. After step 1, with the same trees:

| Tree | First capture | Capture, nothing changed | One file changed |
|------|---------------|--------------------------|------------------|
| 2.5k files, 26 MB | 113 ms | 9 ms | 9 ms, one file read |
| 16k files, 220 MB | 702 ms | 46 ms | 46 ms, one file read |

The first capture of an invocation still reads every file. A project that holds
many more files, or a tool that needs a capture faster than the walk, needs step 2.

## Step 2: change hints (only if step 1 is not enough)

On Linux, an inotify watch on each directory, held while a turn runs, marks the
paths that changed. A capture stats only the marked paths and keeps the cached
entry of every other file.

- A watch limit, a queue overflow, a directory that is not watched yet, and any
  error make the capture a full walk. The hint never replaces the check.
- Fanotify has no advantage: an unprivileged process cannot mark a mount or a
  filesystem, so it needs a mark on each directory as inotify does.
- The hint is a backend (`*_linux.c`). Other systems keep the walk of step 1.
- Step 2 changes only which files the capture stats. The cache and its rules of
  step 1 stay.

## Tests

- A unit test captures a tree twice and checks that the second capture reads no
  file, then changes the content of a file and keeps its size and mtime, and
  checks that the new digest is in the manifest.
- A unit test checks that a file changed within two seconds of a capture is read
  by the next one.
- A unit test checks that a missing object makes the capture read the file.
- The functional test `tool_diff` stays unchanged.
- The benchmark of the table above is run again after each step.
