# Filesystem view performance

This note records where `view create` and the exit of `view enter` spend time
and memory. It does not cover `--verify`, which is a debug path.

## Method

- Project: a 44 MB clone of this repository. It has 859 files and a 1499-file
  `.git`. A capture records about 2400 files and directories.
- Build: `RelWithDebInfo`, Linux, ext4, one warm run.
- Allocations: a `LD_PRELOAD` shim that counts `malloc`, `calloc` and
  `realloc` calls and groups them by call stack. The base is `view list`,
  which does about 46000 calls and 5 MB.
- Time: `perf record` with the `-g` option, split by process and library.

## `view create`

The run takes about 0.27 s. Kernel time is 74% (the data copy). libfyaml is
18%. The run is I/O bound; the allocation count is not the limit.

The run adds about 9400 allocations and 38 MB to the base. Four sites cause
nearly all of it:

| Site | Cost | Cause |
|------|------|-------|
| `fyai_project_capture()`, `linked` set | 1458 allocations, 17 MB | `fy_assoc()` copies the whole mapping for each borrowed Git object. The cost grows with the square of the object count. |
| `capture_scan()`, `fdopendir()` | 369 allocations, 12 MB | libc allocates a buffer of about 32 KB for each directory. |
| `capture_add()`, node path | about 2700 allocations | One heap string for each node. |
| `project_leaf()` and `fyai_project_directory()` | about 2800 allocations | Generic builder values for each node. |

Possible changes:

- Replace the `linked` set with a sort of the nodes by digest and a single
  pass that skips equal neighbors.
- Read directories with `getdents64()` into one buffer for each worker.
- Build a node path from the parent path and the name in a pool, without
  `asprintf()`.

## Exit of `view enter`

Before the change, a command that does nothing took about 0.28 s. The command
was about 0% of this. The snapshot child used 65% of the CPU and the parent
used 34%.

The child captures the frozen upper. For an unchanged view this finds no
change. The child then did these steps for the complete project:

1. `view_wire_value()` copied every string to set the double-quoted style.
   This was about 10000 allocations.
2. `fy_emit()` wrote the manifest as 1.98 MB of YAML. The emitter output
   buffer grew in many small steps: about 700 `realloc()` calls with a
   cumulative size of about 480 MB.
3. `view_transfer()` sent the text through a pipe.

The parent read the text and parsed it with `fy_parse()`.

The cost was proportional to the project size, not to the changes.

### Delta transfer

This step is replaced by the binary manifest below. It is kept for the numbers.

The child now sends the difference from the baseline: the root, and the
objects and attributes that changed or went away. The parent applies the
difference to the baseline that it holds. Objects are content addressed, so an
unchanged object is not sent. An exit that changed nothing sends 4 KB, and the
parent keeps the baseline tables as they are.

The mappings of a generic are not sorted, and a lookup in them is linear. The
code that finds the difference therefore builds a hash index of the keys of
the baseline, in one pass over each table.

| Measure | Before | After |
|---------|--------|-------|
| Bytes in the pipe | 1.98 MB | 4 KB |
| Cumulative bytes requested in the child | 487 MB | 6.7 MB |
| Wall time of `view enter true` | 0.27 s | 0.16 s |

### Sorted tables

The objects and attributes tables of a snapshot keep their keys in byte order.
A lookup is a binary search over the stored pairs, `fyai_project_find()`. The
generic `fy_get()` scans a mapping, which is correct for general code but makes
a walk over a large table quadratic. The capture code builds a table with
`fyai_project_table_create()`, which sorts the pairs first. The exit delta is a
single merge walk over two sorted tables.

On a larger tree (372 MB, 10100 nodes) an exit that changed nothing took
1.4 s before the tables were sorted. The profile showed 85 million generic
compares: 58% in the pruning of stale attributes and 40% in
`capture_reused_objects()`, one linear lookup for each node.

| Measure, 372 MB tree | Before | After |
|----------------------|--------|-------|
| `view enter true` | 1.4 s | 0.14 s |
| `view update` | 1.8 s | 0.55 s |
| `view create` | 0.6 s | 0.6 s |

A tree that was stored before the tables were sorted is not valid: recreate
the view.

### Kernel scale

A clean Linux kernel clone: 84840 files, 5547 directories and 60 symlinks,
1.4 GB of files plus 5.8 GB of Git packs that a view borrows by hard link.
Linux, ext4, NVMe, 12 cores, warm cache.

| Operation | Time |
|-----------|------|
| `view create` | 5.0 s (4.8 s of capture; 20 s of kernel time over 13 workers) |
| `view update` | 4.3 s |
| `view enter true` | 0.44 s |
| `view diff` | 1.3 s |

A tree with build output has more than 100000 nodes. The capture then fails
with `E2BIG`, because of `FYAI_PROJECT_MAX_NODES`.

