;
;       {_______            {_          {______
;             {__          {_ __               {__
;             {__         {_  {__               {__
;          {__           {__   {__               {__
;      {______          {__     {__              {__
;            {__       {__       {__            {__
;              {_________         {______________		Expansion Unit
;
;      gpu64 -- equates and macros shared by the demonstration programs.
;
;      This file is not a program. It is the part of a gpu64 program that is
;      the same in every gpu64 program: the register window, the opcode
;      numbers, the error codes, and four macros for staging an argument
;      block. Copy it into your own project and you can start at the first
;      interesting line.
;
;      Everything here comes straight from docs/api_design.md. Nothing in it
;      is cleverer than that document -- if the two ever disagree, the
;      document is right.
;
;      THE WHOLE PROTOCOL, IN FOUR LINES
;
;              lda #0
;              sta CMD_HI      ; class 0 -- sticky, so once per program
;              #argb 0, 6      ; stage arguments, any order
;              #cmd OP_CLEAR   ; the write to CMD_LO fires the command
;
;      The write to CMD_LO halts the C64 until the command has finished, so
;      there is nothing to poll and nothing to wait for. Read ERRCODE after
;      it if you want to know how it went; every dispatch writes it, so a
;      stale value can never look like a fresh failure.
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; The register window. gpu64 owns $DF0B-$DFFF in IO2; the REU keeps
; $DF00-$DF0A and IO1 ($DE00-$DEFF) is untouched.
; ----------------------------------------------------------------------

CMD_HI   = $df0b		; class selector, sticky
CMD_LO   = $df0c		; opcode -- WRITING THIS FIRES THE COMMAND
STATUS   = $df0d		; bit0 busy, bit1 error, bit2 vblank, bit3 armed
ERRCODE  = $df0e		; result of the last dispatch
ID_LO    = $df0f
ID_HI    = $df10
ARG      = $df11		; ARG0..ARG15 = $df11..$df20

ST_BUSY		= $01
ST_ERROR	= $02
ST_VB_PEND	= $04
ST_VB_ARMED	= $08

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

; Fixed point 8.8 ($8x) and IEEE float32 ($9x) are the same operations on
; different element formats, so they share a low nibble.
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

; --- geometry and spaces -----------------------------------------------
FB_W		= 320
FB_H		= 200
SPACE_C64	= 0
SPACE_REU	= 1

; --- C64 colour numbers, which are also gpu64's reset palette ----------
BLACK = 0
WHITE = 1
RED = 2
CYAN = 3
PURPLE = 4
GREEN = 5
BLUE = 6
YELLOW = 7
ORANGE = 8
BROWN = 9
LTRED = 10
DKGREY = 11
GREY = 12
LTGREEN = 13
LTBLUE = 14
LTGREY = 15

SCREEN   = $0400

; Zero page. $f7-$fe is the tape/RS232 area, free while a PRG runs.
SRC      = $fb
DST      = $fd

; ----------------------------------------------------------------------
; Argument staging.
;
; ARG registers are write-only and are NOT cleared between commands. Every
; opcode reads exactly the byte count its table row lists and never looks
; past it -- so leftovers cannot leak in, but you must stage every byte the
; opcode reads, every time. These macros make an argument block read top to
; bottom in the same order the reference lists it.
; ----------------------------------------------------------------------

; One byte of argument.
argb	.macro	off, val
	lda #\val
	sta ARG+\off
	.endm

; A 16-bit argument, low byte first as a 6502 expects. Coordinates are
; signed; w/h are unsigned.
argw	.macro	off, val
	lda #<(\val)
	sta ARG+\off
	lda #>(\val)
	sta ARG+\off+1
	.endm

; The 6-byte blob descriptor -- space, 24-bit address, 16-bit length. Bulk
; data never crosses the register window byte by byte: you say where the
; bytes already live and gpu64 fetches them itself. The same descriptor is
; also a destination, for the commands that hand data back.
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

; The 4-byte compact descriptor the math opcodes use: no length, because
; the dimensions and the element size already imply it.
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

; Fire the command. This is the write that halts the C64 and does the work.
cmd	.macro	op
	lda #\op
	sta CMD_LO
	.endm

; A 16-bit value staged from a variable rather than a constant.
argwv	.macro	off, var
	lda \var
	sta ARG+\off
	lda \var+1
	sta ARG+\off+1
	.endm

; ----------------------------------------------------------------------
; The BASIC stub: "10 SYS 2064", so RUN launches the code that follows.
; $0810 is the entry point every gpu64 demo and test uses, which is also
; where the host simulator (tools/prgsim) starts a program.
;
; Assemble WITHOUT --nostart. That option strips the 2-byte load address
; and produces a PRG that loads to the wrong place and silently never runs.
; ----------------------------------------------------------------------
basicStub	.macro
	* = $0801
	.byte $0b, $08, $0a, $00, $9e
	.text "2064"
	.byte $00, $00, $00
	* = $0810
	.endm
