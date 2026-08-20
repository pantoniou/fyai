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

## Patches carried from Neovim's own copy

`neovim/libvterm` stopped tracking real changes in mid-2024 (commit
`2c01e1f`; everything after that is a CI bump). Neovim's actual working
copy moved into `neovim/neovim` at `src/nvim/vterm` on 2025-01-03 (commit
`d8bc08db7`, "refactor: adopt vterm") and has kept receiving fixes there
ever since. This vendored copy carries the substance of those later fixes,
hand-ported onto the file layout above, because `neovim/neovim` restructures
the library (splits `vterm.h`/`vterm_internal.h` into several headers,
renames things, and — starting from the `d8bc08db7` import itself, before
any of the commits below — replaces the `chars[VTERM_MAX_CHARS_PER_CELL]`
per-cell codepoint array with Neovim's own single-integer `schar_T`
encoding) too deeply for a plain `git apply` to work.

Considered every commit touching `src/nvim/vterm` in `neovim/neovim` from
the `d8bc08db7` import (2025-01-03) up to the sync point below. Each is
either ported (semantics carried over, adapted to this copy's plain
`chars[]` cell representation) or skipped, for the reason given:

Ported:

- `f8c8a245aa` fix(terminal): don't crash on unprintable chars — `screen.c`,
  `vterm_screen_get_cell()`
- `f1c45fc7a4` feat(terminal): support theme update notifications (DEC mode
  2031) — `screen.c`, `state.c`, `vterm.h`, `vterm_internal.h`
- `6f0bde11cc` feat(terminal): add support for kitty keyboard protocol
  (disambiguate mode only) — `keyboard.c`, `state.c`, `vterm_internal.h`
- `06a1f82f1c` feat(terminal): forward X1 and X2 mouse events — `mouse.c`
- `ee143aaf65` fix(terminal): emit Termrequest for all OSC sequences —
  `state.c`, `on_osc()` now always calls the fallback
- `b28bbee539` fix(terminal): skip setting `string_initial` to false on
  no-op — `parser.c`, `string_fragment()`
- `756751afa3` fix(terminal): stack overflow when too many csi args —
  `vterm_internal.h`, `CSI_ARGS_MAX` 16 -> 32
- `977e91b424` + `112092271b` DA1 response modernized to `?61;22;52c` —
  `state.c`. Carried the final response string only, not the
  FFI-overridable global variable Neovim added around it for its own Lua
  tests; nothing here needs that override hook.
- `03377b9552` feat(terminal): include sequence terminator in TermRequest
  event — `parser.c`, `vterm.h` (adds `VTermStringFragment.terminator`)
- `7a6e8d4430` docs: misc — `screen.c`, one comment typo
- `814f2629cb` fix(terminal): handle split composing chars at right edge —
  `state.c`, `on_text()`. Neovim's fix is written against a grapheme-cluster
  combining algorithm this copy does not have; ported the equivalent
  behavior (widen the "cursor hasn't moved" check to a range, clear
  `at_phantom`) onto the plain `vterm_unicode_is_combining()` check here.
- `e40c5cb06d` fix(vterm): handle split UTF-8 after ASCII properly —
  `state.c`, `on_text()`
- `2368a9edbd` feat(terminal): support SGR dim, overline attributes —
  `pen.c`, `screen.c`, `vterm.c`, `vterm.h`, `vterm_internal.h`
- `b38173e493` feat(terminal): synchronized output (mode 2026) — `state.c`,
  `vterm.h`, `vterm_internal.h`. Carried the vterm-side mode/prop plumbing
  only, not Neovim's own redraw-gating in `terminal.c`, which is outside
  vterm.
- `7bb8231577` fix(terminal): do not reflow altscreen on resize — `screen.c`
- `635acc7dc8` fix(terminal): cursor moves on resize when line above is
  full width — `screen.c`

Skipped:

- `d8bc08db7` refactor: adopt vterm — the import commit itself, not a patch;
  it is the baseline these commits are counted from.
- `47866cd8d2` refactor: delete duplicate utf8-functionality — a deliberate
  feature removal (drops the UK national-charset designator), not a fix.
  Keeping it costs nothing and keeps this copy a superset of Neovim's.
- `442f297c63` refactor(build): remove INCLUDE_GENERATED_DECLARATIONS guards
  — a Neovim build-macro cleanup with no equivalent here.
- `19eb75831b` ci(test): bump Windows runners — CI only, plus a `DLLEXPORT`
  on the global variable this copy doesn't carry (see the DA1 entry above).
- `VTERM_ATTR_URI` / OSC 8 hyperlink support — already present in Neovim's
  tree at the `d8bc08db7` import, so it predates the commit range above and
  was never in `neovim/libvterm` either. Out of scope for this pass; a
  future update should look at it separately if hyperlink attributes start
  to matter for a test.

Last `neovim/neovim` commit considered when this list was made:
`635acc7dc848853fe4a5db2f0c85dc52e8eabed0` (2026-07-28, `src/nvim/vterm`
HEAD examined: `31de0d69fc7`).

## Updating

This is a two-step process; do not skip straight to copying files.

1. **Base sync**: pull a newer commit from
   `https://github.com/neovim/libvterm`, copy the file set from "What is
   vendored" over the files under `third_party/libvterm`, and update the
   commit hash and date near the top of this file.
2. **Patch sync**: check `neovim/neovim`'s `src/nvim/vterm` history for
   commits after the "last commit considered" hash above, and for each one,
   decide port or skip using the same judgment calls as the list above (does
   it touch vterm's own logic, or only Neovim's UI/build/CI integration; can
   its intent be re-expressed against this copy's plain `chars[]` cell
   representation). Hand-port anything that applies, add it to the "Patches
   carried" list with the same one-line format, and move the "last commit
   considered" pointer.

`scripts/update-vendored-libvterm.sh` automates the mechanical half of both
steps — refreshing the base copy, and listing the `neovim/neovim` commits
since the last sync point for triage — but not the actual patch porting,
which needs the judgment above. Run it with `--help` for usage.

Do not hand-edit the vendored sources outside of this process.
