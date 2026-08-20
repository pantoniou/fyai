# Vendored libvterm

`fyai` keeps a copy of libvterm under `third_party/libvterm`. This note
explains why the copy exists, where it comes from, and what it builds into.

## 1. Why a vendored copy

`fyai` uses libvterm only as test infrastructure. A terminal rendering test
feeds a recorded byte stream through a `VTerm` screen and compares the
resulting cells against the sink's own output, because a PTY byte capture
cannot prove cell content, wrapping, cursor position, or SGR state by itself.
See `CLAUDE.md`, "Terminal rendering tests".

The original library, https://www.leonerd.org.uk/code/libvterm, has had no
release or commit since 2023. A test dependency that the build cannot find on
a fresh system, or that needs local patches to build at all, is not a
dependency a contributor can rely on. Vendoring removes that risk: the source
that fyai tests against sits in the tree, builds the same way on every
machine, and changes only when a patch changes it.

## 2. Source

The vendored copy comes from the fork that Neovim maintains,
https://github.com/neovim/libvterm, not from the unmaintained original. This
fork tracked upstream fixes until Neovim moved its own working copy into the
main Neovim tree; it remains the most recent buildable snapshot available as
a standalone library.

`third_party/libvterm/PROVENANCE.md` is the single source of truth for the
vendored commit hash, the vendoring date, and the update procedure. This
document does not repeat the hash, so there is one place to check when asking
"how current is this copy."

## 3. What is vendored

Only the library itself, not Neovim's or upstream's surrounding project:

- `include/vterm.h`, `include/vterm_keycodes.h`: the public API.
- `src/*.c`, `src/*.h`: the implementation.
- `src/encoding/*.tbl`, `src/encoding/*.inc`, `src/fullwidth.inc`: encoding
  and Unicode width tables. Upstream checks the generated `.inc` files into
  its own tree, so building the vendored copy needs no Perl toolchain.

Left out: the `bin/` command-line tools (`unterm`, `vterm-ctrl`,
`vterm-dump`), the Perl-driven `t/` test harness, `doc/`, and the upstream
build scripts. `fyai` builds the library sources directly; it does not need
any of that surrounding project.

## 4. Build integration

`CMakeLists.txt` compiles `third_party/libvterm/src/*.c` into a static
library target, `fyai_vterm`, inside the `BUILD_TESTING` block. Points that
follow from that placement:

- `fyai_vterm` is never linked into the `fyai` binary. It exists only for
  `fyai_test` and, in future, terminal rendering oracles.
- The target is built from source in-tree, not found on the system or
  fetched over the network at configure time. `cmake -S . -B build` needs no
  external libvterm, package, or extra tool to produce `fyai_test`.
- `fyai_vterm` compiles with the same `FYAI_C_DIALECT` and `FYAI_C_WARNINGS`
  as the rest of the tree, plus a short, explicit list of `-Wno-*` flags
  scoped to that one target. The vendored sources predate fyai's C style
  (declarations after statements, table initializers that only set the
  fields they need, callback parameters an implementation does not use), and
  patching vendored, verbatim upstream sources to match a house style would
  make future updates harder to diff against upstream, not easier. Silencing
  the specific warning classes that style produces keeps the build quiet
  without touching the vendored text.

## 5. Updating

Follow the steps in `third_party/libvterm/PROVENANCE.md`: pull a newer
commit from `https://github.com/neovim/libvterm`, copy the same file set
from section 3 over the files under `third_party/libvterm`, and update the
commit hash and date there. Do not hand-edit the vendored sources; carry any
local fix as a patch applied during the copy, noted in `PROVENANCE.md`.
