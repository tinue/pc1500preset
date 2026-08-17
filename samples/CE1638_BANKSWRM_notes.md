# `CE1638_BANKSWRM` -- suspected firmware defect (edit pointer)

## Symptom

After `CE1638_BANKSWRM.BAS` / `CE1638_Firmware.bin` runs (via
`samples/ce163_bankswrm_bin.pc1500`) and pokes a tiny BASIC program (`10
PRINT "1"`, expected to be ~9 tokenized bytes) into the CE-163 window, the
calculator displays a corrupted program line: `34819:_      ~~`.

`34819` decimal is `8803H` -- and `88 03` appears verbatim as a byte pair
inside the leftover machine-code data that `CE1638_BANKSWRM`'s own routine
left in the CE-163 bank (part of the POKE'd ML routine, not BASIC token
data). This is consistent with the BASIC line-list renderer walking off
the end of the real tokenized program and misreading stray ML-routine
bytes as the next line's header.

## Pointer inspection (from a saved `.state` file, CE-163 bank 7 active)

| Pointer | Address | Value |
|---|---|---|
| `BASPRG_ST` (program start) | `7865H` | `0104H` |
| `BASPRG_END` (program end, used by `MEM`) | `7867H` | `010DH` |
| `BASPRG_EDT` (editor pointer) | `7869H` | `1104H` |

`ST` and `END` are 9 bytes apart -- exactly consistent with a small
tokenized `10 PRINT "1"` line, and both look internally consistent with
each other and with the CE-163 bank contents (bank 0-6, which weren't
active at save time, independently show the same 9-byte layout ending at
`0104H`, matching `ST`).

`EDT`, however, is `1104H` -- **over 4KB past `END`**, nowhere near either
of the other two pointers, and not explainable by the CE-163 bank data
itself.

## Suspicion

`CE1638_BANKSWRM`'s own ML routine is expected to set the edit pointer
(`BASPRG_EDT`, `7869H`) to the same value as the program-end pointer
(`BASPRG_END`, `7867H`) once it's done storing the program -- and appears
to fail to do so, leaving `EDT` at a stale/uninitialized value instead.
Since the ROM's line editor uses `EDT` to know where to resume
editing/listing from, a stale `EDT` pointing far past the real end of the
program would explain the renderer reading garbage (the `88 03` bytes)
and displaying a corrupted line number.

This matches a symptom pattern known from real hardware (a corrupted
`<garbage>:_ ~~` line display generally indicating broken BASIC pointers)
-- **but here, unlike the classic real-hardware case, `ST`/`END` are not
broken, only `EDT` is.** That's a narrower, more specific failure than the
usual "pointers corrupted" case.

## Cross-check: `CE1638_BANKSW` (no "RM" suffix)

`CE1638_BANKSW.BAS`/`CE1638_BANKSW.bin` -- a separate, smaller (112-byte)
routine, confirmed proven-good on real hardware -- was run the same way
(`samples/ce163_bankswrm_bin2.pc1500`) and confirmed working correctly
end-to-end, with no corrupted-line symptom. This is evidence the defect is
specific to `CE1638_BANKSWRM`'s own ML routine (most likely a missing or
incorrect write to `7869H` right after it finishes storing the program),
not a bug in `pc1500emu`'s CE-163 emulation or in this project's `.pc1500`
tooling -- both handled `CE1638_BANKSW` correctly under otherwise
identical conditions (same CE-163 8-bank setup, same load path).

## Status

Unconfirmed -- next step is tracing `CE1638_BANKSWRM`'s own ML source (not
in this repo) around wherever it's meant to update `7869H`, to find the
missing/incorrect write directly, rather than inferring it from symptoms
alone.
