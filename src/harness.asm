; The passive 586220 harness. Its distinctive mirrored
; user-port connections were sensed before entering here. A '+' or '!'
; on the instrument strip is the result for that particular electrical path.
accessories:
    ldx #39
-   lda harness_strip,x
    sta screen+19*40,x
    dex
    bpl -
    lda #$37            ; motor off: keyboard dongle connects PA to PB
    sta $01
    ldx #7
    jsr matrix_loop
    ldx #6
    jsr jack_result
    lda #$17            ; motor on: switch closes across joystick sockets
    sta $01
    ldx #4
    jsr matrix_loop
    ldx #13
    jsr jack_result
    lda #$37
    sta $01
    ; U28 selects two separate RC pairs. Give the SID more than two
    ; 512-cycle ADC intervals to settle after each change of input.
    lda #$c0
    sta cia1+2
    lda #0
    sta delta
    ldy #2
-   lda pot_select,y
    sta cia1
    ldx #0
-   dex
    bne -
    ldx #1
-   lda sid+25,x
    cpy #0
    beq pot_unplugged
    cmp #45
    bcc pot_bad
    cmp #195
    bcc pot_next
    bcs pot_bad
pot_unplugged:
    cmp #240
    bcs pot_next
pot_bad:
    inc delta
pot_next:
    dex
    bpl -
    dey
    bpl ---
    lda #0
    sta cia1+2
    lda delta
    ldx #20
    jsr jack_result
    ; The serial plug loops ATN to CLK. The 7406 output is open
    ; collector. Require the clock input to follow BOTH transitions.
    lda #3
    sta cia2
    lda #$3b            ; PA4/5 low release CLK/DATA via the 7406
    sta cia2+2
    nop
    lda cia2
    and #$40
    sta tmp
    lda #$0b
    sta cia2
    nop
    lda cia2
    eor tmp
    and #$40
    eor #$40
    sta delta
    lda #3
    sta cia2
    nop
    lda cia2
    eor tmp
    and #$40
    ora delta
    ldx #27
    jsr jack_result
    ; Keep DATA released: the serial dongle links it to SRQ (/FLAG1).
    ; Cassette SENSE (6510 P4) to READ (CIA1 /FLAG). Release the
    ; line high, clear stale events, pull it low, require a new edge.
    lda #$27            ; P4 latch low, MOTOR off
    sta $01
    lda cia1+13
    lda #$3f            ; P4 becomes output
    sta $00
    nop
    nop
    lda cia1+13
    and #$10
    eor #$10
    sta delta
    lda cia1+13
    and #$10
    ora delta
    ldx #35
    jsr jack_result
    lda #$2f
    sta $00
    lda #$37
    sta $01
    rts

; Ground one output at a time, with the receiving port entirely inputs.
; Each matching line must go low and all the other lines must stay high.
matrix_loop:
    lda #0
    sta cia1+2
    sta cia1+3
    sta cia1
    sta cia1+1
    sta delta
-   lda bits,x
    sta cia1+2
    nop
    eor cia1+1
    eor #$ff
    ora delta
    sta delta
    lda #0
    sta cia1+2
    lda bits,x
    sta cia1+3
    nop
    eor cia1
    eor #$ff
    ora delta
    sta delta
    lda #0
    sta cia1+3
    dex
    bpl -
    lda delta
    rts
jack_result:
    cmp #0
    beq +
    inc faults
    lda #33              ; !
    ldy #10
    bne jack_write
+   lda #43              ; +
    ldy #13
jack_write:
    sta screen+19*40,x
    tya
    sta color+19*40,x
    rts
pot_select: .byte 0,$40,$80
harness_strip: .text "  KEY .  JOY .  POT .  IEC .  TAPE .    "
