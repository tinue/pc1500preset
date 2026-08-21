# The `.pc1500` file format

A `.pc1500` file is a YAML, human-editable, diffable description
of a PC-1500 machine state plus a scripted sequence of keystrokes to run
around loading one program. Apply one with:

```
./build/src/pc1500preset path/to/file.pc1500
```

This launches an unmodified `../pc1500emu` binary and always cold-boots
it from the preset's own firmware/hardware sections. See this project's own
top-level `README.md` for how `pc1500preset` locates the emulator binary,
and why the emulator itself is never modified to support this format.

## Syntax

- The file is a single YAML mapping, parsed with `yaml-cpp` -- ordinary
  YAML rules apply (`#` comments, 2-space indentation, `key: value`,
  `- item` sequences). `pc1500preset` doesn't impose any further layout
  constraints beyond validating the specific fields below.
- Top-level fields can appear in any order -- they're declarative state, not
  a sequence. `pre-load-keys`/`post-load-keys` are the exception: each is a
  YAML sequence, and *within* it, order is execution order.
- File paths (`firmware`, `program.path`, and a `rom-modules` entry's own
  `path`) are resolved relative to the preset file's own directory, not the
  current working directory, so a preset stays portable if its containing
  directory moves.
- **One YAML quirk worth knowing:** a leading `&` is YAML's own anchor
  indicator, so a `type`/`check` value that is *only* a bare Sharp hex
  literal (e.g. `&1234`) must be quoted (`type: "&1234"`). Anywhere `&`
  isn't the first character (`NEW&200`, `CALL&C5,X` -- the common case) it's
  unaffected and needs no quoting.

## Top-level fields

```yaml
model: PC-1500
firmware: ../roms/PC-1500_A04.ROM

memory-expansion:
  - address: 0x0000
    size: 16k
  # or, instead of 'size': module: ce163
  # or, both windows at their own max (model-specific): module: cemax
  #   (module: cemaxa for a PC-1500A file)
  # or, a parametrized generalization of ce163:
  #   module: ce168n
  #   banks: 4
  #   first-read-only-bank: 2
  #   bank-content:
  #     - bank: 2
  #       path: ../roms/some_flash_image.bin

rom-modules:
  - slot: 1
    address: 0xA000
    require-pv: false
    use-pu-bank: false
    path: ../roms/some_module.bin

program:
  path: memtest.bin
  format: binary
  address: 0x00C5
```

- **`model`** (required): `PC-1500` or `PC-1500A`. Applied via the FIFO
  `setmachine` command (`1500`/`1500a`) before any `memory-expansion` setup
  and the initial `reset` -- it must precede `setextram` since the base
  unit selects where the 0x4800 extension-RAM window actually lives
  (0x4800-based on a PC-1500, 0x5800-based on a PC-1500A; see pc1500emu's
  `Bus::extRamExtBase()`).
- **`firmware`** (optional): path to a 16KB system ROM dump, loaded at
  `0xC000`. Defaults to `roms/PC-1500_A04.ROM` (resolved next to the preset
  file) when omitted. Passed to the emulator as its positional ROM argument
  on launch -- see README's "How a preset gets applied" section.
