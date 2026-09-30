; ======================================================================
; gpu64_game_controls.inc -- the controls menu and the key remap.
; Included by gpu64_demo_game.a after the logo, under BASIC ($b1xx up),
; which is banked out from `title` to `finish` -- the only span that
; reaches anything here.
;
; EIGHT ACTIONS, ONE REMAP
;
; The game's movement code reads fixed bits: W in keyB, D in keyC, Q and
; SPACE in keyD, and so on. Rather than teach every reader about bindings,
; kbRemap runs inside readKeys after the matrix is read and rewrites
; exactly those eight bits: each one is set (released), then cleared again
; if the key bound to its action is down. With the default bindings that is
; the identity, and everything downstream -- swimming, the water jump, the
; dead player's released keys, the joystick that replaces the keyboard --
; is unchanged. A key is its matrix position, column * 8 + row, the same
; numbering tools/quake_pics.py names its KEY pictures by.
;
; Only the eight movement actions are bindable. The keys the game already
; uses for something else are reserved and the menu ignores them: RETURN,
; F1-F7, 1-4 (weapons), B and C= (fire), <- (pause), H (health cheat),
; + and - (view), and RUN/STOP. Binding a key another action already has swaps the two, so
; no key ever drives two actions and no action is ever left without a key.
; Bindings last for the session: they live in the program, so the second
; level and every restart keep them, and a reload of the PRG resets them.
;
; THE MENU
;
; The skill menu's fifth line. Quake's M_Keys_Draw on the HDMI screen --
; ttl_cstm at the top, the hint at 12,32, the actions at 16 and their keys
; at 140 from y 48, eight apart, the cursor at 130 blinking between
; console characters 12 and 13 and '=' while it waits for a key -- and the
; same list as text on the C64. W/S or the joystick move, fire, SPACE or
; RETURN picks. Picking an action waits for every key to be released and
; then for one key held alone for two ticks in a row; RETURN or the
; joystick's fire cancels. The wait is a tick loop like the rest of the
; menu, so the HDMI screen keeps being redrawn whole and a command the bus
; lost is healed on the next tick.
; ======================================================================

CT_N      = 8			; bindable actions
CT_ITEMS  = CT_N + 2		; + defaults, done
CT_LBL_X  = 16			; M_Keys_Draw's columns
CT_KEY_X  = 140
CT_CUR_X  = 130
CT_KEYCOL = 22			; the C64 copy's key column
CT_NONE   = $fe			; ctScanAll: nothing is down
CT_MANY   = $ff			; ... several keys, or the stick is off centre
CT_CANCEL = $fd			; ... the joystick's fire
KEY_RETURN = 1			; column 0, row 1

; ----------------------------------------------------------------------
; kbRemap: readKeys, straight after the matrix is read.
; ----------------------------------------------------------------------
kbRemap
	lda keyA
	sta kbRaw+0
	lda keyB
	sta kbRaw+1
	lda keyC
	sta kbRaw+2
	lda keyF
	sta kbRaw+3
	lda keyG
	sta kbRaw+4
	lda keyE
	sta kbRaw+5
	lda keyH
	sta kbRaw+6
	lda keyD
	sta kbRaw+7

	; kbUp bit x = action x's key reads released.
	ldx #CT_N - 1
-	lda bindKey,x
	jsr kbCell
	and kbRaw,y
	cmp #1				; C clear: the bit is 0, i.e. down
	rol kbUp
	dex
	bpl -

	lda keyB
	ora #KB_W | KB_S | KB_A | KB_E | KB_LSH
	sta keyB
	lda keyC
	ora #KC_D
	sta keyC
	lda keyD
	ora #KD_Q | KD_SPC
	sta keyD
	ldx #0
-	lsr kbUp
	bcs +
	ldy ctDefOff,x
	lda keyB,y			; keyB, keyC, keyD sit together
	and ctDefClr,x
	sta keyB,y
+	inx
	cpx #CT_N
	bne -
	rts

; A = key. Y = its column, A = its row's bit. X is kept.
kbCell
	pha
	lsr a
	lsr a
	lsr a
	tay
	pla
	and #7
	sty kbT
	tay
	lda kbBits,y
	ldy kbT
	rts

