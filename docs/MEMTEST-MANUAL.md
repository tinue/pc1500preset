# MEMTEST — 16K Memory Card Test for Sharp PC-1500 / PC-1500A

## Purpose

MEMTEST is a short machine-code utility that tests every accessible byte of a CE1638 or CE163F 16K memory module.  It writes four test patterns to every byte in the testable window, reads each byte back, and compares.  If a mismatch is found the bad address and the expected and actual data values are stored in a small result area inside the module RAM, where you can inspect them with `PEEK`.  If all bytes pass, a flag byte is left as zero.

### Test patterns

| Pass | Pattern | Binary     | Purpose                        |
|------|---------|------------|--------------------------------|
| 1    | `&55`   | 0101 0101  | Alternating low                |
| 2    | `&AA`   | 1010 1010  | Alternating high (complement)  |
| 3    | `&FF`   | 1111 1111  | All ones                       |
| 4    | `&00`   | 0000 0000  | All zeros                      |

The four-pattern sequence detects stuck-at-one/zero faults on individual bit lines and a large class of address-decoder faults.

> **Warning:** The test overwrites every tested byte.  Any BASIC program stored in the module will be destroyed.  Save any programs you want to keep before running MEMTEST.

---

## Prerequisites

1. A Sharp PC-1500 or PC-1500A with a CE1638 or CE163F module installed.
2. The MEMTEST binary loaded into the module (see Loading procedures below).
3. No BASIC program on the module that you wish to keep — the test overwrites it.

The module's banking firmware (the stub at `&00C5`–`&00C4`) must already be in place.  If you can use `CALL &D5` (CE1638) or `CALL &E3` (CE163F) without error, the firmware is intact.

---

## Memory layout

After loading, the code occupies addresses `&00C5`–`&015B` (151 bytes).

```
Address   Contents
-------   --------
&00C5     8E 05        BCH MEMTEST — forward branch skipping result area
&00C7     ERR_FLAG     0 = pass,  1 = fault found
&00C8     ERR_ADDR_H   High byte of first bad address
&00C9     ERR_ADDR_L   Low byte  of first bad address
&00CA     ERR_EXPCT    Pattern written (expected value)
&00CB     ERR_ACTUAL   Value read back (actual value)
&00CC     … &015B      Test code (MEMTEST entry point at &00CC)
```

The result bytes live inside the module's own RAM and survive power-off (battery backed on CE1638/CE163F).

---

## Loading procedure — PC-1500 (CE1638 or CE163F)

### Step 1 — Clear any existing program

```
NEW &104      ' CE1638: protect firmware
```
or
```
NEW &112      ' CE163F: protect firmware
```

### Step 2 — Load the binary

Transfer `memtest.obj` to the PC-1500 using your preferred method (CLOAD from cassette, CE-126P printer/cassette interface, or a modern transfer tool).

The binary must be loaded to address **`&00C5`**.

### Step 3 — Protect the code with NEW

```
NEW &200
```

This tells BASIC that its program area begins at `&0200`, leaving `&00C5`–`&01FF` untouched.  The value `&200` is safe for both CE1638 (firmware ends at `&0103`) and CE163F (firmware ends at `&0111`), with a comfortable gap before the test code at `&00C5`.

### Step 4 — Run the test

```
CALL &C5
```

The test scans from the BASIC program-start address (`&0200` after `NEW &200`) to `&3FFF` — four complete passes.  On a 16 MHz PC-1500 this takes roughly 5–10 seconds.  The display will appear frozen during the test; this is normal.

---

## Loading procedure — PC-1500A (full module coverage)

On the PC-1500A the address range `&7C01`–`&7FFF` is a dedicated machine-code area that is **not** bank-switched.  Loading MEMTEST there allows it to test the entire module window from `&0000` to `&3FFF` without any untestable stub region.

### Step 1 — Reassemble for the PC-1500A

In `memtest.asm`, comment out the PC-1500 `ENTRY` line and uncomment the PC-1500A line:

```asm
;ENTRY      .EQU    $00C5       ; PC-1500 (CE1638 / CE163F)
ENTRY       .EQU    $7C01       ; PC-1500A — full module coverage
```

Reassemble:

```sh
tasm -5801 -x7 -g3 memtest.asm memtest_1500a.obj memtest_1500a.lst
```

### Step 2 — Load the binary

Transfer `memtest_1500a.obj` and load it to address **`&7C01`**.

No `NEW` command is needed — the machine-code area is permanently available and does not overlap BASIC.

### Step 3 — Run the test

```
CALL &7C01
```

The test scans from the BASIC program-start pointer (typically the start of the CE1638/CE163F firmware area) to `&3FFF`.  For a complete test including address `&0000`, first set the start pointer:

```
POKE &7863, 0 : POKE &7864, 0
CALL &7C01
```

---

## Reading the results

After `CALL` returns, examine the result bytes with `PEEK`.

### PC-1500 build (ENTRY = `&00C5`)

