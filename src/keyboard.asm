; Modal keyboard diagnostic. Backups live in deep-test RAM; returning starts
; a new pass before those pages are verified. Sticky fault results survive.
kb_row = $33
kb_col = $34
kb_bits = $35
kb_exit = $36
kb_clock = $37
kb_seen = $38
kb_live = $40
kb_ptr = $48
kb_paint = $4a
kb_bit = $4b

keyboard_entry:
    lda #$10
    sta cia1+2
    nop
    lda cia1+1
    pha
    lda #0
    sta cia1+2
    pla
    and #$20
    beq kb_open
    rts
kb_open:
    ; Save dashboard, colors and sprite-enable state.
    lda vic+$15
    sta $1800
    lda #0
    sta vic+$15
    sta sid+24          ; silence the paused score without changing S setting
    ldx #0
kb_save:
    .for k in range(4)
    lda screen+256*k,x
    sta $1000+256*k,x
    lda color+256*k,x
    sta $1400+256*k,x
    lda #32
    sta screen+256*k,x
    lda #11
    sta color+256*k,x
    .endfor
    inx
    bne kb_save
    ldx #7
    lda #0
-   sta kb_seen,x
    sta kb_live,x
    dex
    bpl -
    lda #$85
    sta mode
    ldx #0
-   lda kb_header,x
    sta screen,x
    inx
    cpx #kb_header_end-kb_header
    bne -
    ldx #0
-   lda kb_labels,x
    sta screen+5*40,x
    inx
    cpx #kb_labels_end-kb_labels
    bne -
    ; Opening K must be released before testing starts.
kb_release:
    jsr kb_scan
    lda kb_live+4
    and #$20
    bne kb_release
    lda #0
    sta kb_exit
    lda cia1+8
    sta kb_clock
kb_frame:
    jsr frame
    jsr kb_scan
    ldx #0
    stx kb_row
kb_draw_row:
    lda kb_live,x
    ora kb_seen,x
    sta kb_seen,x
    lda kb_live,x
    sta kb_bits
    txa
    asl
    asl
    asl
    sta kb_col
    ldy #0
kb_draw_key:
    sty kb_bit
    lda #11
    sta kb_paint
    lda kb_seen,x
    and bits,y
    beq +
    lda #14
    sta kb_paint
+   lda kb_bits
    and bits,y
    beq +
    lda #5
    sta kb_paint
+   ldx kb_col
    jsr kb_paint_key
    ldy kb_bit
    inc kb_col
    ldx kb_row
    iny
    cpy #8
    bne kb_draw_key
    inc kb_row
    ldx kb_row
    cpx #8
    bne kb_draw_row
    lda kb_live
    and #2              ; RETURN
    beq kb_reset_exit
    lda kb_live+7
    and #$80            ; RUN/STOP
    beq kb_reset_exit
    lda cia1+8
    cmp kb_clock
    beq kb_frame
    sta kb_clock
    inc kb_exit
    lda kb_exit
    cmp #20
    bcc kb_frame
    jmp kb_close
kb_reset_exit:
    lda #0
    sta kb_exit
    lda cia1+8
    sta kb_clock
    jmp kb_frame
kb_scan:
    ldx #0
-   lda bits,x
    sta cia1+2
    nop
    lda cia1+1
    eor #$ff
    sta kb_live,x
    lda #0
    sta cia1+2
    inx
    cpx #8
    bne -
    rts
kb_close:
    lda #$86
    sta mode
    ldx #0
-   .for k in range(4)
    lda $1000+256*k,x
    sta screen+256*k,x
    lda $1400+256*k,x
    sta color+256*k,x
    .endfor
    inx
    bne -
    lda $1800
    sta vic+$15
    lda controls_off
    and #sound_key
    bne kb_stay_muted
    lda #12
    sta sid+24
kb_stay_muted:
    ; Discard only the interrupted sweep; retain its diagnostic results.
    ldx #$ff
    txs
    jmp next_pass