- **`memory-expansion`** (optional): a list, up to one entry per window,
  each `address` plus either `size` or `module` -- `address` is `0x0000`
  or `0x4800` (the emulator's two extension-RAM windows). `size` is a byte
  count, optionally suffixed `k`/`K` for KiB (`16k` = 16384), and applies
  via the FIFO `setextram` command. `module` is only valid at `0x0000`,
  and accepts `ce163`, `ce155`, `cemax`/`cemaxa`, or `ce168n`:
  - `ce163` -- the CE-163 memory module (128K of RAM, banked into the
    `0000H`-`3FFFH` window; see `../pc1500emu/README.md` for how bank
    selection works), applied via the FIFO `setce163 1` command.
  - `ce155` -- the real 1982-era CE-155 module: 8K split across both
    windows (2K isolated at `3800H`-`3FFFH`, plus 6K filling the entire
    expansion window -- `4800H`-`5FFFH` on a PC-1500, `5800H`-`5FFFH` on a
    PC-1500A), applied via the FIFO `setce155 1` command; takes no
    additional fields. Unlike `ce163`/`cemax`/`ce168n`, its `0000H`
    footprint is only that top 2K, not the whole window -- addresses below
    `3800H` in that window are unmapped.
  - `cemax` (PC-1500 files) / `cemaxa` (PC-1500A files) -- both
    extension-RAM windows at their own already-existing max size at once:
    16K at `0000H`-`3FFFH` plus the full expansion window, 10K on a
    PC-1500 (`cemax`) or 6K on a PC-1500A (`cemaxa`). Not a real Sharp
    module, and not its own emulator feature either -- this is pure
    preset-loader sugar for a `size: 16384` entry at `0x0000` plus a
    `size: 10240`/`size: 6144` entry at `0x4800`, confirmed empirically
    bit-for-bit identical (same `BASPRG_END`/end-of-RAM pointers, same
    `MEM` reading, same writable footprint) to `pc1500emu`'s own
    `setce128k` FIFO command, which does exactly that same thing
    internally (`Bus::setCe128kEnabled` just sets those two existing
    knobs to their own maxima -- no separate memory-mapping logic exists
    on the emulator side). Sending neither `setextram`/module command --
    this expands to plain `setextram` calls, same as a `size:` entry.
    Picking the wrong one for the file's `model` is a parse-time error
    (`cemax` on a PC-1500A, or vice versa, would otherwise configure an
    expansion-window size larger than that model's real window, spilling
    past `0x7000` into fixed system RAM -- `pc1500emu`'s `setextram`
    doesn't clamp this itself).
  - `ce168n` -- a parametrized generalization of CE-163: same
    `0000H`-`3FFFH` window and fixed 16K bank size, but with the bank
    count and a first-read-only-bank boundary as parameters, applied via
    the FIFO `setce168n <banks> <first-read-only-bank>` command. Requires
    two additional fields on the entry:
    - **`banks`** (required): total number of 16K banks, `>= 1`.
    - **`first-read-only-bank`** (required): the index of the first bank
      that simulates flash (normal writes discarded, but `bank-content`
      can still seed it). A value `>= banks` means "every bank is
      writable".
    - **`bank-content`** (optional): a list of `{bank, path}` entries
      preloading specific banks' contents (any bank, not just read-only
      ones -- a writable bank preloaded with initial RAM content is
      reasonable too). Each `path` is resolved relative to the preset
      file's own directory, same convention as `firmware`/
      `rom-modules[].path`/`program.path`. Each `bank` must be `< banks`.
      Only valid alongside `module: ce168n` -- rejected under `module:
      ce163` or a plain `size` entry.

  `ce163`, `ce155`, and `ce168n` (and `cemax`/`cemaxa`, via the plain
  `setextram` calls they expand to) all apply before `reset`, since the
  ROM only detects installed extension RAM/CE-163/CE-155/CE-168N at
  reset/cold-start; `ce168n`'s `bank-content` entries are applied after
  `reset` instead (see "Load pipeline" below), since bank-select addresses
  only mean anything once the module is enabled and sized.
  `size` and `module` can't both be given in the same entry, and
  `module: ce163`, `module: ce155`, `module: cemax`/`cemaxa`, and
  `module: ce168n` are mutually exclusive with each other and with a
  `size` entry at either window (since `cemax`/`cemaxa` set both
  `size`-equivalent fields directly, giving both a `module: ce163` entry
  and a `module: cemax` entry in the same file is rejected the same way
  two conflicting `size` entries at the same window would be). Every
  `memory-expansion` field applied over the FIFO is sent
  unconditionally as part of applying a preset, including an explicit
  "off" for every option a given file doesn't mention -- pc1500emu is
  driven headless here, so an omitted field must mean "no expansion," not
  "leave whatever an earlier run configured."
- **`rom-modules`** (optional): a list of CE-150/153/158-style plug-in ROM
  modules at `0x8000`-`0xBFFF`, each with `slot` (`1`-`4` or `auto`, for
  slot 1), `address` (hex load address), `require-pv`/`use-pu-bank`
  (booleans, default `false` -- see `../pc1500emu`'s own
  `Bus::RomModule` comment for what they mean), and `path`. Printer
  (CE-150) and serial (CE-158) peripherals aren't emulated, so they're out
  of scope here too. Applied via the FIFO `loadrommodule` family of
  commands.
- **`program`** (optional, at most one): a mapping with `format` (default
  `binary`), `address`, and exactly one of `path` or `text`:
  - `format: binary` loads a flat byte range at `address` (required in this
    mode) via the FIFO `loadbinary` command -- same semantics as CLOAD M.
  - `format: basic-tokenized` loads already-tokenized BASIC program bytes
    into the fixed program area via the FIFO `loadbasic` command
    (`address` is ignored).
  - `format: basic-text` types a plain-text BASIC listing through the
    ROM's own line editor via the FIFO `loadbasictext` command (`address`
    is ignored).
  - **`path`**: an external file, resolved relative to the preset file's
    own directory, same as `firmware`/`rom-modules[].path`.
  - **`text`**: the program's source inlined directly in the preset, as a
    YAML block scalar -- only valid with `format: basic-text` (there's no
    reasonable inline notation for raw binary bytes or tokenized BASIC, so
    those still require `path`). Since the FIFO `loadbasictext` command
    only accepts a path, `pc1500preset` writes `text` out to a temp file
    right before sending that command and removes it right after, whether
    or not the load succeeded -- this is invisible from the preset
    author's side, just an implementation detail of `main.cpp`'s
    `applyPreset()`. Good for short, throwaway listings you'd rather keep
    in one diffable file than split across a `.pc1500` and a companion
    `.bas`; a longer or reused program is still better off as its own file
    via `path`.

