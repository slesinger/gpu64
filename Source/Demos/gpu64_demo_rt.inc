;
;       {_______            {_          {______
;             {__          {_ __               {__
;             {__         {_  {__               {__
;          {__           {__   {__               {__
;      {______          {__     {__              {__
;            {__       {__       {__            {__
;              {_________         {______________		Expansion Unit
;
;      gpu64 -- small runtime shared by the demonstration programs.
;
;      Included once, at the END of a demo (it contains code and data, not
;      macros). It holds the housekeeping every demo needs and none of the
;      gpu64 API: text on the C64's own screen, a hex printer, the STOP key,
;      and the two vblank helpers whose only job is to keep working on a
;      display gpu64 could not measure a frame period from.
;
;      THE ONE THING WORTH READING HERE
;
;      dmVbWait / dmFlip are written so that the same demo runs on a display
;      with a frame clock and on one without. gpu64 reports the measured
;      frame period in the info block, and 0 means every vblank feature will
;      answer UNSUPPORTED. dmInit reads that once into dmVbOk; the two
;      helpers then either sync to the display or free-run. A real program
;      wants the same fallback -- an animation that refuses to run at all
;      because it cannot be tear-free is worse than one that tears.
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; Start-up: clear the C64 screen, put gpu64 in a known state, and find out
; whether this display has a usable frame clock.
; ----------------------------------------------------------------------
dmInit
	ldx #0
	lda #$20
dmiCls
	sta SCREEN,x
	sta SCREEN+$100,x
	sta SCREEN+$200,x
	sta SCREEN+$300,x
	inx
	bne dmiCls

	; CMD_HI is sticky and survives RUN/STOP, so a previous program can
	; have left it pointing at another class. Class 0 is the reset value
	; and everything in these demos lives there.
	lda #0
	sta CMD_HI

	; GET_INFO does not answer through a register: it DMA-writes the
	; 16-byte info block into memory you nominate. Bytes 14-15 are the
	; measured frame period in microseconds, and 0 means "no frame clock".
	#argblob 0, SPACE_C64, dmInfo, 16
	#cmd OP_GET_INFO

	lda #0
	sta dmVbOk
	lda dmInfo+14
	ora dmInfo+15
	beq dmiNoVb
	lda #1
	sta dmVbOk
dmiNoVb
	rts

; ----------------------------------------------------------------------
; Frame pacing.
;
; The polling form of the vblank signal: STATUS bit2 sets once per display
; frame and stays set until VBLANK_ACK clears it. No IRQ handler needed --
; this is usable from BASIC.
; ----------------------------------------------------------------------
dmVbWait
	lda dmVbOk
	beq dmvFree			; no frame clock: free-run instead
dmvPoll
	lda STATUS
	and #ST_VB_PEND
	beq dmvPoll
	#cmd OP_VBLANK_ACK
	rts
dmvFree
	; Nothing to sync to. Burn a little time so the animation is not
	; running at the CPU's own speed, and carry on.
	ldx #0
dmvDelay
	dex
	bne dmvDelay
	rts

; Make the drawn page visible. With a frame clock this is a deferred flip,
; which lands on a real frame boundary and cannot tear; without one it is
; an immediate flip, which can. Either way the wait on STATUS bit0 is a
; register spin, not a halt: the C64 is free to do something else instead.
dmFlip
	lda dmVbOk
	beq dmfNow
	#argb 0, 1			; 1 = at next vblank
	#cmd OP_PAGE_FLIP
dmfWait
	lda STATUS
	and #ST_BUSY
	bne dmfWait
	rts
dmfNow
	#argb 0, 0			; 0 = immediately
	#cmd OP_PAGE_FLIP
	rts

; ----------------------------------------------------------------------
; The C64's own screen. Demos report on it, so a bench tester can read the
; result without also reading the HDMI log.
; ----------------------------------------------------------------------

; A = row (0-24). Sets the cursor (DST) to the start of it.
;
; A table, not a multiply: 25 rows is small, and the bounds check below is
; here because a row number past the end of a table like this one does not
; fail loudly -- it writes the text into the program instead, and the demo
; then looks like it never ran.
dmRow
	cmp #25
	bcc +
	lda #24
+
	tax
	lda dmRowLo,x
	sta DST
	lda dmRowHi,x
	sta DST+1
	rts

dmRowLo
	.for r = 0, r < 25, r = r + 1
	.byte <(SCREEN + r * 40)
	.next
dmRowHi
	.for r = 0, r < 25, r = r + 1
	.byte >(SCREEN + r * 40)
	.next

; Print the string that follows the JSR, at the cursor. The text sits where
; it is used, so a demo needs no string table.
dmText
	pla
	sta SRC
	pla
	sta SRC+1
	inc SRC
	bne +
	inc SRC+1
+
	ldy #0
dmtLoop
	lda (SRC),y
	beq dmtEnd
	jsr dmScreenCode
	sta (DST),y
	iny
	bne dmtLoop
dmtEnd
	tya
	clc
	adc DST
	sta DST
	bcc +
	inc DST+1
+
	tya
	sec				; +1 for the terminator
	adc SRC
	sta SRC
	bcc +
	inc SRC+1
+
	jmp (SRC)

; A = byte, printed as two hex digits at the cursor. Clobbers Y -- read
; every byte you are about to print BEFORE you print the first one.
dmHexA
	pha
	lsr a
	lsr a
	lsr a
	lsr a
	jsr dmHexDigit
	ldy #0
	sta (DST),y
	pla
	and #$0f
	jsr dmHexDigit
	ldy #1
	sta (DST),y
	clc
	lda DST
	adc #2
	sta DST
	bcc +
	inc DST+1
+
	rts

dmHexDigit
	cmp #10
	bcc dmhDec
	sec
	sbc #9				; 'A'..'F' are screen codes $01..$06
	rts
dmhDec
	clc
	adc #$30			; '0'..'9' are $30..$39
	rts

; PETSCII upper case to screen code; digits, space and punctuation pass
; through unchanged, which covers everything these demos print.
dmScreenCode
	cmp #$41
	bcc dmsDone
	cmp #$5b
	bcs dmsDone
	sec
	sbc #$40
dmsDone
	rts

; ERRCODE at the cursor, as "OK" or as the code itself. Demos print this
; once at the end: every dispatch writes ERRCODE, so if the last one says
; OK and something looked wrong on the HDMI screen, the argument block was
; wrong rather than the command.
dmErr
	lda ERRCODE
	beq dmeOk
	pha
	ldy #0
	lda #$05			; 'E'
	sta (DST),y
	iny
	lda #$12			; 'R'
	sta (DST),y
	clc
	lda DST
	adc #2
	sta DST
	bcc +
	inc DST+1
+
	pla
	jmp dmHexA
dmeOk
	ldy #0
	lda #$0f			; 'O'
	sta (DST),y
	iny
	lda #$0b			; 'K'
	sta (DST),y
	rts

; ----------------------------------------------------------------------
; Ending.
; ----------------------------------------------------------------------

; Z set when the STOP key is down. Animation loops end on this.
dmStop
	jmp $ffe1

; Hold the C64 screen until STOP, then clear it -- returning straight to
; BASIC would print READY. over the last line.
dmHold
	jsr $ffe1
	bne dmHold
	jsr $e544			; KERNAL clear screen
	lda #0
	sta CMD_HI
	rts

; A cheap 8-bit LFSR. Demos that want scattered values use it so that two
; runs look the same, which matters when a bench tester is comparing a
; photograph against what the host renderer produced.
dmRand
	lda dmSeed
	beq dmrZero
	asl a
	beq dmrNoEor
	bcc dmrNoEor
dmrEor
	eor #$1d
dmrNoEor
	sta dmSeed
	rts
dmrZero
	lda #$1d
	sta dmSeed
	rts

; ----------------------------------------------------------------------
; Runtime data.
; ----------------------------------------------------------------------
dmVbOk	.byte 0				; 1 = this display has a frame clock
dmSeed	.byte $a5
dmInfo	.fill 16, 0			; the info block GET_INFO wrote
