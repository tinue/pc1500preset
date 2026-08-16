# pc1500preset

`pc1500preset` applies a `.pc1500` file (see
`docs/preset_file_format.md`) -- firmware, memory expansion, ROM modules, a
program to load, and a scripted sequence of keystrokes around loading it --
to a Sharp PC-1500 emulator session, for reproducible bug reports, demo
programs, and regression traces.

## Why this is a separate project

`pc1500preset` is an add-on to someone else's project, not a fork or a
feature of it. The emulator itself
([pc1500emu](https://github.com/pchambre/pc1500emu), vendored here as a
git submodule under `vendor/pc1500emu`) isn't this project's own code and
changes independently upstream. Rather than patching it to understand
`.pc1500` files directly, `pc1500preset` is a standalone tool that
launches an **unmodified** `pc1500emu` binary and drives it entirely
through its own existing FIFO/pipe scripting
interface (see `vendor/pc1500emu/README.md`'s "Scriptable command
interface" section) -- `setextram`, `loadrommodule`, `reset`, `typeline`,
`loadbinary`/`loadbasic`/`loadbasictext`, `key`. Every one of those
commands is already part of upstream `pc1500emu`, so `vendor/pc1500emu`
never needs a single line changed to support this.

**`vendor/pc1500emu` is never to be modified.** It's a plain submodule
checkout, updated only by bumping which upstream commit it points at.

## Layout

```
src/            pc1500preset's own source (the only thing this project builds)
samples/        example .pc1500 files and the programs they load
roms/           PC-1500 firmware ROM dumps (gitignored -- bring your own dumps)
docs/           the .pc1500 file format
vendor/pc1500emu/   git submodule: an unmodified pc1500emu checkout
scripts/        convenience build scripts
```

## Building

The two projects build completely independently -- this project's
`CMakeLists.txt` never adds `vendor/pc1500emu` as a subdirectory, and
`vendor/pc1500emu`'s own build is untouched from upstream.

```sh
# This project's own tool:
cmake -B build
cmake --build build

# The vendored emulator, on its own:
cmake -B vendor/pc1500emu/build -S vendor/pc1500emu
cmake --build vendor/pc1500emu/build
```

Or use `scripts/build_all.sh` to do both in one step.

`vendor/pc1500emu`'s own README documents its build dependencies (SDL2,
SDL2_ttf).

## Getting a submodule checkout

```sh
git clone --recurse-submodules <this-repo-url>
# or, after a plain clone:
git submodule update --init
```

## Usage

```sh
./build/src/pc1500preset samples/memtest.pc1500
```

By default `pc1500preset` looks for the emulator binary at (in order):
`$PC1500EMU_PATH`, `vendor/pc1500emu/build/src/host/pc1500emu` (relative to
the current directory), next to `pc1500preset`'s own executable, then on
`PATH`. Override with `--emulator <path>` if none of those fit. Run
`./build/src/pc1500preset --help` for the full option list.

`roms/` is gitignored -- place your own PC-1500 ROM dumps there (see
`samples/memtest.pc1500`'s `firmware:` line for the expected
filename) before running the sample.