; ----------------------------------------------------------------------
; The menu. The skill menu calls it and redraws itself afterwards.
; ----------------------------------------------------------------------
ctMenu
	lda #0
	sta ctCur
	sta ctGrab
	sta tiTick
	jsr tiMark
	jsr ctC64Static
ctTick
	jsr ctC64Lines
	jsr ctHdmi
	inc tiTick
	lda ctGrab
	bne ctGrabTick

	jsr tiTickWait
	lda tiNew
	lsr a
	bcc +
	dec ctCur			; up
	bpl +
	lda #CT_ITEMS - 1
	sta ctCur
+	lda tiNew
	and #2
	beq +
	inc ctCur			; down
	lda ctCur
	cmp #CT_ITEMS
	bcc +
	lda #0
	sta ctCur
+	lda tiNew
	and #4
	beq ctTick
	lda ctCur
	cmp #CT_N
	bcc ctPick
	beq ctDefaults
	rts				; done

ctPick
	lda #1				; wait for everything to be released
	sta ctGrab
	lda #CT_MANY
	sta ctCand
	bne ctTick

ctDefaults
	ldx #CT_N - 1
-	lda ctDefault,x
	sta bindKey,x
	dex
	bpl -
	bmi ctTick

ctGrabTick
	lda #TICK_MS
	jsr tiWait
	jsr ctScanAll
	ldx ctGrab
	cpx #1
	bne ctArmed
	cmp #CT_NONE
	bne ctTick
	inc ctGrab			; all released: now take a key
	bne ctTick
ctArmed
	cmp #CT_CANCEL
	beq ctGrabEnd
	cmp #KEY_RETURN
	beq ctGrabEnd
	cmp #CT_NONE
	bcs ctNoCand			; nothing, or nothing usable
	jsr kbCell
	and ctResv,y
	bne ctNoCand			; the game uses that key already
	lda ctFound
	cmp ctCand
	beq ctBind			; the same key two ticks running
	sta ctCand
	jmp ctTick
ctNoCand
	lda #CT_MANY
	sta ctCand
	jmp ctTick

ctBind
	; Whoever has this key gets the picked action's old one.
	ldy ctCur
	ldx #CT_N - 1
-	cmp bindKey,x
	bne +
	lda bindKey,y
	sta bindKey,x
	lda ctCand
+	dex
	bpl -
	sta bindKey,y
ctGrabEnd
	lda #0
	sta ctGrab
	lda #7				; whatever is still held is not a new press
	sta tiIn
	jmp ctTick

; The whole matrix, each column read twice. A = the one key down
; (column * 8 + row, also in ctFound), CT_NONE, CT_MANY, or CT_CANCEL. The
; stick off centre ghosts into the columns, so then the keyboard is not
; read at all; port 1's row pulls are masked out, as in tiScan.
ctScanAll
	sei
	lda #$ff
	sta $dc02
	lda #0
	sta $dc03
	lda #$ff
	sta CIA_PRA
	lda CIA_PRB
	eor #$ff
	sta tiP1
	lda $dc00
	eor #$ff
	and #$1f
	beq ctsKb
	and #$10
	beq +
	lda #CT_CANCEL
	bne ctsOut
+	lda #CT_MANY
	bne ctsOut
ctsKb
	lda #CT_NONE
	sta ctFound
	ldx #7
ctsCol
	lda ctColSel,x
	sta CIA_PRA
	lda CIA_PRB
	ora CIA_PRB
	ora tiP1
	eor #$ff			; 1 = down
	beq ctsNext
	ldy ctFound
	cpy #CT_NONE
	bne ctsMany			; a second column
	sta kbT
	tay
	dey
	tya
	and kbT
	bne ctsMany			; a second row
	txa
	asl a
	asl a
	asl a
	sta ctFound
	lda kbT
-	lsr a
	bcs ctsNext
	inc ctFound
	bne -
ctsNext
	dex
	bpl ctsCol
	lda ctFound
	jmp ctsOut
ctsMany
	lda #CT_MANY
ctsOut
	sta ctFound
	lda #$ff
	sta CIA_PRA
	cli
	lda ctFound
	rts

