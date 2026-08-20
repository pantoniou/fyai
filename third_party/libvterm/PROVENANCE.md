# Vendored libvterm

`fyai` uses libvterm only as test infrastructure: it feeds recorded terminal
byte streams through a `VTerm` screen and compares cells against the sink's
own rendering. See `CLAUDE.md`, "Terminal rendering tests".

The original library, https://www.leonerd.org.uk/code/libvterm, has no
release or commit since 2023 and does not build with current toolchains
without patches. This copy is vendored from the fork that Neovim maintains,
https://github.com/neovim/libvterm, which carries the needed fixes and
tracked upstream until Neovim moved its working copy into the main Neovim
tree.

- Upstream repository: https://github.com/neovim/libvterm
- Vendored commit: 934bc2fbf21800ac3458a499df8820ca5fb45fd3
- Vendored on: 2026-08-20
- License: MIT (see `LICENSE`), copyright Paul Evans

## What is vendored

Only the library sources, not the command-line tools, the Perl-based test
harness, or the build scripts:

- `include/vterm.h`, `include/vterm_keycodes.h`
- `src/*.c`, `src/*.h`
- `src/encoding/*.tbl`, `src/encoding/*.inc` (generated tables, checked in
  upstream, so no Perl toolchain is needed to build)
- `src/fullwidth.inc` (generated table, checked in upstream)

## Updating

Pull a newer commit from `https://github.com/neovim/libvterm`, copy the same
file set over the files here, and update the commit hash and date above. Do
not hand-edit the vendored sources; carry local fixes as a patch applied
during the copy, noted in this file.
