kb_header:
    #row "KEYBOARD TEST - UNPLUG BOTH JOYSTICKS"
    #row "GREEN:HELD BLUE:SEEN GRAY:UNTESTED"
    #row "HOLD RUN/STOP + RETURN 2 SEC TO EXIT"
    #row "RESTORE IS NOT A MATRIX KEY"
kb_header_end:
; Physical keyboard layout. The two C labels are down/up and right/left
; cursor keys. SHIFT LOCK mirrors the left Shift matrix contact.
kb_labels:
    .text "ARR 1 2 3 4 5 6 7 8 9 0 + - "
    .byte 28                           ; pound key
    .text " HM DEL  F1"
    .text "CTRL Q W E R T Y U I O P @ * "
    .byte 30                           ; up-arrow key
    .text "        F3"
    #row "STP SHLK A S D F G H J K L : ; = RET  F5"
    #row "CMD LSH Z X C V B N M , . / RSH C C   F7"
    #row "               SPACE"
kb_labels_end:
    .cerror kb_labels_end-kb_labels != 5*40, "keyboard layout must be five rows"

; Screen color offset and label width for each key in CIA matrix order.
kb_key_offset:
    .byte 33,113,154,158, 38,78,118,152
    .byte 8,47,89,10, 128,91,49,124
    .byte 12,51,93,14, 132,95,53,130
    .byte 16,55,97,18, 136,99,57,134
    .byte 20,59,101,22, 140,103,61,138
    .byte 24,63,105,26, 144,107,65,142
    .byte 28,67,109,30, 148,111,69,146
    .byte 4,0,40,6, 175,120,45,80
kb_key_width:
    .byte 3,3,1,2, 2,2,2,1
    .byte 1,1,1,1, 1,1,1,3
    .byte 1,1,1,1, 1,1,1,1
    .byte 1,1,1,1, 1,1,1,1
    .byte 1,1,1,1, 1,1,1,1
    .byte 1,1,1,1, 1,1,1,1
    .byte 1,1,1,2, 3,1,1,1
    .byte 1,3,4,1, 5,3,1,3

; Paint a variable-width label. This lives with the layout in ROMH, which is
; visible at $e000 while the keyboard modal uses EasyFlash Ultimax mapping.
kb_paint_key:
    lda kb_key_offset,x
    cmp #$ff
    beq kb_key_painted
    clc
    adc #<(color+5*40)
    sta kb_ptr
    lda #>(color+5*40)
    adc #0
    sta kb_ptr+1
    ldy kb_key_width,x
    lda kb_paint
-   dey
    sta (kb_ptr),y
    bne -
kb_key_painted:
    ; SHIFT LOCK is a mechanical latch on the left Shift matrix contact.
    cpx #15
    bne +
    ldy #3
    lda kb_paint
-   sta color+7*40+4,y
    dey
    bpl -
+   rts