; ----------------------------------------------------------------------
; The HDMI screen, whole, then a flip.
; ----------------------------------------------------------------------
ctHdmi
	lda #PIC_CONFADE
	ldx #0
	ldy #0
	jsr tiPicAt
	lda #0
	jsr tiPic
	lda #PIC_TTL_CSTM
	ldx #TTL_CSTM_X
	ldy #4
	jsr tiPicAt
	lda #1
	jsr tiPic
	lda #PIC_CT_HINT0
	ldx ctGrab
	beq +
	lda #PIC_CT_HINT1
+	ldx #12
	ldy #32
	jsr tiPicAt
	lda #1
	jsr tiPic

	ldx #0
chLoop
	stx ctI
	txa
	clc
	adc #PIC_CT_ACT0		; ACT0-7, DEFAULTS, DONE are consecutive
	ldy ctYTab,x
	ldx #CT_LBL_X
	jsr tiPicAt
	lda #1
	jsr tiPic
	ldx ctI
	cpx #CT_N
	bcs +
	lda bindKey,x
	clc
	adc #PIC_KEY0
	ldy ctYTab,x
	ldx #CT_KEY_X
	jsr tiPicAt
	lda #1
	jsr tiPic
+	ldx ctI
	inx
	cpx #CT_ITEMS
	bne chLoop

	lda #PIC_CT_GRAB
	ldx ctGrab
	bne +
	lda tiTick			; M_Keys_Draw blinks at 4 Hz; this is 5
	lsr a
	and #1
	clc
	adc #PIC_CT_CUR0
+	ldx ctCur
	ldy ctYTab,x
	ldx #CT_CUR_X
	jsr tiPicAt
	lda #1
	jsr tiPic
	jmp dmFlip

; ----------------------------------------------------------------------
; The C64 copy.
; ----------------------------------------------------------------------
ctC64Static
	jsr tiCls
	lda #1
	jsr dmRow
	jsr dmText
	.text "           CUSTOMIZE CONTROLS"
	.byte 0
	lda #21
	jsr dmRow
	jsr dmText
	.text "   W/S OR JOYSTICK, FIRE TO PICK"
	.byte 0
	ldx #0
-	stx ctI
	ldy ctRowTab,x
	jsr ctRowAt
	lda #<ctLblStr
	sta SRC
	lda #>ctLblStr
	sta SRC+1
	ldx ctI
	lda #3
	jsr ctNth
	ldx ctI
	inx
	cpx #CT_ITEMS
	bne -
	rts

ctC64Lines
	; The hint, row 3.
	ldy #3
	jsr ctRowAt
	lda #$20
	ldy #39
-	sta (DST),y
	dey
	bpl -
	lda #<ctHintStr
	sta SRC
	lda #>ctHintStr
	sta SRC+1
	ldx ctGrab
	beq +
	ldx #1
+	lda #5
	jsr ctNth

	ldx #0
cclLoop
	stx ctI
	; The line's colour: yellow on the cursor, grey elsewhere.
	ldy ctRowTab,x
	lda tiColLo,y
	sta SRC
	lda tiColHi,y
	sta SRC+1
	lda #LTGREY
	cpx ctCur
	bne +
	lda #YELLOW
+	ldy #CT_KEYCOL + 9
-	sta (SRC),y
	dey
	bpl -
	; The cursor and the key, blank first.
	ldy ctRowTab,x
	jsr ctRowAt
	lda #$20
	ldy #1
	sta (DST),y
	ldy #CT_KEYCOL
-	sta (DST),y
	iny
	cpy #CT_KEYCOL + 9
	bne -
	cpx ctCur
	bne cclNoCur
	lda #$3e			; '>'
	ldy ctGrab
	beq +
	lda #$3d			; '='
+	ldy #1
	sta (DST),y
cclNoCur
	cpx #CT_N
	bcs cclNext
	lda #<ctKeyStr
	sta SRC
	lda #>ctKeyStr
	sta SRC+1
	lda bindKey,x
	tax
	lda #CT_KEYCOL
	jsr ctNth
cclNext
	ldx ctI
	inx
	cpx #CT_ITEMS
	bne cclLoop
	rts

; Y = row: DST at its start.
ctRowAt
	lda tiScrLo,y
	sta DST
	lda tiScrHi,y
	sta DST+1
	rts

; SRC = a table of 0-terminated strings, each under 256 bytes in all;
; X = which; A = column. Prints string X at DST + A.
ctNth
	sta ctCol
	ldy #0
