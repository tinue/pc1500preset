---
name: preset-file-format
description: Use when adding, changing, or fixing anything in the .pc1500 file format itself -- new top-level fields, new script-line verbs, parser validation, or how a preset gets applied over the FIFO interface. Covers src/preset_file.{h,cpp}, src/main.cpp's applyPreset, and docs/preset_file_format.md.
---

# `.pc1500` file format: editing and enhancement

The format is a YAML, diffable description of PC-1500 machine state plus a
scripted keystroke sequence, parsed with `yaml-cpp` (fetched via CMake
`FetchContent` in the top-level `CMakeLists.txt` -- see its comment for
why, and `src/CMakeLists.txt` for the `yaml-cpp::yaml-cpp` link). Three
places must stay in sync for any change:

1. **`src/preset_file.h`** — the `PresetFile`/`ScriptLine`/
   `RomModuleAttachment` structs and `parsePresetFile()`'s declaration.
2. **`src/preset_file.cpp`** — the actual YAML-to-struct parser.
3. **`docs/preset_file_format.md`** — the format spec, kept authoritative
   and human-readable; update it in the same change as the parser, not
   later.

A fourth place, `src/main.cpp`'s `applyPreset()`, only needs touching if
the new field changes what gets sent over the emulator's FIFO interface
(most format changes do).

## Parser architecture (`src/preset_file.cpp`)

- `YAML::LoadFile(path)` parses the whole file into a `YAML::Node` tree;
  everything else is validating that tree's shape and pulling values out of
  it into `PresetFile`.
- `topLevelKeywords()` lists every recognized top-level field — **adding a
  new top-level field means adding its key string to that set**, or every
  file that sets it fails with "unrecognized field" even if you add
  handling for it further down. The loop at the top of
  `parsePresetFile()`'s try-block enforces this against every key actually
  present in the root mapping.