The first measurement of the exit was 1.55 s. 66% of it was libfyaml dedup:
the builder of the exit child had `FYGBCF_DEDUP_ENABLED`, and the installed
libfyaml calls the libgcc `__popcountdi2` for its bitmap (48% of the CPU). The
child result is transient text, so the child builder does not deduplicate. The
exit dropped to 0.44 s. The builders that store into the arena keep it.

`view create` of the kernel reaches 5.4 GB of resident memory, mostly the
mapped Git packs that the hash reads. This is page cache and not heap.

### Binary frame and scratch arena

The exit child sends a frame on the pipe: a header with a magic number, a
version, a status and the payload length, then the payload in the binary
encoding of `fyai_wire.h`. The encoding has tagged values with varint lengths,
and a digest or a hex name takes its raw bytes. The reader checks every count
and length against the bytes that remain. An error frame carries the errno and
the path of the capture. There is no YAML emit or parse on the exit path.

The capture and the delta use `fyai_scratch`, a libfyaml `mremap` allocator
without dedup, for the nodes, paths, directory entries and the pair arrays.
The arena is released in one call. Only the capture thread allocates from it
in a normal run: a worker allocates only to read the extended attributes of a
file that has some.

Measured on the kernel tree after 5000 changed files:

| Measure | YAML text | Binary frame and arena |
|---------|-----------|------------------------|
| Exit | 0.97 s | 0.83 s |
| Bytes in the pipe | 4.2 MB | 2.0 MB |
| Bytes requested in the child | 2.2 GB | 23 MB |

### Binary manifest

The objects of a project were generics in the arena: one mapping for each file,
symlink and directory, found by digest. Building them was most of the serial
phase of `view create` and of the exit. A manifest is now one immutable binary
file in `<project>/.fyai/manifests`, mapped and read in place:

- Fields are little-endian and naturally aligned, and records start at multiples
  of 8 bytes, so a record is read through a struct with no copy.
- An index of digests gives a binary search, and the path timestamps are a
  second sorted table. Opening checks only the header and the bounds of the
  regions.
- The view record in the arena holds a reference: the root, the file name and
  the number of objects.
- The exit child captures into a manifest and stores it as a delta over the
  baseline manifest when that is smaller. A delta holds the records that differ
  from the baseline, the digests that the tree no longer has, and the same for
  the path timestamps; it names its base by the digest of the base file. A
  lookup reads the delta first and then the base. A base is always a full
  manifest, so there is one level. The child sends a frame with the root and the
  name, and a file that already exists is kept. There is no text.
- An unchanged subtree is copied as its records. A file gets its identity in its
  worker, and the files are checked once more in parallel.

| Measure, kernel tree | Generics | Binary manifest |
|----------------------|----------|-----------------|
| `view create` | 3.6 s | 2.4 s |
| Manifest phase of the create | 1.5 s | 0.2 s |
| `view enter true` | 0.45 s | 0.19 s |
| Exit after 5000 changed files | 0.80 s | 0.43 s to 0.54 s |
| `view diff` | 1.3 s | 0.16 s |

A full manifest of the kernel tree is 15 MB. The delta that an exit stores is
2.3 KB when nothing changed and 1.2 MB after 5000 changed files. Making the
delta costs about 20 to 40 ms: the exit takes 0.19 to 0.22 s with no change, and
0.43 to 0.46 s after 5000 changed files.

### Parallel scan and delta from the capture

The scan walks the directories one level at a time on the libfyaml thread pool
in work-stealing mode; the directories of a level are independent. The
exit child now builds the delta straight from the capture. It adds the records
that the baseline lacks or holds in another form. A walk of the retained
subtrees gives the objects that the tree still has, so the removed list is exact.
The full manifest and the merge walk are not made.

The finish pass for directories and symlinks also runs level by level, from the
deepest level up, on the same pool. A level of fewer than 256 nodes is finished by
the caller.

| Measure, kernel tree | Before | After |
|----------------------|--------|-------|
| `view create` | 2.4 s | 1.8 s |
| Scan phase | 540 ms | 130 ms |
| `view create`, reservation mode | - | 2.6 s to 3.2 s |
| `view enter true`, second run | 0.37 s | 0.37 s |
| Exit after 5000 changed files | 3.9 s | 3.8 s |

Reservation mode of the pool is slower than work stealing for the scan, so work
stealing stays the only mode. The parallel finish pass gains about 50 ms: a
directory is cheap, and the files are done by their workers.

The sort of the index and the copy of the records in the manifest builder cost
12 ms and 7 ms for 90000 objects. They are not worth a parallel version.

### Remaining cost

Most of the rest is kernel time for the per-file system calls on the overlay
mount, and the copies.

## Open items

- Find the source of the remaining 10000 `lseek` calls.
- Overlap the scan with the capture; the gain is not known.
- Measure the delta transfer on a project with many changed files.
- Measure on a filesystem with reflink, where the create copy is cheap.
- Measure a cold cache.
