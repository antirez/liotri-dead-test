; Quick 6510 execution checks. Only result and carry are compared in BCD mode.
cpu_alu:
    cld
    clc
    lda #$7f
    adc #1
    bvc alu_bad
    bpl alu_bad
    bcs alu_bad
    cmp #$80
    bne alu_bad
    sec
    sbc #1
    bvc alu_bad
    bmi alu_bad
    bcc alu_bad
    cmp #$7f
    bne alu_bad
    sec
    lda #$ff
    adc #0
    bne alu_bad
    bcc alu_bad
    bvs alu_bad
    lda #$80
    rol                ; carry in and out: $80 -> $01 -> $80
    bcc alu_bad
    cmp #1             ; equal leaves the carry set for ROR
    bne alu_bad
    ror
    bcc alu_bad
    cmp #$80
    bne alu_bad
    ldx #7
-   lda carry_inputs,x
    sec                ; propagate carry, then borrow, across each bit
    adc #0
    cmp bits,x
    bne alu_bad
    clc
    sbc #0
    cmp carry_inputs,x
    bne alu_bad
    dex
    bpl -
    sed
    sec
    lda #$09
    adc #0
    bcs alu_bad
    cmp #$10
    bne alu_bad
    clc
    sbc #0
    bcc alu_bad
    cmp #$09
    bne alu_bad
    clc
    lda #$45
    adc #$55
    bcc alu_bad
    cmp #0
    bne alu_bad
    sec
    sbc #1
    bcs alu_bad
    cmp #$99
    bne alu_bad
    cld
    lda #0
    rts
alu_bad:
    cld
    lda #1
    rts

carry_inputs: .byte 0,1,3,7,15,31,63,127

; The probe, its tables and its failure message must survive loss of ROML
; and I/O. $0200 is already qualified, and is free until the SID scope runs.
install_bank_probe:
    ldx #0
-   lda bank_image,x
    sta bank_probe,x
    lda bank_image+$100,x
    sta bank_probe+$100,x
    inx
    bne -
    jmp bank_probe
bank_cookie: .byte $a5
bank_image:
.logical $0200
bank_probe:
    lda #$86            ; leave Ultimax for 8K cartridge mode while in RAM
    sta mode
    lda #$34
    sta $01
    lda #$5a
    sta bank_cookie     ; marker in DRAM beneath the cartridge signature
    lda #0
    sta delta            ; first drive P0-P2, then test their input pull-ups
bank_sweep:
    ldx #7
bank_setting:
    lda delta
    bne bank_via_ddr
    txa
    ora #$30
    sta $01
    bne bank_check
bank_via_ddr:
    txa
    eor #7               ; zero output latch: release each high bit to input
    ora #$28
    sta $00
bank_check:
    ldy #0
    lda bank_cookie
    cmp cart_expected,x
    bne bank_fail
    ; A VIC write echoes at both $D000 and its $D040 mirror. DRAM echoes
    ; only at $D000. A character-ROM window follows neither write, even if
    ; its socket is empty. Require BOTH complementary tags at each address.
    lda #3
    sta tmp
    .for tag in [$55,$aa]
    lda #tag
    sta $d000
    cmp $d000
    beq +
    lda tmp
    and #2
    sta tmp
+   lda #tag
    cmp $d040
    beq +
    lda tmp
    and #1
    sta tmp
+   .endfor
    ldy #16
    lda tmp
    cmp io_expected,x
    bne bank_fail
    dex
    bpl bank_setting
    lda delta
    bne bank_pass
    inc delta
    lda #$30
    sta $01
    bne bank_sweep
bank_pass:
    lda #$37
    sta $01
    lda #$2f
    sta $00
    lda #0
    rts
bank_fail:
    ; A bad CHAREN path may leave I/O inaccessible. VIC bank, colors,
    ; screen and font were set up in Ultimax. Report using only low RAM.
    lda #$37
    sta $01
    lda #$2f
    sta $00
    stx tmp              ; requested P0-P2 levels
    ldx #0
    lda #32
-   .for k in range(4)
    sta screen+$100*k,x
    .endfor
    inx
    bne -
-   lda bank_device,y
    sta screen+12*40+7,x
    iny
    inx
    cpx #16
    bne -
    ldx #size(bank_title)-1
-   lda bank_title,x
    sta screen+8*40+7,x
    dex
    bpl -
    ldx tmp
    lda delta
    beq bank_print_setting
    lda #48              ; the DDR pass changed $00; its high nibble is 2
    sta screen+8*40+7+size(bank_title)-4
    lda #50
    sta screen+8*40+7+size(bank_title)-1
    txa
    eor #7
    ora #8
    tax
bank_print_setting:
    lda bank_hex,x
    sta screen+8*40+7+size(bank_title)
    ldx #size(bank_text)-1
-   lda bank_text,x
    sta screen+15*40+4,x
    dex
    bpl -
-   jmp -
cart_expected: .byte $5a,$5a,$5a,$a5,$5a,$5a,$5a,$a5
io_expected:   .byte 1,0,0,0,1,3,3,3
bank_hex: .text "0123456789ABCDEF"
bank_device: .text "8K CART / RAM   VIC $D000/$D040 "
bank_title: .text "BANK MAP FAILED  $01=$3"
bank_text: .text "CHECK 6510 PORT / PLA / DECODE"
bank_end:
.cerror bank_end-bank_probe > $200, "bank probe overlaps the screen"
.here
