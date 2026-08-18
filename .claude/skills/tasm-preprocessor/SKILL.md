---
name: tasm-preprocessor
description: Use when asked to add real preprocessor/macro-expander support (#include/#define/#ifdef, MACRO/ENDM, .EXPORT) to ../pc1500emu's TASM-to-sdas converter (pc1500disasm --mode convert), or to scope/hand off that work. This touches ../pc1500emu source, not this project's own -- see the repo-boundary note below before editing anything.
---

# TASM → sdas preprocessor/macro-expander enhancement

`pc1500disasm --mode convert` (in `../pc1500emu`) rewrites hand-written
TASM (`tasm5801.tab`) assembly to this toolchain's `sdas` dialect. It's
currently a **syntax-level, single-pass, line-based rewrite** — no
preprocessing or macro expansion. Three constructs are explicitly out of
scope today and reported as hard errors rather than silently
mis-converted: `#INCLUDE`/`#DEFINE`/`#IFDEF` preprocessor directives,
`MACRO`/`ENDM` macro definitions, and `.EXPORT` (cross-module linking).
Adding real support for these is the scope of this skill.

## Repo boundary — read before touching anything

This code lives at `../pc1500emu/src/disasm/tasm_convert.{h,cpp}`, in an
independent sibling checkout. **This project's own `CLAUDE.md` states
`../pc1500emu/` is never modified from work done in this repo** — that
rule exists because `pc1500preset` depends on driving an *unmodified*
upstream `pc1500emu` binary through its FIFO interface, and that sibling
checkout is meant to advance only via deliberate work done directly in it.

Practically: if you were invoked from within `pc1500preset` and asked
to do this work, either (a) confirm with the user that they specifically
want to edit inside the sibling checkout (a real, independently
committable git repo — `cd ../pc1500emu && git status` to check its
own branch/history), or (b) treat this skill as scoping/reference material
to hand off to a separate session working directly in a `pc1500emu`
checkout. Don't casually modify sibling-checkout files as a side effect of
a task scoped to `pc1500preset` itself.

That sibling checkout's own `.claude/skills/pc1500-dev/SKILL.md` is the
broader skill for all `pc1500emu` work (CPU internals, hardware quirks,
real-hardware testing) and is auto-discovered by Claude Code whenever a
task touches files under `../pc1500emu/` — this skill is a narrower
companion specific to the converter's preprocessor gap.

## Current architecture (`tasm_convert.cpp`, ~320 lines)

- Single converter class, one pass over the input, line by line — no
  separate lexer/token stream, no AST. Each line is classified and
  rewritten in place:
  - `mnemonicAliasMap()` — TASM mnemonics with no sdas equivalent
    (`CALL`/`RET`/`SCF` → `SJP`/`RTN`/`SEC`), confirmed against two real
    TASM sources (see the file's own top comment for provenance).
  - `directiveMap()` — `.EQU`/`.ORG`/`.DB`/`.BYTE`/`.DW`/`.WORD`/
    `.ASCII`/`.TEXT` → their sdas spellings.
  - `rewriteHexLiterals()` / `rewriteRegisters()` — token-level rewrites
    within a line (`$`-hex → sdas hex, register name casing).
  - String-literal detection (`.TEXT`/`.ASCII` operands) is quote-aware so
    rewriting doesn't corrupt string contents.
  - A first `.ORG` gets a synthesized `.area CODE (ABS)` wrapper; a second
    `.ORG` is a warning (multi-segment TASM sources aren't supported).
- `TasmConvertResult` carries `output`, `warnings` (non-fatal,
  best-effort pass-through of unrecognized tokens), and `errors` (fatal —
  `ok()` is `errors.empty()`). `#INCLUDE`/`#DEFINE`/`#IFDEF`, `MACRO`/
  `ENDM`, and `.EXPORT` currently hit the `err()` path — grep
  `tasm_convert.cpp` for `"TASM macro definitions"` and the
  `bareUpper == "EXPORT" || bareUpper == "MACRO" || bareUpper == "ENDM"`
  branch to find the exact rejection points to replace.

## Why this needs a real preprocessing pass, not a bigger single pass

The existing converter can rewrite a construct in place because TASM and
sdas syntax map roughly line-for-line for what it already handles. The
three deferred constructs don't:

- `#INCLUDE` requires resolving and splicing in another file's content
  *before* line-based conversion can proceed (and needs a search-path/
  include-once story if the included file itself includes something).
- `#DEFINE`/`#IFDEF` requires a symbol table and text substitution/
  conditional inclusion resolved *before* a line's meaning (even whether
  it exists in the output at all) is knowable.
- `MACRO`/`ENDM` requires capturing a template with parameters and
  re-expanding it at each invocation site — a many-lines-in/many-lines-out
  transform, not a rewrite-in-place one.

The right shape is a **separate preprocessing phase that runs over the raw
TASM source and produces a flat, fully-expanded, fully-resolved token/line
stream**, which the existing single-pass converter then consumes
unchanged (or nearly so) — same separation of concerns as a real C
preprocessor sitting in front of a compiler's own parser. Don't try to
fold macro expansion or conditional resolution into the existing
line-by-line rewrite loop; it doesn't have the right shape (no lookahead,
no symbol table, no concept of "this line produced zero or many output
lines").

## Test corpus and validation approach

`../pc1500emu/tests/tasm_convert_test.cpp` is the existing test file —
extend it rather than starting a new one. The converter's existing
coverage was built against two real hand-written/ROM-disassembled TASM
sources (see `tasm_convert.h`'s top comment): a small hand-written
memory-test program, and a much larger real ROM disassembly
(`github.com/Jeff-Birt/Sharp_CE-158`) that is also where the case for
needing a real preprocessor came from — **check whether the CE-158
source (or a fetchable equivalent) actually uses `#INCLUDE`/`#DEFINE`/
`MACRO` in practice** before designing the feature, so the implementation
targets constructs a real source uses rather than a hypothetical TASM
manual reading. Any new construct support should be confirmed by feeding
it a real file that exercises it, the same way the existing converter's
mnemonic/directive coverage was validated — not just synthetic unit-test
snippets.

## Backward compatibility

Files that don't use any of these constructs must convert identically to
before — the preprocessing phase should be a no-op pass-through when none
of `#INCLUDE`/`#DEFINE`/`#IFDEF`/`MACRO`/`.EXPORT` appear in the source.
Verify by re-running the existing `ctest` suite
(`cd ../pc1500emu/build && ctest --output-on-failure`) after any
change — a regression here would silently break every source that doesn't
even touch the new feature.
