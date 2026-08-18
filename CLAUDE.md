# pc1500preset

`pc1500preset`: applies a `.pc1500` file (firmware, memory expansion,
ROM modules, a program, and a scripted keystroke sequence) to an
**unmodified** `pc1500emu` binary, driven entirely through its existing
FIFO/pipe scripting interface. See `README.md` and
`docs/preset_file_format.md` for the full picture before making changes.

## Repo boundary — read this first

- `src/`, `docs/`, `samples/`, `scripts/` are this project's own code —
  free to edit.
- `../pc1500emu/` is an independent sibling checkout (never a submodule of
  this repo). **Never modify it from work done in this project.** This
  tool's entire design point is driving an unmodified upstream binary
  through its own FIFO interface rather than patching it — see the
  README's "Why this is a separate project" section. If a task seems to
  need a change inside `../pc1500emu/`, that's a signal the task belongs
  in a separate session/checkout of `pc1500emu` itself, not here.
- That said, `../pc1500emu/` is a real, independently-developed repo with
  its own `.claude/skills/pc1500-dev/SKILL.md` (LH5801 CPU internals,
  hardware quirks, disassembler/converter internals, real-hardware testing
  methodology). Claude Code auto-discovers it and applies it when a task
  touches files under `../pc1500emu/` — you don't need to invoke it
  manually.

## This project's two skills

- **`preset-file-format`** — extending or fixing the `.pc1500` file
  format itself: the parser (`src/preset_file.{h,cpp}`), the applier
  (`src/main.cpp`'s `applyPreset`), and `docs/preset_file_format.md`. Use
  for anything like "add a new top-level field", "support X in a script
  line", or "the parser rejects/accepts something it shouldn't".
- **`tasm-preprocessor`** — adding real preprocessor/macro-expander support
  (`#include`/`#define`/`#ifdef`, `MACRO`/`ENDM`, `.EXPORT`) to
  `../pc1500emu`'s `pc1500disasm --mode convert` (TASM → sdas
  converter). This is `../pc1500emu` source, so per the boundary above
  it's for a separate `pc1500emu` working session — the skill exists here
  because you may be asked to scope or hand off that work from this repo.

Both are optional — invoke with `/preset-file-format` or
`/tasm-preprocessor` (or let Claude Code pick them up by description) only
when the task actually calls for them.

## Build

```sh
cmake -B build && cmake --build build          # this project's own tool
./scripts/build_all.sh                          # also builds ../pc1500emu
```

No automated test suite exists for `src/` yet (unlike `../pc1500emu`,
which has a full `ctest` suite). Verify changes by running
`./build/src/pc1500preset samples/memtest.pc1500` and by hand-testing
against `.pc1500` files that exercise the changed feature — see the
`preset-file-format` skill for the manual-verification checklist.

## Quick orientation

```
src/            pc1500preset's own source (preset_file.{h,cpp}, main.cpp)
docs/           .pc1500 file format spec
samples/        example .pc1500 files + the programs they load
roms/           gitignored -- bring your own PC-1500 ROM dumps
../pc1500emu/       sibling checkout, never modified here
```