## Hex and address literals

`memory-expansion[].address`, `rom-modules[].address`, and `program.address`
are all plain hexadecimal: `0xXXXX` (recommended) or bare hex digits. These
are structural fields, not values typed into the machine, so they don't use
the PC-1500's own `&XXXX` BASIC hex-literal syntax -- that's reserved for
`type`/`check` values below, since those are literally keystrokes sent to
the ROM's line editor.

## Script sections: `pre-load-keys:` / `post-load-keys:`

Two insertion points around the program load -- `pre-load-keys` runs right
after the cold-boot reset (before the program is loaded), `post-load-keys`
runs after it's resident. Each is a YAML sequence of single-key mappings,
executed in order:

```yaml
pre-load-keys:
  - key: cl
  - type: NEW&200
  - key: mode

post-load-keys:
  - type: CALL&C5
```

Each step is one complete, executed-in-order statement, of four kinds:

- **`key: <name>`** -- a single physical key tap, no implicit Enter.
  `<name>` is one of the PC-1500 emulator's own key names (same vocabulary
  as the FIFO `key`/`presskey` commands): `cl`, `enter`, `mode`, `def`,
  `sml`, `rcl`, `shift`, `off`, `up`/`down`/`left`/`right`, `f1`-`f6`,
  `space`, a single letter/digit, or any of those prefixed `shift+` (e.g.
  `key: shift+f1`) for a genuine PC-1500 Shift-tap first. Forwarded to the
  emulator's own FIFO `key` command verbatim, which parses this same
  vocabulary itself.
  - **`key: sml` caveat**: unlike Shift (a one-shot modifier for the very
    next key -- see `type`'s own lowercase handling below), SML is a
    **persistent** lowercase-input toggle on real hardware: it stays in
    effect across every key that follows, including subsequent script
    steps and any `type` step's own per-letter Shift-taps, until another
    `key: sml` toggles it back off. A `key: sml` step anywhere in a
    script therefore inverts the case of everything typed after it for
    the rest of the script (letters that would've been uppercase come out
    lowercase, and -- since `type`'s lowercase handling adds its own
    Shift-tap on top -- a lowercase letter in `<text>` comes out
    uppercase again while SML is active). This is real PC-1500 keyboard
    behavior, faithfully reproduced, not a `pc1500preset` bug -- **this
    format makes no attempt to track or compensate for SML state**, so
    avoid `key: sml` in a script that also uses lowercase `type` text, or
    account for the inversion by hand if you do use both.
- **`type: <text>`** -- types `<text>` through the ROM's own tokenizing
  line editor, **followed by an automatic Enter**. Implemented via the
  FIFO `typeline` command, which types and presses Enter synchronously
  (unlike the raw FIFO `type` command, which only queues keystrokes and
  does not auto-submit) -- exactly the "one complete statement per step"
  semantics this format wants. `<text>` uses the PC-1500's own `&XXXX` hex
  syntax freely (e.g. `NEW&200`, `CALL&C5,X`) since it's typed verbatim
  into the machine; quote it if it would otherwise start with `&` (see
  "Syntax" above). Before sending it, `pc1500preset` first waits for the
  machine's own BUSY indicator to clear (see "Waiting for readiness"
  below) -- `typeline` steps the CPU directly rather than queuing into the
  normal keyboard path, so it must not be sent while a program (e.g. a
  minutes-long memory test triggered by an earlier `type: CALL...`) still
  has the CPU.
  - **Lowercase letters in `<text>`**: the PC-1500 keyboard has one
    physical key per letter, not separate upper/lowercase keys -- which
    case it types is a ROM keyboard-dispatch mode, not a separate
    keystroke. `typeline` alone (like typing on real hardware with no
    modifier held) always types uppercase. A genuine lowercase letter in
    `<text>` is produced the same way pressing the real Shift key before a
    letter does: `pc1500preset` splits a `<text>` containing lowercase
    letters into runs, sending each non-lowercase run whole via the FIFO
    `typelinenoenter` command (so it accumulates on the same input line
    without submitting) and each lowercase letter individually via `key
    shift+<letter>` -- a one-shot Shift-tap, not the ROM's separate,
    *persistent* SML lowercase-mode toggle (see the `sml` caveat right
    below). The whole line is still submitted with one Enter at the end,
    same as an all-uppercase `type` step.
