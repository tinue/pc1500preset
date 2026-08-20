; ============================================================
; MEMTEST_SDAS.ASM -- 16K Memory Card Test for Sharp PC-1500 / PC-1500A
; ============================================================
;
; sdaslh5801 port of memtest.asm (originally written for tasm -5801).
; Logic, register usage, and comments are unchanged from the tasm source
; -- only directive/operand syntax was translated for this project's
; assembler (../sdcc-pc1500/sdcc/bin/sdaslh5801):
;   - $XX / $XXXX hex literals -> 0xXX / 0xXXXX (sdas requires C-style hex)
;   - .EQU/.ORG/.DB -> lowercase .equ/.org/.db
;   - Mnemonics and register names lowercased (sdas is case-sensitive)
;   - No space after "," in two-operand instructions (sdas convention,
;     matches this project's own disassembler output)
;   - `.area CODE (ABS)` + `.org` header, matching the convention this
;     repo's disassembler/VS-Code-extension tooling expects: the "LH5801:
;     Assemble to BIN" command reads the load address straight back out
;     of this pair, no separate address prompt needed.
;
; Tests the CE1638 or CE163F 16K memory module for faults by
; writing four patterns to every byte in the testable window,
; reading each byte back, and comparing.  At the first mismatch
; the bad address and data values are saved in the result area
; and the test stops.  On a clean pass ERR_FLAG is 0.
;
; Test patterns (per full iteration, in order): 0x55, 0xAA, 0xFF, 0x00
;
; Assembly command:
;   sdaslh5801 -plosgff memtest_sdas.asm
;   (or use the lh5801-asm VS Code extension's "LH5801: Assemble to BIN")
;
; --- Build variants ---
; PC-1500 with CE1638 or CE163F:
;   ENTRY = 0x00C5  (stub ends at 0xC4; code ends well before 0x0200)
;   Before running: NEW &200
;   Run:            X=10: CALL &C5,X
;   Protect:        NEW &200   (safe for both CE1638 and CE163F)
;
; PC-1500A (full module coverage, code outside module window):
;   ENTRY = 0x7C01
;   Before running: (no NEW needed -- machine-code area is not bank-switched)
;   Run:            X=10: CALL &7C01,X
;
; ============================================================
; Calling convention (CALL address, variable):
;
;   - The BASIC variable's value is placed in the X register before entry.
;   - If the routine returns with carry set, the X register value is
;     written back to the variable.
;   - This routine always sets carry on return, so the result is always
;     stored back into the BASIC variable.
;
; Result codes (returned in X register AND stored in ERR_FLAG):
;   0 = all iterations passed
;   1 = fault found (see ERR_ADDR_*, ERR_EXPCT, ERR_ACTUAL)
;   2 = test not run (iteration count was zero)
;
; Iteration count: pass the number of full 4-pattern sweeps via the
;   BASIC variable (e.g. X=10).  Supported range: 1-255.
;   If 0 is passed, the test is skipped and result code 2 is returned.
;
; ============================================================
; Result PEEK addresses  (PC-1500 build, ENTRY=0x00C5)
;
;   PRINT PEEK(&C7)                             ; 0=pass, 1=fail, 2=not run
;   PRINT HEX$(PEEK(&C8)*256+PEEK(&C9))         ; first bad address
;   PRINT HEX$(PEEK(&CA)),HEX$(PEEK(&CB))       ; expected, actual
;
; For PC-1500A build (ENTRY=0x7C01) add 0x7B3C to every address:
;   PRINT PEEK(&7C03)                           ; 0=pass, 1=fail, 2=not run
;   PRINT HEX$(PEEK(&7C04)*256+PEEK(&7C05))     ; first bad address
;   PRINT HEX$(PEEK(&7C06)),HEX$(PEEK(&7C07))   ; expected, actual
;
; ============================================================

; --- Build selection: uncomment ONE line ---
ENTRY      .equ    0x00C5      ; PC-1500 (CE1638 / CE163F)
;ENTRY       .equ    0x7C01      ; PC-1500A -- full module coverage

; --- System RAM pointers (read-only, never written by this code) ---
; BASIC Bottom = first byte of testable user RAM (start of test range).
; Start = end of used BASIC area (0x7867/0x7868) = first free byte.
; This preserves any BASIC program in memory.
; End of range is hardcoded: last byte tested is 0x3FFF (full module window).
BOTTOM_H    .equ    0x7867      ; High byte of BASIC used-area end (= first free byte, 0x0112 with no program)
BOTTOM_L    .equ    0x7868      ; Low byte  of BASIC used-area end

; ============================================================
            .area   CODE (ABS)
            .org    ENTRY
; ============================================================

; +0  Two-byte forward branch; lands at MEMTEST (+7), skipping
;     the five result bytes.
            bch     MEMTEST     ; 8E 05

; +2  Result area -- read these with PEEK() after CALL
ERR_FLAG:   .db     0x00        ; 0 = all passes clean, 1 = fault found, 2 = not run
ERR_ADDR_H: .db     0x00        ; High byte of first bad address
ERR_ADDR_L: .db     0x00        ; Low byte  of first bad address
ERR_EXPCT:  .db     0x00        ; Pattern written   (expected value)
ERR_ACTUAL: .db     0x00        ; Value read back   (actual value)

; ============================================================
; MEMTEST -- main test routine, entered via CALL from BASIC
; ============================================================
MEMTEST:                        ; ENTRY+7

; Initialise: clear ERR_FLAG.
            ldi     a,0x00
            sta     (ERR_FLAG)

; Check whether the iteration count (X register on entry) is zero.
; CPA compares A with a register; A is still 0x00 from the LDI above.
            cpa     xh          ; flags = 0x00 - xh
            bzr     COUNT_OK    ; xh != 0 -> count is nonzero, proceed
            cpa     xl          ; xh==0, flags = 0x00 - xl
            bzr     COUNT_OK    ; xl != 0 -> count is nonzero, proceed

; Count is zero -- mark as "not run" and return.
            ldi     a,0x02
            sta     (ERR_FLAG)
            ldi     xh,0x00
            ldi     xl,0x02
            sec                 ; carry set -> BASIC writes X register back to variable
            rtn

COUNT_OK:
; Save the iteration count into UL, biased down by one: LOP branches
; back unless UL was already 0 *before* its decrement, so priming it
; with (count-1) makes the OUTER loop below run exactly `count` times.
; Supported range: 1-255 (XH is not used for the count).
            lda     xl
            sta     ul
            dec     ul

; Load Y with the end sentinel (0x4000 = one past last module byte 0x3FFF).
            ldi     yh,0x40
            ldi     yl,0x00

; ============================================================
; OUTER -- one full sweep of all four patterns
; ============================================================
OUTER:
; ============================================================
; Pass 1 -- pattern 0x55 (01010101)
; ============================================================
PASS1:
            lda     (BOTTOM_H)          ; X = first free byte (0x0112 with no BASIC program)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            lda     xh                  ; clamp: if BASIC ptr is in internal RAM (>= 0x4000),
            cpa     yh                  ;   fall back to module start 0x0112.
            bcr     PASS1_OK            ;   carry clear (C=0) = xh < 0x40 = pointer in module space, ok
            ldi     xh,0x01             ;   pointer >= 0x4000: no BASIC program in module
            ldi     xl,0x12
PASS1_OK:
            ldi     a,0x55
            sta     uh                  ; UH holds current pattern
LOOP1:      lda     uh
            sta     (x)                 ; write pattern
            lda     (x)                 ; read back
            cpa     uh                  ; compare (flags = a - uh)
            bzr     ERROR               ; Z=0 -> mismatch -> record & stop

            inc     x                   ; advance pointer

            lda     xh                  ; 16-bit end check: X vs Y
            cpa     yh                  ; flags = xh - yh
            bcr     LOOP1               ; C=0: xh < yh -> keep going
            bzr     NEXT1               ; C=1 and Z=0: xh > yh -> pass done (skip xl test)
            lda     xl
            cpa     yl                  ; flags = xl - yl
            bcr     LOOP1               ; C=0: xl < yl -> keep going
NEXT1:
; ============================================================
; Pass 2 -- pattern 0xAA (10101010)
; ============================================================
PASS2:
            lda     (BOTTOM_H)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            lda     xh
            cpa     yh
            bcr     PASS2_OK
            ldi     xh,0x01
            ldi     xl,0x12
PASS2_OK:
            ldi     a,0xAA
            sta     uh

LOOP2:      lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR

            inc     x

            lda     xh
            cpa     yh
            bcr     LOOP2
            bzr     NEXT2
            lda     xl
            cpa     yl
            bcr     LOOP2
NEXT2:

; ============================================================
; Pass 3 -- pattern 0xFF (11111111)
; ============================================================
PASS3:
            lda     (BOTTOM_H)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            lda     xh
            cpa     yh
            bcr     PASS3_OK
            ldi     xh,0x01
            ldi     xl,0x12
PASS3_OK:
            ldi     a,0xFF
            sta     uh

LOOP3:      lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR

            inc     x

            lda     xh
            cpa     yh
            bcr     LOOP3
            bzr     NEXT3
            lda     xl
            cpa     yl
            bcr     LOOP3
NEXT3:

; ============================================================
; Pass 4 -- pattern 0x00 (00000000)
; ============================================================
PASS4:
            lda     (BOTTOM_H)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            lda     xh
            cpa     yh
            bcr     PASS4_OK
            ldi     xh,0x01
            ldi     xl,0x12
PASS4_OK:
            ldi     a,0x00
            sta     uh

LOOP4:      lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR

            inc     x

            lda     xh
            cpa     yh
            bcr     LOOP4
            bzr     NEXT4
            lda     xl
            cpa     yl
            bcr     LOOP4
NEXT4:
; All four passes completed without error.
; LOP decrements UL and branches back to OUTER unless UL was already 0
; (see the priming comment at COUNT_OK above).
            lop     ul,OUTER

; ============================================================
; All iterations completed without error.
; ============================================================
            ldi     xh,0x00
            ldi     xl,0x00
            sec                         ; carry set -> BASIC writes result to variable
            rtn

; ============================================================
; ERROR -- record the first mismatch and return to BASIC
; ============================================================
; On entry: A = value actually read, X = bad address, UH = pattern written.
ERROR:
            sta     (ERR_ACTUAL)        ; save the bad readback
            lda     uh
            sta     (ERR_EXPCT)         ; save the expected pattern
            lda     xh
            sta     (ERR_ADDR_H)        ; save bad-address high byte
            lda     xl
            sta     (ERR_ADDR_L)        ; save bad-address low byte
            ldi     a,0x01
            sta     (ERR_FLAG)          ; mark failure
            ldi     xh,0x00
            ldi     xl,0x01
            sec                         ; carry set -> BASIC writes result to variable
            rtn