- Field dispatch is a plain `if (YAML::Node n = root["..."]) { ... }` chain
  in `parsePresetFile()` — add a new branch there, following the existing
  style (validate the node's shape, extract values, set a line-numbered
  `*error` via the `failAt(node, msg)`/`failFile(msg)` lambdas and
  `return false` on any problem). `failAt` uses `node.Mark().line + 1` for
  the line number — always pass the most specific node available (e.g. the
  bad value's own node, not its parent mapping) so the reported line
  actually points at the offending text.
- Script steps (inside a `pre-load-keys`/`post-load-keys` sequence) are
  single-key YAML mappings — `key`, `type`, `wait`, and `check` today,
  handled in the `parseScript` lambda, each producing a `ScriptLine` tagged
  with a `ScriptLineKind` (`kKeyPress`/`kType`/`kWait`/`kCheck`) rather
  than a bare bool. A new verb goes in that lambda's
  `if (verb == "key") ... else if (verb == "type") ...` chain, and needs a
  matching case in `main.cpp`'s `runScriptLine()` switch.
- Helpers already exist for the two literal kinds the format uses:
  `parseHex16()` (`0xXXXX` / bare hex / `&XXXX` for leniency, used for
  `address` fields — pulled out of a YAML scalar via
  `node.as<std::string>()`, never `node.as<int>()`, so `16k`-style
  suffixed sizes and bare hex digits both still parse the same way as
  before) and `parseSize()` (decimal byte count with optional `k`/`K`
  suffix). Reuse these for any new numeric field rather than writing a new
  parser.
- Any `YAML::Exception` thrown by a `.as<T>()` conversion elsewhere in the
  try-block (e.g. calling `.as<bool>()` on a value that isn't
  `true`/`false`) is caught once at the bottom and turned into the same
  line-numbered `*error` format via `e.mark.line`.
- `&` is YAML's anchor indicator when it's a scalar's first character —
  this only matters for `type`/`check` values (the only fields that carry
  the PC-1500's own `&XXXX` BASIC hex syntax through verbatim); a value
  that's *only* a bare `&XXXX` literal needs quoting in the source file
  (`type: "&1234"`), not something the parser can work around.

## `check`'s design: don't read `displaytext` for this

**Confirmed directly against the live emulator (not a guess): the FIFO
`displaytext` command does NOT reflect a statement's evaluated/rendered
output** — it only reflects the last typed input line, verbatim. E.g.
`type: PRINT "HELLO"` renders "HELLO" legibly on the real dot-matrix, but
`displaytext` still reads back the literal `PRINT "HELLO"` source text.
This is why `check` in `main.cpp` reads the FIFO `display` command (the
raw dot-matrix ASCII art) and searches it for an *encoded* glyph pattern
(`kGlyphZero`, `displayContainsGlyph()`) rather than comparing decoded
text — there's no decoder the other way (arbitrary dot-matrix pixels back
to text), only an encoder (known expected string → known glyph bitmap →
search for it). Only `"0"` has a confirmed glyph today; `preset_file.cpp`
rejects any other `check` value at parse time for exactly this reason.
Adding a new supported value means adding its 5×7 bitmap to `main.cpp`
next to `kGlyphZero` (confirm it against the live `display` output the
same way `kGlyphZero` was derived, not by guessing a font) *and* loosening
`preset_file.cpp`'s `parseScript` lambda's `script.text != "0"` check
together — never one without the other.

## Hard invariants — don't relax without a deliberate decision

- **Parsing is all-or-nothing.** `parsePresetFile()` returns `false` on
  the very first problem; nothing about the emulator is touched until the
  whole file has parsed cleanly (see `main.cpp`'s call site — the emulator
  isn't even launched yet at parse time). Don't add a code path that
  partially applies a preset on a parse error.
- **Every parse error must be file:line-numbered** (`failAt(node, msg)`,
  which uses `node.Mark().line + 1`) — this is a diffable, human-edited
  format; a bare "invalid value" without a line number is a real
  regression for anyone hand-editing a preset. Pass the most specific node
  you have (the bad scalar itself, not its parent mapping/sequence) so the
  reported line is actually useful. Use `failFile(msg)` (no line number)
  only for whole-file conditions that aren't tied to one line, like a
  missing required field.
- **All file paths are resolved relative to the preset file's own
  directory** (`resolveRelative()`, against `baseDir` = the preset's
  parent path), not the current working directory — this is what keeps a
  preset portable if its containing directory moves. Any new path-typed
  field must go through `resolveRelative()`.
- **Each script step is a single-key YAML mapping** (`- key: cl`, not
  `- key: cl\n  extra: field`) — `parseScript()` rejects anything else via
  the `item.size() != 1` check. Don't loosen this to allow extra keys per
  step; it's what keeps a step unambiguously "one verb."

## `applyPreset()` (`src/main.cpp`) — mapping fields to FIFO commands

Each preset field maps to one specific FIFO command, sent in a fixed
order (`memory-expansion` → `rom-modules` → `reset` → boot-settle delay →
`pre-load-keys` → `program` → `post-load-keys` — see
`docs/preset_file_format.md`'s "Load pipeline" section for why this order
is fixed). If you add a field that needs its own FIFO command:

- Commands that only need to succeed use `sendCommand()` (fire-and-forget,
  e.g. `setextram`, `reset`, `key`).
- Commands whose result must be checked use `sendCommandForResponse()` +
  `requireOk()` (e.g. `loadrommodule`, `loadbinary`/`loadbasic`/
  `loadbasictext`, `typeline`) — use this whenever a bad path or bad
  argument could otherwise fail silently.
- Check `../pc1500emu/README.md`'s "Scriptable command interface"
  section for the exact command syntax and which commands are
  fire-and-forget vs. response-writing — **never invent a new FIFO
  command or change the emulator to add one**; if the field you're adding
  has no existing FIFO command to drive it, it can't be added to this
  format without first getting a change landed upstream in `pc1500emu`
  (out of scope for this project — see the top-level `CLAUDE.md`'s repo
  boundary section).
- **`program.text` (inline BASIC) has no FIFO command of its own** —
  `loadbasictext` only takes a path — so `applyPreset()` spills it to a
  `std::filesystem::temp_directory_path()` file first (named
  `pc1500preset-inline-<pid>.bas`) and removes it right after the FIFO
  call, success or failure. If you add another inline-able field, follow
  the same pattern rather than inventing a second mechanism.

## Manual verification checklist

There's no automated test suite for `src/` yet. Verify a parser or
applier change by:

1. Build: `cmake -B build && cmake --build build`.
2. Run the existing sample end-to-end:
   `./build/src/pc1500preset samples/memtest.pc1500` — confirms you
   haven't broken the baseline pipeline.
3. Write a small throwaway `.pc1500` file that exercises the new/
   changed field, both a valid case and at least one invalid case (missing
   arg, bad hex, wrong keyword) — confirm the invalid case fails with a
   correctly line-numbered error *before* the emulator launches (add a
   deliberate typo partway through the file and confirm nothing was sent
   to the FIFO up to that point isn't actually observable — the real
   check is that parsing itself fails first).
4. For anything touching the script sections or program loading, actually
   watch the emulator window (or check `displaytext`/`status` FIFO
   queries) to confirm the keystrokes/program landed as intended — a
   silently-wrong `key`/`type` mapping won't fail loudly.
5. A `check` line's PASSED/FAILED line is the one piece of this tool with
   an actual correctness contract (it drives the process exit code) —
   when touching `waitUntilReady()` or the `check` case in
   `runScriptLine()`, deliberately test both a passing and a failing
   `check` and confirm the exit code (`echo $?`) matches, not just the
   printed text.

## When you change the format, also update

- `docs/preset_file_format.md` — grammar section, and the field-by-field
  list.
- `samples/memtest.pc1500` and/or add a new sample if the change
  introduces a genuinely new use case worth documenting by example — this
  file doubles as the format's own worked example in the docs.