- **`wait: <seconds>`** -- an unconditional real-time delay (decimal
  seconds, e.g. `wait: 20` or `wait: 2.5`), independent of machine state.
  Since `type`/`check` already wait for readiness on their own, this is
  for pure pacing -- e.g. giving a human watching the emulator window a
  moment to actually see a result before the script races on to the next
  step.
- **`check: <expected>`** -- waits for readiness (as `type` does above),
  then reads the actual dot-matrix display (the FIFO `display` command)
  and searches it for `<expected>`'s glyphs, at any horizontal position on
  the line. This is *not* the same as reading `displaytext` back:
  `displaytext` only reflects the last typed input line verbatim, not what
  a statement actually evaluated to -- confirmed directly, e.g.
  `type: PRINT "HELLO"` renders "HELLO" legibly on the real dot-matrix
  while `displaytext` still shows the raw `PRINT "HELLO"` source text. So
  `check` works the other way around: it encodes `<expected>` into the
  PC-1500's own dot-matrix glyphs and looks for that pattern in the live
  screen, which is what a human watching the emulator actually sees.
  **Only `check: 0` is supported today** -- `0` is the only expected value
  with a known glyph encoding so far (the ROM's zero-with-a-slash digit, to
  disambiguate it from letter `O`); any other `check` value is rejected at
  parse time. Each `check` step prints a `PASSED`/`FAILED` line to stdout
  as it runs; `pc1500preset` exits `0` only if every `check` in the script
  passed (or the script had none at all), `1` if any failed -- see "Exit
  code" below.

A step's key (`key`/`type`/`wait`/`check`) and value are a single-entry
YAML mapping (`- key: cl`, not `- key: cl\n  extra: field`) -- one verb per
step, same as before.

### Waiting for readiness

`type` and `check` both poll the emulator's own `status` FIFO command
(specifically its `busy=` bit) before acting, and proceed the moment it
reports idle rather than on a fixed timer -- this is what makes it safe to
`type` or `check` right after a `type: CALL...` that can run for minutes
(e.g. a memory test), without either an unconditionally long fixed wait or
racing a program that's still running. `key` script steps, by contrast,
only queue into the emulator's normal frame-driven keyboard path (the same
one real typing uses), so they're always safe to send regardless of
machine state; `pc1500preset` still pauses a flat 0.5 real-time seconds
after one, a prototype-grade stand-in for watching the display to confirm
the tap actually landed. See this project's `README.md` for why timing
here is real-time waits/polling rather than the cycle-exact stepping an
in-process applier could use.

### Exit code

`pc1500preset` exits `0` when the preset applied successfully and every
`check` step (if any) passed, `1` if any `check` failed. A parse error or a
failed FIFO command (e.g. a bad ROM-module path) still fails loudly and
exits non-zero independent of `check` steps, same as before this format
existed. The script always runs to completion regardless of an earlier
`check` failing -- a failing `check` doesn't abort the rest of the script,
it's only tallied into the exit code.

### Process lifecycle: interactive vs. test presets

If the script has no `check` steps, the emulator process is left running
after `pc1500preset` exits, for interactive use -- same as before `check`
existed. Automation mode (see step 3 below) is turned back off first, so
the real keyboard works immediately once control is handed back. If it has
at least one `check` step, that's a test run rather than a demo:
`pc1500preset` terminates the emulator (SIGTERM, falling back to SIGKILL
after a short grace period) once the whole script has finished, rather
than leaving a window open indefinitely.

## Load pipeline (fixed order, always a cold boot)

1. Parse and validate the whole file. Any error (bad hex, missing required
   field, unrecognized field) fails before the emulator is even launched --
   a preset is applied all-or-nothing.
2. Launch `../pc1500emu` with `firmware` as its positional ROM argument
   and `--no-state` (so it never resumes a `--conf`-configured session).
3. Wait for its FIFO command interface to come up, then send
   `automation on`.
