;
;       {_______            {_          {______
;             {__          {_ __               {__
;             {__         {_  {__               {__
;          {__           {__   {__               {__
;      {______          {__     {__              {__
;            {__       {__       {__            {__
;              {_________         {______________		Expansion Unit
;
;      gpu64 -- shared harness for the API conformance suite.
;
;      Every gpu64_test_*.a includes this. It is not a program on its own.
;
;      WHY A HARNESS
;
;      The suite's whole value is that a bench run produces a verdict, not a
;      screenful of numbers to interpret. A tester should be able to look at
;      one line from across the room and know whether to look closer. So the
;      harness owns all of the output and the tests own none of it: a test
;      computes a fail code and calls one macro.
;
;      A fail code of 0 is a pass. Anything else is printed as F<hex> and
;      the convention is that it is *the byte that was wrong* wherever there
;      is one -- almost always the unexpected ERRCODE, so a failure line
;      reads back as "this opcode answered $04 when it should have answered
;      $00" without any lookup table. Codes that cannot be an ERRCODE are
;      used for the failures that are not about ERRCODE at all:
;
;        $E1  a buffer did not match what it was compared against
;        $E2  a buffer was not uniformly filled with the expected byte
;        $E3  a single byte read back from the framebuffer was wrong
;        $E4  a value was in range when it should not have been, or the
;             other way round -- used where there is no byte to show
;        $FE  the command returned OK where an error was required. This one
;             is worth calling out: it means an error path did not fire, and
;             an error path that never fires is not a tested error path.
;
;      USING IT
;
;      Layout of a test program:
;
;              .include "gpu64_testkit.inc"    ; equates + macros
;      start   jsr kitInit
;              #title "GPU64 SOMETHING TEST"
;              ... tests ...
;              jmp kitFinish
;              .include "gpu64_testkit_rt.inc" ; the runtime, once, at the end
;
;      A test is three steps: stage arguments, dispatch, judge.
;
;              #argb 0, 6
;              lda #OP_CLEAR
;              jsr cmd0                ; A = ERRCODE
;              #wantOk                 ; X = fail code
;              #check "CLEAR"          ; prints and counts
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; The register window. gpu64 owns $DF0B-$DFFF; REU keeps $DF00-$DF0A.
; ----------------------------------------------------------------------

CMD_HI   = $df0b
CMD_LO   = $df0c
STATUS   = $df0d
ERRCODE  = $df0e
ID_LO    = $df0f
ID_HI    = $df10
ARG      = $df11
RESULT   = $df21

ST_BUSY		= $01
ST_ERROR	= $02
ST_VB_PEND	= $04
ST_VB_ARMED	= $08
ST_FRAME_RDY	= $10

; --- class 0 opcodes ---------------------------------------------------
OP_NOP		= $00
OP_RESET_STATE	= $01
OP_VBLANK_ARM	= $02
OP_VBLANK_ACK	= $03
OP_SET_DRAW_PAGE = $04
OP_PAGE_FLIP	= $05
OP_GET_INFO	= $06
OP_LOG_ENABLE	= $07
OP_SET_BORDER	= $08
OP_VBLANK_SYNC	= $09
OP_CLEAR	= $10
OP_SET_PIXEL	= $20
OP_LINE		= $21
OP_RECT		= $22
OP_RECT_FILL	= $23
OP_PAL_SET	= $30
OP_PAL_LOAD	= $31
OP_BLIT		= $40
OP_BLIT_KEYED	= $41
OP_READ_RECT	= $42

; Fixed point 8.8 ($8x) and IEEE float32 ($9x) share a low nibble.
OP_MAT_MUL	= $80
OP_MAT_ADD	= $81
OP_MAT_SUB	= $82
OP_MAT_SCALE	= $83
OP_MAT_TRANSPOSE = $84
OP_MAT_IDENTITY	= $85
OP_MAT_INVERSE	= $86
OP_FLT_MUL	= $90
OP_FLT_ADD	= $91
OP_FLT_SUB	= $92
OP_FLT_SCALE	= $93
OP_FLT_TRANSPOSE = $94
OP_FLT_IDENTITY	= $95
OP_FLT_INVERSE	= $96

; --- error codes -------------------------------------------------------
ERR_OK		= $00
ERR_BAD_OPCODE	= $01
ERR_BAD_CLASS	= $02
ERR_OUT_OF_RANGE = $03
ERR_BAD_ARGS	= $04
ERR_SINGULAR	= $05
ERR_UNSUPPORTED	= $06
ERR_BUSY	= $07
ERR_OUT_OF_MEM	= $08
ERR_QUEUE_FULL	= $09
ERR_BAD_ID	= $0a
ERR_NO_CAMERA	= $0b

; --- harness fail codes ------------------------------------------------
F_CMP		= $e1
F_FILL		= $e2
F_PIXEL		= $e3
F_RANGE		= $e4
F_NOERR		= $fe

; --- framebuffer geometry, as the API reference states it --------------
FB_W		= 320
FB_H		= 200
FB_PAGES	= 2
BORDER_W	= 32
BORDER_H	= 36

SPACE_C64	= 0
SPACE_REU	= 1

SCREEN   = $0400

; Zero page. $f7-$fe is the tape/RS232 area, unused while a PRG runs.
P1       = $f7
P2       = $f9
SRC      = $fb
DST      = $fd

; ----------------------------------------------------------------------
; Argument staging.
;
; ARG registers are write-only and are NOT cleared between commands, so
; every opcode must stage every byte it reads, every time. These macros
; exist so that is obvious at the call site: an opcode's argument block
; reads top to bottom in the same order the reference table lists it.
; ----------------------------------------------------------------------

argb	.macro	off, val
	lda #\val
	sta ARG+\off
	.endm

argw	.macro	off, val
	lda #<(\val)
	sta ARG+\off
	lda #>(\val)
	sta ARG+\off+1
	.endm

; The 6-byte blob descriptor: space, 24-bit addr, 16-bit len. This is how
; every bulk payload crosses -- never byte-by-byte through the window.
argblob	.macro	off, space, addr, len
	lda #\space
	sta ARG+\off
	lda #<(\addr)
	sta ARG+\off+1
	lda #>(\addr)
	sta ARG+\off+2
	lda #0
	sta ARG+\off+3
	lda #<(\len)
	sta ARG+\off+4
	lda #>(\len)
	sta ARG+\off+5
	.endm

; The 4-byte compact descriptor the math opcodes use: no len, because the
; dimensions and the element size already imply it.
argcd	.macro	off, space, addr
	lda #\space
	sta ARG+\off
	lda #<(\addr)
	sta ARG+\off+1
	lda #>(\addr)
	sta ARG+\off+2
	lda #0
	sta ARG+\off+3
	.endm

; ----------------------------------------------------------------------
; Judging. Each of these leaves the fail code in X, ready for #check.
; ----------------------------------------------------------------------

; A = ERRCODE. Pass if OK; otherwise the fail code IS the errcode.
wantOk	.macro
	tax
	.endm

; A = ERRCODE. Pass only if it equals \err -- and a command that answered
; OK where an error was demanded fails as $FE, never silently as 0.
wantErr	.macro	err
	ldy #\err
	jsr kitExpect
	.endm

; X = fail code already set by hand.
raw	.macro
	.endm

; ----------------------------------------------------------------------
; Reporting. The name is stored inline after the JSR, so a test needs no
; string table and the name sits where the test is.
; ----------------------------------------------------------------------

check	.macro	name
	jsr kitCheck
	.text \name
	.byte 0
	.endm

title	.macro	name
	jsr kitTitle
	.text \name
	.byte 0
	.endm

; A bare note line in the results area -- for a value that is reported
; rather than judged, e.g. the measured frame period.
note	.macro	name
	jsr kitNote
	.text \name
	.byte 0
	.endm

; ----------------------------------------------------------------------
; Checking data.
;
; Most of what this suite asserts is not an error code but a byte that came
; back from the framebuffer or from a math result. A test opens with
; #clracc, makes as many #expectb assertions as it likes, and closes with
; #useacc -- so one result line can stand for a whole shape, and the fail
; code is the first byte that was actually wrong.
; ----------------------------------------------------------------------

clracc	.macro
	lda #0
	sta accFail
	.endm

useacc	.macro
	ldx accFail
	.endm

expectb	.macro	addr, val
	lda \addr
	cmp #\val
	beq +
	jsr kitBad
+
	.endm

; The same, for a value that must NOT be a particular byte -- used where
; the assertion is "this pixel was left alone", and the interesting failure
; is that something painted over it.
rejectb	.macro	addr, val
	lda \addr
	cmp #\val
	bne +
	jsr kitBad
+
	.endm

; A little-endian 16-bit field, which is how every multi-byte value in this
; API crosses -- info block entries, 8.8 matrix elements, coordinates.
expectw	.macro	addr, val
	lda \addr
	cmp #<(\val)
	beq +
	jsr kitBad
+
	lda \addr+1
	cmp #>(\val)
	beq +
	jsr kitBad
+
	.endm

; Length for the kitFill / kitSeq / kitCmp / kitChkFill helpers.
setlen	.macro	n
	lda #<(\n)
	sta kitLen
	lda #>(\n)
	sta kitLen+1
	.endm

setptr	.macro	p, addr
	lda #<(\addr)
	sta \p
	lda #>(\addr)
	sta \p+1
	.endm

; A slot that shows a 16-bit measurement rather than a verdict.
; After a failed #check on a kitCmp, this reports what actually differed:
; the offset of the first mismatching byte, and the two bytes themselves
; as "wantgot" -- expected (from P1) in the high nibble pair, actual (from
; P2) in the low. Two extra result lines that turn $E1 into a diagnosis.
cmpdiff	.macro	name, name2, name3
	#showw kitOff, \name
	#showw kitDiffN, \name3
	lda kitDiffB
	sta kitVal16
	lda kitDiffA
	sta kitVal16+1
	jsr kitShow
	.text \name2
	.byte 0
	.endm

; The second and third mismatches from the same kitCmp: their offsets, and
; their two actual bytes packed into one word (2nd high, 3rd low). Together
; with #cmpdiff this is three offsets and three got-values from a single
; run, which is what it takes to tell scattered damage from one bad byte.
cmpmore	.macro	name2, name3, namegot
	#showw kitOff2, \name2
	#showw kitOff3, \name3
	lda kitGot3
	sta kitVal16
	lda kitGot2
	sta kitVal16+1
	jsr kitShow
	.text \namegot
	.byte 0
	.endm

; The offsets alone, for a pass whose want/got values are already known from
; another pass. Four slots instead of six -- the screen has exactly 21 rows
; per column and three transfer passes do not otherwise fit.
cmpwhere .macro	nameat, namen, name2, name3
	#showw kitOff, \nameat
	#showw kitDiffN, \namen
	#showw kitOff2, \name2
	#showw kitOff3, \name3
	.endm

showw	.macro	addr, name
	lda \addr
	sta kitVal16
	lda \addr+1
	sta kitVal16+1
	jsr kitShow
	.text \name
	.byte 0
	.endm

; ----------------------------------------------------------------------
; Staging the drawing opcodes.
;
; These only lay out ARG bytes -- they never dispatch. Keeping the two
; apart is deliberate: a test that stages a shape and then fires the wrong
; opcode is a bug the suite must be able to express, because that is what a
; real program's bug looks like.
; ----------------------------------------------------------------------

stgPixel .macro	x, y, c
	#argw 0, \x
	#argw 2, \y
	#argb 4, \c
	.endm

stgLine	.macro	x0, y0, x1, y1, c
	#argw 0, \x0
	#argw 2, \y0
	#argw 4, \x1
	#argw 6, \y1
	#argb 8, \c
	.endm

; Serves RECT and RECT_FILL, which take the same nine bytes.
stgRect	.macro	x, y, w, h, c
	#argw 0, \x
	#argw 2, \y
	#argw 4, \w
	#argw 6, \h
	#argb 8, \c
	.endm

; BLIT and BLIT_KEYED; the key byte is ARG14 and is staged separately, so
; a keyed blit reads as "the same blit, plus a key".
stgBlit	.macro	src, x, y, w, h
	#argblob 0, SPACE_C64, \src, (\w)*(\h)
	#argw 6, \x
	#argw 8, \y
	#argw 10, \w
	#argw 12, \h
	.endm

stgRead	.macro	dst, x, y, w, h
	#argblob 0, SPACE_C64, \dst, (\w)*(\h)
	#argw 6, \x
	#argw 8, \y
	#argw 10, \w
	#argw 12, \h
	.endm

; Fill a readback buffer with a byte no test ever expects, so a READ_RECT
; that quietly wrote nothing cannot pass on leftovers. $AA is chosen for
; being outside every colour index the suite draws with.
; Fills a destination with a byte that is not a plausible result, so a
; command that wrote nothing at all cannot be mistaken for one that wrote
; the right thing. When two buffers are filled and then compared against
; each other, they MUST be given different bytes -- otherwise "nothing
; happened" compares equal and the check passes vacuously.
poison	.macro	addr, len, val=$aa
	#setptr P1, \addr
	#setlen \len
	lda #\val
	sta kitVal
	jsr kitFill
	.endm

; Fold a dispatch's ERRCODE into the accumulated fail code, for a test that
; fires several commands and judges them as one line. Must follow a cmd0 /
; cmd1 directly, whose Z flag still reflects the ERRCODE it returned.
accOk	.macro
	beq +
	jsr kitBad
+
	.endm

; A little-endian 32-bit field -- an IEEE 754 single, for the $9x math set.
expectd	.macro	addr, val
	lda \addr
	cmp #<(\val)
	beq +
	jsr kitBad
+
	lda \addr+1
	cmp #>(\val)
	beq +
	jsr kitBad
+
	lda \addr+2
	cmp #`(\val)
	beq +
	jsr kitBad
+
	lda \addr+3
	cmp #((\val) >> 24) & $ff
	beq +
	jsr kitBad
+
	.endm
