# pc1500preset

`pc1500preset` applies a `.pc1500` file (see
`docs/preset_file_format.md`) -- firmware, memory expansion, ROM modules, a
program to load, and a scripted sequence of keystrokes around loading it --
to a Sharp PC-1500 emulator session, for reproducible bug reports, demo
programs, and regression traces.

## Why this is a separate project

`pc1500preset` is an add-on to someone else's project, not a fork or a
feature of it. The emulator itself
([pc1500emu](https://github.com/pchambre/pc1500emu), checked out
separately as a sibling project at `../pc1500emu`) isn't this project's
own code and changes independently upstream. Rather than patching it to
understand `.pc1500` files directly, `pc1500preset` is a standalone tool
that launches an **unmodified** `pc1500emu` binary and drives it entirely
through its own existing FIFO/pipe scripting
interface (see `../pc1500emu/README.md`'s "Scriptable command
interface" section) -- `setextram`, `loadrommodule`, `reset`, `typeline`,
`loadbinary`/`loadbasic`/`loadbasictext`, `key`. Every one of those
commands is already part of upstream `pc1500emu`, so `../pc1500emu`
never needs a single line changed to support this.

**`../pc1500emu` is never to be modified from work done in this project.**
It's an independent sibling checkout, updated only by working in that
checkout directly.

## Layout

```
src/            pc1500preset's own source (the only thing this project builds)
samples/        example .pc1500 files and the programs they load
roms/           PC-1500 firmware ROM dumps (gitignored -- bring your own dumps)
docs/           the .pc1500 file format
../pc1500emu/        sibling checkout of the pc1500emu project (not part of this repo)
scripts/        convenience build scripts
```

## Building

The two projects build completely independently -- this project's
`CMakeLists.txt` never adds `../pc1500emu` as a subdirectory, and
`../pc1500emu`'s own build is untouched from upstream.

```sh
# This project's own tool:
cmake -B build
cmake --build build

# The emulator, on its own:
cmake -B ../pc1500emu/build -S ../pc1500emu
cmake --build ../pc1500emu/build
```

Or use `scripts/build_all.sh` to do both in one step.

`../pc1500emu`'s own README documents its build dependencies (SDL2,
SDL2_ttf).

## Getting the emulator

Clone or check out `pc1500emu` next to this repo, so it lives at
`../pc1500emu` relative to `pc1500preset`, and build it there following
its own `README.md`'s setup instructions.

## Usage

```sh
./build/src/pc1500preset samples/memtest.pc1500
```

By default `pc1500preset` looks for the emulator binary at (in order):
`$PC1500EMU_PATH`, `../pc1500emu/build/src/host/pc1500emu` (relative to
the current directory), next to `pc1500preset`'s own executable, then on
`PATH`. Override with `--emulator <path>` if none of those fit. Run
`./build/src/pc1500preset --help` for the full option list.

`roms/` is gitignored -- place your own PC-1500 ROM dumps there (see
`samples/memtest.pc1500`'s `firmware:` line for the expected
filename) before running the sample.