4. Clear every memory-expansion option (`setextram 0000 0`, `setextram ext
   0`, `setce163 0`, `setce155 0`, `setce168n 0 0`), then apply
   `memory-expansion` window sizes via `setextram` (`module:
   cemax`/`cemaxa` included -- see above), or enable the CE-163/CE-155/
   CE-168N module via `setce163`/`setce155`/`setce168n` for a `module:
   ce163`/`module: ce155`/`module: ce168n` entry. The clearing step runs
   regardless of what the preset specifies, so a file with no
   `memory-expansion` section reliably boots with none, rather than
   inheriting state from an earlier run against the same emulator process
   or its persisted conf file.
5. Load `rom-modules` attachments into their slots via `loadrommodule`.
6. Send `reset`.
7. If either script section or `bank-content` is non-empty, wait out the
   ROM's own power-on RAM-check/boot sequence before sending any keystrokes
   or bank-select writes -- confirmed empirically (by an earlier
   in-process prototype of this format) that keys sent immediately after
   reset are missed entirely, since the ROM doesn't start polling the
   keyboard until it settles into its post-boot idle loop.
8. Apply `memory-expansion`'s `ce168n` `bank-content` entries: for each,
   select the target bank (a write to `5800H + bank`, same trigger
   mechanism CE-163 itself uses) and `loadbinary` the file into it.
9. Replay `pre-load-keys`.
10. Load `program` at its `address` (per `program.format`).
11. Replay `post-load-keys`.

There is no delta/attach-to-a-running-instance mode -- a preset always
starts from a cold boot. The emulator process is left running afterward for
interactive use; `pc1500preset` itself exits once the pipeline completes.

## Example

`samples/memtest.pc1500` boots a stock PC-1500, expands RAM at
`0x0000` by 16K, reserves memory below `&200` (`NEW&200`) so the memory
tester loaded at `0x00C5` isn't overwritten by BASIC's own program area, and
starts it with `CALL&C5,X` (`X` here is the memory tester's own parameter
for how many passes to run, not a preset-format concept):

```yaml
model: PC-1500
firmware: ../roms/PC-1500_A04.ROM

memory-expansion:
  - address: 0x0000
    size: 16k

pre-load-keys:
  - key: cl
  - type: NEW&200
  - key: mode

program:
  path: memtest.bin
  format: binary
  address: 0x00C5

post-load-keys:
  - type: X=2
  - type: CALL&C5,X
  - wait: 20
  - key: cl
  - type: X
  - check: 0
```

The memory test can take minutes; `type: CALL&C5,X` itself only blocks
until the `CALL` statement is *accepted* by the ROM's line editor -- the
routine then keeps running in the background via the emulator's normal
frame loop, so the next `type`/`check` step's own readiness wait is what
actually blocks until it's done. `key: cl` clears the input line before
typing the bare `X` short-form-`PRINT` query -- skipping it risks
concatenating onto the leftover `CALL&C5,X` text and getting a syntax
error instead (this project's own established convention, see
`pre-load-keys` above). `check: 0` then confirms a `0` is actually visible
on the dot-matrix display -- the memory test's own convention for "no
errors found". `wait: 20` isn't needed for correctness here (`type`/`check`
already wait for readiness on their own) -- it's there so a human watching
the emulator window gets a moment to see the result before the script
moves on.

Run it with:

```
./build/src/pc1500preset samples/memtest.pc1500
```

`samples/ce163_bankswrm_bin.pc1500` shows a `module: ce168n` memory
expansion with `banks: 8` and `first-read-only-bank: 8` -- the same 128K
capacity as the CE-163 module it used to enable, but with every bank left
writable (`first-read-only-bank >= banks`). A `bank-content` entry
preloading a read-only bank looks like this instead:

```yaml
memory-expansion:
  - address: 0x0000
    module: ce168n
    banks: 4
    first-read-only-bank: 2
    bank-content:
      - bank: 2
        path: ../roms/some_flash_image.bin
```

Banks below `first-read-only-bank` (here, 0 and 1) behave like ordinary
CE-163 RAM banks -- writable, blank at boot unless also named in
`bank-content`. Banks at/above it (here, 2 and 3) simulate flash: normal
writes are discarded, but `bank-content` can still seed them, which is how
a preset gets flash content into the machine in the first place.

`samples/iterator.pc1500` shows the `program.text` inline form (no
companion `.bas` file, no `check` -- it's a demo, not a test):

```yaml
model: PC-1500
firmware: ../roms/PC-1500_A04.ROM

program:
  format: basic-text
  text: |
    10 WAIT 0
    20 FOR I = 1 TO 100
    30 PRINT I
    40 NEXT I

post-load-keys:
  - key: cl
  - key: mode
  - type: RUN
```