ctnSkip
	dex
	bmi ctnPut
-	lda (SRC),y
	iny
	cmp #0
	bne -
	beq ctnSkip
ctnPut
	lda (SRC),y
	beq ctnDone
	cmp #$40			; '@' is screen code 0
	bne +
	lda #0
	beq ++
+	jsr dmScreenCode
+	sty kbT
	ldy ctCol
	sta (DST),y
	inc ctCol
	ldy kbT
	iny
	bne ctnPut
ctnDone
	rts

; ----------------------------------------------------------------------
; Tables.
; ----------------------------------------------------------------------

; The default keys, which are also the bits kbRemap writes: W S A D Q E
; LEFT SHIFT SPACE, in the menu's order.
ctDefault	.byte 9, 13, 10, 18, 62, 14, 15, 60
ctDefOff	.byte 0, 0, 0, keyC - keyB, keyD - keyB, 0, 0, keyD - keyB
ctDefClr	.byte $ff ^ KB_W, $ff ^ KB_S, $ff ^ KB_A, $ff ^ KC_D
		.byte $ff ^ KD_Q, $ff ^ KB_E, $ff ^ KB_LSH, $ff ^ KD_SPC
bindKey		.byte 9, 13, 10, 18, 62, 14, 15, 60

; Per column, the rows whose keys the game already uses.
ctResv		.byte $7a		; RETURN F7 F1 F3 F5
		.byte $09		; 3 4
		.byte $00
		.byte $30		; B H
		.byte $00
		.byte $09		; + -
		.byte $00
		.byte $ab		; 1 <- 2 C= RUN/STOP

kbBits		.byte $01, $02, $04, $08, $10, $20, $40, $80
ctColSel	.byte $fe, $fd, $fb, $f7, $ef, $df, $bf, $7f
ctYTab		.byte 48, 56, 64, 72, 80, 88, 96, 104, 120, 128
ctRowTab	.byte 5, 6, 7, 8, 9, 10, 11, 12, 14, 15

ctLblStr	.text "WALK FORWARD", 0, "BACKPEDAL", 0, "TURN LEFT", 0
		.text "TURN RIGHT", 0, "STEP LEFT", 0, "STEP RIGHT", 0
		.text "RUN", 0, "JUMP / SWIM UP", 0, "DEFAULTS", 0, "DONE", 0
ctHintStr	.text "RETURN OR FIRE TO CHANGE", 0
		.text "PRESS A KEY, RETURN CANCELS", 0
; tools/quake_pics.py's KEY_NAMES, the same order.
ctKeyStr	.text "DEL", 0, "RETURN", 0, "CRSR RT", 0, "F7", 0
		.text "F1", 0, "F3", 0, "F5", 0, "CRSR DN", 0
		.text "3", 0, "W", 0, "A", 0, "4", 0, "Z", 0, "S", 0, "E", 0, "L SHIFT", 0
		.text "5", 0, "R", 0, "D", 0, "6", 0, "C", 0, "F", 0, "T", 0, "X", 0
		.text "7", 0, "Y", 0, "G", 0, "8", 0, "B", 0, "H", 0, "U", 0, "V", 0
		.text "9", 0, "I", 0, "J", 0, "0", 0, "M", 0, "K", 0, "O", 0, "N", 0
		.text "+", 0, "P", 0, "L", 0, "-", 0, ".", 0, ":", 0, "@", 0, ",", 0
		.text "POUND", 0, "*", 0, ";", 0, "HOME", 0, "R SHIFT", 0, "=", 0
		.text "UP ARROW", 0, "/", 0
		.text "1", 0, "LT ARROW", 0, "CTRL", 0, "2", 0, "SPACE", 0, "C=", 0
		.text "Q", 0, "RUN STOP", 0
	.cerror ctHintStr - ctLblStr > 256 || * - ctKeyStr > 256, "a string table outgrew ctNth"

kbRaw		.fill 8, 0
kbUp		.byte 0
kbT		.byte 0
ctCur		.byte 0
ctGrab		.byte 0		; 0 menu, 1 waiting for release, 2 taking a key
ctCand		.byte 0
ctFound		.byte 0
ctI		.byte 0
ctCol		.byte 0