```basic
PRINT PEEK(&C7)                            ' 0 = pass,  1 = fault
PRINT HEX$(PEEK(&C8)*256+PEEK(&C9))        ' first bad address (if fault)
PRINT HEX$(PEEK(&CA)),HEX$(PEEK(&CB))      ' expected value,  actual value
```

### PC-1500A build (ENTRY = `&7C01`)

All PEEK addresses shift by `&7C01 - &00C5 = &7B3C`:

```basic
PRINT PEEK(&7C03)                           ' 0 = pass,  1 = fault
PRINT HEX$(PEEK(&7C04)*256+PEEK(&7C05))     ' first bad address (if fault)
PRINT HEX$(PEEK(&7C06)),HEX$(PEEK(&7C07))   ' expected value,  actual value
```

---

## Interpreting the results

### All clear — `PEEK(&C7)` returns `0`

Every byte in the tested range passed all four patterns.  The module is functioning correctly for the tested area.

### Fault found — `PEEK(&C7)` returns `1`

A mismatch was detected.  The remaining result bytes tell you exactly where and what failed.

```
PEEK(&C7) = 1               fault flag
PEEK(&C8)*256+PEEK(&C9)     address of first bad byte
PEEK(&CA)                   pattern that was written
PEEK(&CB)                   value that was read back
```

**Example output:**

```
1
1A4F
55
00
```

This means: address `&1A4F` was written `&55` but read back as `&00`.  The bit(s) that differ between `&55` and `&00` identify the stuck or shorted data lines.

### Interpreting the failure pattern

| Written | Read back | Likely fault |
|---------|-----------|-------------|
| `&55`   | `&00`     | Multiple bits stuck low, or address aliasing (writes landing on wrong cell) |
| `&FF`   | `&00`     | Cell(s) or data line stuck low |
| `&00`   | `&FF`     | Cell(s) or data line stuck high |
| `&55`   | `&FF`     | Data bus contention or address decoder fault |
| any     | random    | Intermittent contact or power issue |

If every byte in a large block (e.g. `&1000`–`&1FFF`) fails with the same readback, suspect an address line fault rather than individual cell damage.

If failures appear only under certain patterns, check the specific data bit positions: `&55` = bits 6,4,2,0; `&AA` = bits 7,5,3,1; `&FF` = all bits.

### Intermittent failures

Run the test several times in succession.  If the failing address changes between runs, the fault is intermittent — possibly a cold-solder joint or marginal contact at the expansion connector.  Check the module's connector edge and the PC-1500's expansion slot for debris or oxidation.

---

## Limitations

### Untestable region on PC-1500 (CE1638/CE163F)

When the code runs from `&00C5` inside the module window, the region `&0000`–`&01FF` cannot be tested — writing there would overwrite the firmware stub and the test code itself.  The test starts from the BASIC program-start pointer (typically `&0200` after `NEW &200`).

Bytes `&0000`–`&01FF` are therefore **not tested** by the PC-1500 build.  Use the **PC-1500A build** at `&7C01` for full coverage of all 16K.

### Single-pass, single-address stop

The test stops at the first failing byte; it does not continue to map all bad addresses.  If you suspect widespread damage, note the first address reported, manually POKE that address to a harmless value (to prevent the test stopping there again), and re-run — repeating until the test passes.

### Pattern order

The test writes `&55` first and `&00` last.  After a clean test the entire module window contains `&00`.  A BASIC program loaded onto the module before the test will be destroyed.

### Bank 0 only

The test operates on whichever bank is currently active.  Repeat the test for each bank by switching with `CALL &D5, n` (CE1638) or `CALL &E3, n` (CE163F) between runs.

---

## Injecting a known fault (verification)

To confirm the test is working, you can create an artificial fault:

```basic
NEW &200
CALL &C5                ' should show PEEK(&C7)=0 (pass)
POKE &2000, &AB         ' corrupt one byte
CALL &C5                ' should show PEEK(&C7)=1
PRINT PEEK(&C8)*256+PEEK(&C9)   ' should show 2000
```

If `PEEK(&C7)` returns 1 and the address matches `&2000`, the test is detecting faults correctly.

---

## Quick-reference card

```
  Load (PC-1500):   NEW &200  →  transfer binary to &00C5  →  NEW &200
  Run:              CALL &C5
  Check pass/fail:  PRINT PEEK(&C7)          ' 0=OK, 1=FAIL
  Bad address:      PRINT HEX$(PEEK(&C8)*256+PEEK(&C9))
  Expected/Actual:  PRINT HEX$(PEEK(&CA)),HEX$(PEEK(&CB))

  Load (PC-1500A):  reassemble with ENTRY=$7C01  →  transfer to &7C01
  Run:              CALL &7C01
  Check pass/fail:  PRINT PEEK(&7C03)
  Bad address:      PRINT HEX$(PEEK(&7C04)*256+PEEK(&7C05))
  Expected/Actual:  PRINT HEX$(PEEK(&7C06)),HEX$(PEEK(&7C07))
```
