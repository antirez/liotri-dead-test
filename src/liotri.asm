; Liotri C64 hardware test. NMOS 6510, PAL/NTSC, one EasyFlash bank. Assemble with 64tass.
; The CRT headers are assembled too, so the emitted file is ready to plug in.
.cpu "6502"
* = 0
.enc "ascii"
.cdef " ~",32
.text "C64 CARTRIDGE   "
.byte 0,0,0,$40, 1,0, 0,32, 1,0
.fill 6,0
.text "LIOTRI C64 HARDWARE TEST"
.fill $40-*,0
chip .macro addr
    .text "CHIP"
    .byte 0,0,$20,$10, 0,2, 0,0, >\addr,<\addr, $20,0
.endmacro
#chip $8000
.logical $8000

; Screen layout and the workspace admitted by cold_start.
screen = $0400
color = $d800
vic = $d000
sid = $d400
cia1 = $dc00
cia2 = $dd00
mode = $de02
ptr = $02
page = ptr+1
stage = $04
seed = $05
delta = $06
badbits = $07
firstlo = $08
firsthi = $09
tmp = $0a
crc = $0c
phase = $0e
beat = $0f
pass = $10
tod1 = $11
tod2 = $12
faults = $13
map_ptr = $14
sprite_phase = $16
rom_index = $17
hold = $18
note = $19
pending_tod = $1a
irq_seen = $1b
nmi_seen = $1c
addr_bad = $1d
tod_seen = $1e
rom_mark = $1f
last_stage = $20
step_down = $21
show_orbit = $22
march_value = $23
harness_found = $24
addr_probe = $25
keys_sample = $26
keys_down = $27
controls_off = $28      ; PB2 = animation disabled, PB5 = muted
anim_key = $04
sound_key = $20
run_status = screen+40+16
sprite_shape = $0300
flags = $0b00           ; sticky page flags in unused character slots 96..127
map = screen + 3*40+21
status = screen+3*40+14
meter = screen+23*40+2
.enc "panel"
.cdef "AZ",1," ?",32,"@@",0

start:
    lda #$86            ; ROML stays at $8000 on both sides of the change
    sta mode
    ldx #$ff
    txs
    lda #3
    sta cia2
    lda #$3b            ; VIC bank 0; PA3..5 low release the IEC lines.
    sta cia2+2
    lda #$08
    sta vic+$16
    lda #$12            ; screen at $0400, glyphs in qualified RAM at $0800
    sta vic+$18
    lda #11
    sta vic+$21
    lda #0
    sta vic+$20
    sta vic+$15
    sta vic+$17
    sta vic+$1b
    sta vic+$1c
    sta vic+$1d
    ldx #controls_off-$02
-   sta $02,x
    dex
    bpl -
    ldx #0
-   sta flags,x
    inx
    bne -
    lda #$7f
    sta cia1+13
    sta cia2+13
    lda #0
    sta vic+$1a
    sta cia1+15
    sta cia2+15
    sta cia1+8         ; unlock/start the TODs, qualify while other tests run
    sta cia2+8
    lda cia1+8
    sta tod1
    lda cia2+8
    sta tod2
    lda #1
    sta pending_tod
    jsr color_test
    pha
    jsr panel
    jsr fallback_font    ; even the first visible glyph comes from the cart
    pla
    bne +
    ldx #0
    stx vic+$21
    ldx #11
    stx vic+$20
+   ldx #1
    jsr verdict
    ldx #0
    lda #0
    jsr verdict
    lda #$1b
    sta vic+$11
    jsr install_kernel
    jsr address_test
    sta addr_bad
    ldx #7
    jsr verdict
    jsr raster_test
    ldx #2
    jsr verdict
    #cia_test cia1
    ldx #3
    jsr verdict
    #cia_test cia2
    ldx #4
    jsr verdict
    jsr irq_test
    jsr sid_test
    ldx #5
    jsr verdict
    jsr sprites_test
    ldx #6
    jsr verdict
    jsr poll_tod
    jsr userport
    lda harness_found
    beq +
    jsr accessories
+   lda #0              ; hand CIA1 ports over to the keyboard after harness tests
    sta cia1+2
    sta cia1+3
    sta cia1             ; only PA1 will be driven, and only during a scan
    jsr roms
    ; If an alias affects the VIC's RAM fetches the panel itself can be
    ; unreadable. Switch both its matrix and its font to cartridge ROM.
    lda addr_bad
    beq next_pass
    ldx addr_probe
    lda #$85
    sta mode
    jmp address_report

next_pass:
    inc pass
    lda pass
    ldx #0
    jsr hexbyte
    lda seed
    clc
    adc #$5a
    sta seed
    lda #0
    sta show_orbit
    sta stage
    jsr reset_map
    lda #3
    sta last_stage
    lda pass
    and #1
    bne stage_loop
    lda #6              ; even passes run the six March C- elements
    sta stage
    lda #12
    sta last_stage
stage_loop:
    ldx stage
    jsr stage_name
    ldx stage
    lda direction,x
    sta step_down
    lda #$10
    ldx step_down
    beq +
    lda #$ff
+   sta page
page_loop:
    jsr scan_page
    jsr tile
    jsr instruments
    ldx step_down
    bne backwards
    inc page
    bne page_loop
    beq stage_done
backwards:
    dec page
    lda page
    cmp #$0f
    bne page_loop
stage_done:
    inc stage
    lda stage
    cmp last_stage
    bne stage_loop
    lda faults
    ora badbits
    beq clean_pass
    ldx #4
    jsr stage_name
    lda #100
    jsr hold_map
    jmp next_pass
clean_pass:
    ldx #8
-   lda clean_label,x
    sta run_status,x
    lda #13
    sta color+(run_status-screen),x
    dex
    bpl -
    ldx #5
    jsr stage_name
    lda #24              ; show the final RAM tiles before painting the scope
    jsr hold_map
    inc show_orbit
    jsr scope_colors
    ldx #pass_message_end-pass_message-1
-   lda pass_message,x
    sta screen+20*40,x
    dex
    bpl -
    ldx #12
-   lda scope_title,x
    sta screen+42,x
    dex
    bpl -
    lda #76
    ldx pass
    cpx #1              ; give the operator more reading time on the first pass
    bne +
    lda #176
+   sta hold
scope_frame:
    jsr instruments
    jsr scope_map
    jsr frame
    dec hold
    bne scope_frame
    ldx #pass_message_end-pass_message-1
    lda #32
-   sta screen+20*40,x
    dex
    bpl -
    jmp next_pass

hold_map:
    sta hold
-   jsr instruments
    jsr frame
    dec hold
    bne -
    rts

clean_label: .text "CHECKS OK"
fault_label: .text "RAM FAULT"
pass_message:
    #row "            NO FAULTS FOUND."
    #row "      RAM TESTS REPEAT UNTIL RESET"
    #row "      TO CATCH INTERMITTENT FAULTS."
pass_message_end:

; Read all 1024 low nibbles. Fold each of A0..A9 into the test value.
; Separate complete writes and reads so a mutually aliased pair is exposed.
color_test:
    ldy #0
    sty delta
color_pass:
    ldx #0
-   lda folded,x
    eor color_seed,y
    sta color,x
    eor #1
    sta color+$100,x
    eor #3
    sta color+$200,x
    eor #1
    sta color+$300,x
    inx
    bne -
    ldx #0
-   .for k in range(4)
    lda folded,x
    eor color_seed,y
    eor #k
    eor color+k*$100,x
    and #15
    ora delta
    sta delta
    .endfor
    inx
    bne -
    iny
    cpy #2
    bne color_pass
    lda delta
    rts
color_seed: .byte 0,15
folded: .for i in range(256)
    .byte (i ^ (i >> 4)) & 15
    .endfor

; Counter should move through several raster lines even with IRQs masked.
raster_test:
    ldy #4
    lda vic+$12
-   ldx #100
    sta tmp
-   lda vic+$12
    cmp tmp
    bne +
    dex
    bne -
    lda #1
    rts
+   dey
    bne --
    lda #0
    rts

; Timer A clocks Timer B; Timer B must stop itself after its one shot.
; Interrupt status must latch both sources, then clear on read.
cia_test .macro base
    lda #$80
    sta \base+14
    lda #0
    sta \base+15
    sta \base+5
    sta \base+7
    lda #31
    sta \base+4
    lda #3
    sta \base+6
    lda \base+13
    lda #$59
    sta \base+15
    lda #$91
    sta \base+14
    ldx #100
-   dex
    bne -
    lda #$80
    sta \base+14
    lda \base+15
    and #1
    sta tmp
    lda \base+13
    and #$83
    eor #3
    ora tmp
    sta tmp
    lda \base+13
    and #3
    ora tmp
.endmacro

; Use the ROMH interrupt vectors in Ultimax. Even an IRQ held low cannot
; trap us in an interrupt storm: its handler masks IRQ in the saved P.
irq_test:
    lda #$85
    sta mode
    lda #0
    sta irq_seen
    sta nmi_seen
    .for base in [cia1,cia2]
    lda #$80
    sta base+14
    lda #0
    sta base+5
    lda #180
    sta base+4
    lda base+13
    lda #$81
    sta base+13
    lda #$99
    sta base+14
    .endfor
    cli
    ldx #0
-   dex
    bne -
    sei
    lda #$7f
    sta cia1+13
    sta cia2+13
    lda cia1+13
    lda cia2+13
    lda #$86
    sta mode
    lda irq_seen
    eor #$81
    ldx #14
    jsr verdict
    lda nmi_seen
    eor #$81
    ldx #15
    jmp verdict

; Test the two actual SID readbacks. The purely audible paths have a separate
; voice sequence. A held TEST resets the saw phase; gate makes ENV3 attack.
sid_test:
    ldx #24
    lda #0
-   sta sid,x
    dex
    bpl -
    lda #$28
    sta sid+18
    lda #$81
    sta sid+15
    lda #$f0
    sta sid+20
    lda sid+27
    sta delta
    lda #$21
    sta sid+18
    lda sid+27
    sta tmp
    ldx #0
-   lda sid+27
    cmp tmp
    bne +
    dex
    bne -
    inc delta
    ; An original SID can wait for its 15-bit ADSR counter to wrap
    ; after a rate change. Exit immediately on success, allow that wrap.
+   ldy #24
-   ldx #0
-   lda sid+28
    cmp #16
    bcs attack_seen
    dex
    bne -
    dey
    bne --
    inc delta
attack_seen:
    sta tmp
    lda #$20
    sta sid+18
    ldy #24
-   ldx #0
-   lda sid+28
    cmp tmp
    bcc released
    dex
    bne -
    dey
    bne --
    inc delta
released:
    lda delta
    rts

; A bounded frame wait also keeps a faulty raster from hanging the panel.
frame:
    ldy #2
frame_edge:
    ldx #0
    stx tmp
-   lda vic+$11
    eor frame_level-1,y
    bmi +
    inx
    bne -
    inc tmp
    lda tmp
    cmp #12
    bne -
    rts
+   dey
    bne frame_edge
    rts
frame_level: .byte $80,0

sprites_test:
    ldx #0
-   lda shapes,x
    sta sprite_shape,x
    inx
    bne -
    ldx #7
-   lda #sprite_shape/64
    sta screen+1016,x
    lda palette,x
    sta vic+$27,x
    dex
    bpl -
    ldx #14
-   lda #160
    sta vic,x
    lda #220
    sta vic+1,x
    dex
    dex
    bpl -
    lda #0
    sta vic+$10
    ; An opaque character target underneath the constellation.
    lda #$a0
    sta screen+21*40+17
    lda #$ff
    sta vic+$15
    jsr frame
    lda vic+$1e
    lda vic+$1f
    jsr frame
    lda vic+$1e
    and vic+$1f
    eor #$ff
    sta delta
    lda vic+$1e            ; read-to-clear, before sprites return next frame
    ora vic+$1f
    ora delta
    sta delta
    lda #$55
    sta vic+$15
    jsr frame
    lda vic+$1e
    eor #$55
    ora delta
    sta delta
    lda vic+$1f
    eor #$55
    ora delta
    sta delta
    ldx #7
-   txa
    and #3
    clc
    adc #sprite_shape/64
    sta screen+1016,x
    dex
    bpl -
    jsr position_sprites
    lda #$ff
    sta vic+$15
    jsr frame
    lda vic+$1e           ; separate all eight, expect zero
    ora delta
    pha
    lda #32
    sta screen+21*40+17
    lda #$ff
    sta vic+$15
    lda #12
    sta sid+24
    pla
    rts

; A passive 586220 user-port dongle connects PB0..3 to PB4..7.
; Drive only one zero at a time (the other pins are inputs), in both
; directions. OPEN is also the normal result with no harness attached.
userport:
    lda #0
    sta cia2+3
    sta cia2+1
    sta delta
    ldx #7
-   lda bits,x
    sta cia2+3
    nop
    lda cia2+1
    eor expected_pb,x
    bne +
    inc harness_found
+   ora delta
    sta delta
    dex
    bpl -
    lda #0
    sta cia2+3
    ldx #13
    lda delta
    beq port_ok
    ldy harness_found
    bne port_ok           ; some pairs worked: this is a damaged loop
    lda #3
    jmp field
port_ok:
    jmp verdict
bits: .byte 1,2,4,8,16,32,64,128
expected_pb: .byte $ee,$dd,$bb,$77,$ee,$dd,$bb,$77

.include "harness.asm"

; CRC-16/CCITT-FALSE, high byte first, init $ffff. Changing CHAREN
; exposes U5 to the CPU while ROML stays mapped for instruction fetch.
roms:
    lda #0
    sta rom_index
rom_loop:
    ldx rom_index
    lda rom_start,x
    sta page
    lda #0
    sta ptr
    lda #$ff
    sta crc
    sta crc+1
crc_page:
    lda rom_port,x
    sta $01
    ldy #0
-   lda (ptr),y
    eor crc+1
    tax
    lda crc_lo,x
    sta tmp
    lda crc_hi,x
    eor crc
    sta crc+1
    lda tmp
    sta crc
    iny
    bne -
    inc page
    lda #$37             ; the character ROM can cover CIA1 during the CRC
    sta $01
    jsr read_keys
    ldx rom_index
    lda page
    cmp rom_end,x
    bne crc_page
    lda crc
    cmp good_lo,x
    bne rom_diff
    lda crc+1
    cmp good_hi,x
    bne rom_diff
    cpx #0
    bne +
    jsr stock_font
+   lda #0
    beq rom_result
rom_diff:
    inc faults
    lda #2
rom_result:
    sta rom_mark
    ldx rom_index
    ldy rom_rows,x
    tya
    tax
    lda rom_mark
    jsr field
    jsr poll_tod
    inc rom_index
    lda rom_index
    cmp #3
    bne rom_loop
    rts
rom_rows: .byte 8,9,10
rom_start: .byte $d0,$a0,$e0
rom_end:   .byte $e0,$c0,0
rom_port:  .byte $33,$37,$37
good_lo:   .byte $42,$12,$4f
good_hi:   .byte $f5,$a5,$30
crc_entry .function i
    v := i << 8
    .rept 8
        v := ((v << 1) & $ffff) ^ ($1021 * ((v >> 15) & 1))
    .endrept
    .endfunction v
crc_lo: .for i in range(256)
    .byte <crc_entry(i)
    .endfor
crc_hi: .for i in range(256)
    .byte >crc_entry(i)
    .endfor

; This kernel fits in qualified low RAM. Disable the cartridge with the I/O
; visible first, then expose all DRAM with the 6510 port. Restore in reverse.
install_kernel:
    ldx #0
-   lda kernel_image,x
    sta all_ram,x
    lda kernel_image+256,x
    sta all_ram+256,x
    inx
    bne -
    rts
kernel_image:
.logical $0e00
all_ram:
    lda #4
    sta mode
    lda #$30
    sta $01
    rts
normal_map:
    lda #$37
    sta $01
    lda #$86
    sta mode
    rts

; Write different tags at a base address and its 16 one-bit neighbours
; before checking any of them. This covers A0..A15, including ROML's A15.
address_test:
    lda #$ff
    sta addr_probe
    jsr all_ram
    lda #0
    sta delta
    ldx #16
-   jsr address_ptr
    txa
    sta (ptr),y
    dex
    bpl -
    ldx #16
-   jsr address_ptr
    txa
    eor (ptr),y
    beq probe_next
    ora delta
    sta delta
    lda addr_probe
    bpl probe_next
    txa
    cpx #16
    bne probe_record
    lda (ptr),y         ; base was overwritten: which neighbour's tag?
    cmp #16
    bcc probe_record
    lda #16             ; value is not a tag
probe_record:
    sta addr_probe
probe_next:
    dex
    bpl -
    jsr normal_map
    lda #0
    sta ptr
    lda delta
    rts
address_ptr:
    lda addr_lo,x
    sta ptr
    lda addr_hi,x
    sta page
    ldy #0
    rts
addr_lo: .for i in range(17)
    .byte <($37b7 ^ ((1 << i) & $ffff))
    .endfor
addr_hi: .for i in range(17)
    .byte >($37b7 ^ ((1 << i) & $ffff))
    .endfor

; Global sweeps, so far-apart aliases or a lost write survive until verify.
; Log the very first failing address, OR all failing data bits, keep scanning.
scan_page:
    jsr all_ram
    lda #0
    sta delta
    ldx stage
    beq write_page
    cpx #6
    bcs march_page
    dex
    beq invert_page
    ldy #0
-   tya
    eor page
    eor seed
    eor #$ff
    eor (ptr),y
    beq +
    jsr mismatch
+   iny
    bne -
    jmp normal_map
invert_page:
    ldy #$ff
-   tya
    eor page
    eor seed
    tax
    eor (ptr),y
    beq +
    jsr mismatch
+   txa
    eor #$ff
    sta (ptr),y
    dey
    cpy #$ff
    bne -
    jmp normal_map
write_page:
    ldy #0
-   tya
    eor page
    eor seed
    sta (ptr),y
    iny
    bne -
    jmp normal_map
mismatch:
    ora delta
    sta delta
    lda badbits
    bne +
    sty firstlo
    lda page
    sta firsthi
+   lda delta
    ora badbits
    sta badbits
    rts
; Full March C- over $1000..$ffff. Each read/write pair acts on one cell
; before moving to the next. Both page order and byte order are reversed.
march_page:
    cpx #6
    bne march_read
    ldy #0
    lda #0
-   sta (ptr),y
    iny
    bne -
    jmp normal_map
march_read:
    lda #0
    cpx #8
    beq march_ones
    cpx #10
    bne +
march_ones:
    lda #$ff
+   sta march_value
    ldy #0
    lda step_down
    beq march_up
    dey
march_down:
    lda (ptr),y
    eor march_value
    beq +
    jsr mismatch
+   lda march_value
    eor #$ff
    sta (ptr),y
    dey
    cpy #$ff
    bne march_down
    jmp normal_map
march_up:
    lda (ptr),y
    eor march_value
    beq +
    jsr mismatch
+   cpx #11
    beq +
    lda march_value
    eor #$ff
    sta (ptr),y
+   iny
    bne march_up
    jmp normal_map
.cerror * > $1000, "RAM kernel exceeds its qualified pages"
.here

; Two-digit hex. X selects a pair in the footer: pass, first page/offset, mask.
hexbyte:
    pha
    lsr
    lsr
    lsr
    lsr
    jsr digit
    inx
    pla
    and #15
digit:
    tay
    lda hex,y
    ldy hex_pos,x
    sta screen+24*40,y
    rts
hex: .text "0123456789ABCDEF"
hex_pos: .byte 7,8, 19,20, 21,22, 32,33

; Re-arm the page map for the next sweep. Faults retain their red cross.
reset_map:
    ldx #12
-   lda layout+42,x
    sta screen+42,x
    dex
    bpl -
    lda #12             ; tile index for a cold-tested low page
    sta stage
    lda #0
    sta delta
    sta page
-   jsr tile
    inc page
    beq +
    lda page
    cmp #16
    bne -
    inc stage           ; remaining pages await this sweep
    bne -
+   lda #0
    sta stage
    rts

tile:
    ldx page
    lda flags,x
    ora delta
    sta flags,x
    lda page
    lsr
    lsr
    lsr
    lsr
    tay
    lda map_lo,y
    sta map_ptr
    lda map_hi,y
    sta map_ptr+1
    txa
    and #15
    tay
    lda flags,x
    bne bad_tile
    ldx stage
    lda tile_char,x
    sta (map_ptr),y
    lda tile_color,x
    bne tile_ink
bad_tile:
    lda #69
    sta (map_ptr),y
    lda #10
tile_ink:
    pha
    lda map_ptr+1
    clc
    adc #>(color-screen)
    sta map_ptr+1
    pla
    sta (map_ptr),y
    lda delta
    beq +
    ldx #8
-   lda fault_label,x
    sta run_status,x
    lda #10
    sta color+(run_status-screen),x
    dex
    bpl -
    ldx #2
    lda firsthi
    jsr hexbyte
    ldx #4
    lda firstlo
    jsr hexbyte
    ldx #6
    lda badbits
    jsr hexbyte
+   rts
tile_char:  .byte 65,66,67,0,0,0, 65,66,65,66,65,67,68,64
tile_color: .byte 14,3,13,0,0,0, 14,3,14,3,14,13,3,12
direction:  .byte 0,1,0,0,0,0, 0,0,0,1,1,0
map_lo: .for i in range(16)
    .byte <(map+i*40)
    .endfor
map_hi: .for i in range(16)
    .byte >(map+i*40)
    .endfor

; Default screen code font can be replaced without help from U5. Only the
; first 64 glyphs and their inverses are used, including reverse space.
fallback_font:
    ldx #0
-   .for k in range(2)
    lda font+256*k,x
    sta $0800+256*k,x
    eor #$ff
    sta $0c00+256*k,x
    .endfor
    inx
    bne -
    jmp install_glyphs
stock_font:
    lda #$33
    sta $01
    ldx #0
-   .for k in range(2)
    lda $d000+256*k,x
    sta $0800+256*k,x
    eor #$ff
    sta $0c00+256*k,x
    .endfor
    inx
    bne -
    lda #$37
    sta $01
    rts                 ; VIC keeps fetching from the same RAM addresses
install_glyphs:
    ldx #size(glyphs)-1
-   lda glyphs,x
    sta $0a00,x
    dex
    bpl -
    ldx #15
-   lda #68
    sta map,x
    lda #3
    sta color+3*40+21,x
    dex
    bpl -
    rts

; 64..69: waiting, write, invert, verified, cold-checked, error.
; 70..77: the eight scope heights.
glyphs .block
    .byte 0,0,0,$18,$18,0,0,0
    .byte 0,$54,$28,$54,$28,$54,$28,0
    .byte 0,$7c,$44,$5c,$54,$7c,0,0
    .byte 0,$7e,$7e,$7e,$7e,$7e,$7e,0
    .byte 0,$7e,$42,$42,$42,$42,$7e,0
    .byte 0,$66,$3c,$18,$18,$3c,$66,0
    .for h in range(8)
        .for y in range(8)
            .byte $ff * (y == 7-h)
        .endfor
    .endfor
    .endblock

panel:
    ldx #0
-   .for k in range(4)
    lda layout+k*256,x
    sta screen+k*256,x
    lda #15
    sta color+k*256,x
    .endfor
    inx
    bne -
    ldx #39
-   lda screen,x
    ora #$80
    sta screen,x
    lda #3
    sta color,x
    sta color+23*40,x
    lda #14
    sta color+2*40,x
    sta color+19*40,x
    dex
    bpl -
    rts

; A is the measured difference, X is the row in the results column.
verdict:
    cmp #0
    beq field
    inc faults
    lda #1
field:
    asl
    asl
    tay
    lda result_lo,x
    sta map_ptr
    lda result_hi,x
    sta map_ptr+1
    tya
    tax
    ldy #0
-   lda result_text,x
    sta (map_ptr),y
    inx
    iny
    cpy #4
    bne -
    lda map_ptr+1
    clc
    adc #>(color-screen)
    sta map_ptr+1
    dex
    lda result_ink,x
    ldy #3
-   sta (map_ptr),y
    dey
    bpl -
    rts
result_text: .text "OK  BAD DIFFOPEN"
result_ink: .byte 13,13,13,13, 10,10,10,10, 7,7,7,7, 12,12,12,12
result_lo: .for i in range(16)
    .byte <(status+i*40)
    .endfor
result_hi: .for i in range(16)
    .byte >(status+i*40)
    .endfor

stage_name:
    txa
    asl
    asl
    asl
    tax
    ldy #0
-   lda stage_text,x
    sta screen+1*40+27,y
    inx
    iny
    cpy #8
    bne -
    rts
stage_text: .text "WRITE   INVERT  VERIFY          FAULT   PASS    "
            .text "M0 FILL M1 UP   M2 UP   M3 DOWN M4 DOWN M5 READ "

; Slow tests overlap the sweeps. Use successive TOD readings, so a stuck
; nonzero register cannot pass. Gate three voices and draw actual OSC3 bins.
poll_tod:
    lda pending_tod
    beq tod_return
    .for base,old,mask in [(cia1,tod1,1),(cia2,tod2,2)]
    lda base+8
    cmp #10             ; only valid tenths can qualify
    bcs +
    cmp old
    beq +
    lda tod_seen
    ora #mask
    sta tod_seen
+   .endfor
    inc pending_tod
    lda tod_seen
    cmp #3
    beq tod_done
    lda pending_tod
    cmp #100
    bne tod_return
tod_done:
    .for mask,rowno in [(1,11),(2,12)]
    lda tod_seen
    and #mask
    eor #mask
    ldx #rowno
    jsr verdict
    .endfor
    lda #0
    sta pending_tod
tod_return:
    rts

; Release all columns between scans. Ignore rows already grounded by a
; joystick or a short. Accept a change after two equal samples, then
; toggle only on the press edge so a held key cannot auto-repeat.
read_keys:
    lda cia1+1
    sta tmp
    ldx #2
    stx cia1+2
    nop
    lda cia1+1
    ldx #0
    stx cia1+2
    eor #$ff
    and tmp
    and #anim_key|sound_key
    cmp keys_sample
    sta keys_sample
    bne keys_done
    tay
    eor keys_down
    sty keys_down
    and keys_down
    beq keys_done
    eor controls_off
    sta controls_off

    ; The SID oscillators keep running at zero volume for the scope.
    and #sound_key
    beq sound_on
    lda #0
    ldx #3
    bne set_volume
sound_on:
    lda #12
    ldx #0
set_volume:
    sta sid+24
    ldy #sound_field-screen
    jsr switch_label
    lda controls_off
    and #anim_key
    beq sprites_on
    lda #0
    ldx #3
    bne set_sprites
sprites_on:
    lda #$ff
    ldx #0
set_sprites:
    sta vic+$15
    ldy #sprite_field-screen
switch_label:
    .for k in range(3)
    lda switch_text+k,x
    sta screen+k,y
    .endfor
keys_done:
    rts
switch_text: .text "ON OFF" ^ $80  ; reverse video, like the dashboard title

instruments:
    jsr read_keys
    jsr poll_tod
    inc phase
    lda controls_off
    and #anim_key
    bne play_score
    lda phase
    and #3
    bne +
    inc sprite_phase
+   jsr position_sprites
    jmp play_score
position_sprites:
    lda show_orbit
    bne orbit
    ldx #0
-   txa
    asl
    asl
    asl
    clc
    adc sprite_phase
    and #63
    tay
    lda wave,y
    clc
    adc sprite_x,x
    pha
    txa
    asl
    tay
    pla
    sta vic,y
    txa
    asl
    asl
    clc
    adc sprite_phase
    and #63
    tay
    lda wave,y
    lsr
    lsr
    clc
    adc #211
    pha
    txa
    asl
    tay
    pla
    sta vic+1,y
    inx
    cpx #8
    bne -
    lda #$c0            ; sprites 6 and 7 are to the right of X=255
    sta vic+$10
    rts

; During the healthy pass summary the eight sprites orbit the memory map.
; The two lookup tables encode actual VIC coordinates, including X bit 8.
orbit:
    lda #0
    sta vic+$10
    ldx #7
-   txa
    asl
    asl
    asl
    clc
    adc phase
    and #63
    tay
    lda orbit_x_hi,y
    beq +
    lda vic+$10
    ora bits,x
    sta vic+$10
+   lda orbit_x_lo,y
    pha
    lda orbit_y,y
    pha
    txa
    asl
    tay
    pla
    sta vic+1,y
    pla
    sta vic,y
    dex
    bpl -
    rts
orbit_x_lo: .for i in range(64)
    .byte <int(248+58*cos(2*pi*i/64))
    .endfor
orbit_x_hi: .for i in range(64)
    .byte >int(248+58*cos(2*pi*i/64))
    .endfor
orbit_y: .for i in range(64)
    .byte int(125+48*sin(2*pi*i/64))
    .endfor

play_score:
    inc beat
    lda beat
    and #31
    cmp #24
    bne +
    lda #$10
    sta sid+4
    lda #$40
    sta sid+11
    lda #$20
    sta sid+18
    jmp scope
+   cmp #0
    bne scope
    ldx note
    .for v in range(3)
    lda tune_lo+v*8,x
    sta sid+v*7
    lda tune_hi+v*8,x
    sta sid+v*7+1
    .endfor
    inx
    txa
    and #7
    sta note
    lda #$09
    sta sid+10
    lda #$28
    sta sid+5
    sta sid+12
    lda #$02
    sta sid+19
    lda #$a6
    sta sid+6
    sta sid+13
    sta sid+20
    lda #$11
    sta sid+4
    lda #$41
    sta sid+11
    lda #$21
    sta sid+18
scope:
    ; A fresh 32-sample trace on every update. Sample spacing is the CPU
    ; loop, so the trace visibly changes slope as the score changes pitch.
    ldy #0
-   lda sid+27
    lsr
    lsr
    lsr
    lsr
    lsr
    clc
    adc #70
    sta meter,y
    iny
    cpy #32
    bne -
    lda sid+28
    lsr
    lsr
    lsr
    lsr
    lsr
    tax
    lda palette,x
    sta color+23*40+36
    lda #67
    sta screen+23*40+36
    rts
; A healthy pass earns a short large-format oscillator display. The map
; doubles as 16 columns of samples from OSC3. Individual tests never wait
; for this view. Capture at a fixed spacing before painting any pixels.
scope_title: .text "SID / OSC3   "
samples = $0280
scope_map:
    ldy #15
-   lda sid+27
    lsr
    lsr
    lsr
    lsr
    sta samples,y
    ldx #26
-   dex
    bne -
    dey
    bpl --
    ldx #0
scope_row:
    lda map_lo,x
    sta map_ptr
    lda map_hi,x
    sta map_ptr+1
    txa
    eor #15
    sta tmp
    ldy #15
-   lda samples,y
    cmp tmp
    lda #64
    bcc +
    lda #67
+   sta (map_ptr),y
    dey
    bpl -
    inx
    cpx #16
    bne scope_row
    rts
scope_colors:
    ldx #0
scope_color_row:
    lda map_lo,x
    sta map_ptr
    lda map_hi,x
    clc
    adc #>(color-screen)
    sta map_ptr+1
    lda scope_ink,x
    ldy #15
-   sta (map_ptr),y
    dey
    bpl -
    inx
    cpx #16
    bne scope_color_row
    rts
scope_ink: .byte 1,1,7,7,13,13,3,3,14,14,4,4,6,6,6,6

palette: .byte 14,3,13,1,7,8,10,4
sprite_x: .byte 30,68,106,144,182,220,2,40
wave: .for i in range(64)
    .byte int(8+8*sin(2*pi*i/64))
    .endfor
; Triangle bass, pulse arpeggio, saw lead. A minor, on a PAL phi2 clock.
score = [45,45,41,41,48,48,43,43, 57,64,60,65,60,67,59,62, 69,72,69,65,67,72,71,67]
sid_pitch .sfunction n, int(0.5 + 440.0 * 2.0**((n-69)/12.0) * $1000000 / 985248)
tune_lo: .for n in score
    .byte <sid_pitch(n)
    .endfor
tune_hi: .for n in score
    .byte >sid_pitch(n)
    .endfor

; Four 24x21 hardware sprites. The mask in the collision test uses the
; first ship for all eight channels, then gives each its own pointer.
.include "sprites.asm"

row .macro s
    .text \s
    .cerror len(\s)>40, "panel row overflows"
    .fill 40-len(\s),32
.endmacro
layout:
    .text " LIOTRI C64   S:SOUND "
sound_field = screen+(*-layout)
    .text "ON   A:SPRITES "
sprite_field = screen+(*-layout)
    .text "ON "
    .cerror *-layout != 40, "shortcut bar must fill the first row"
    #row "  MEMORY SWEEP             STARTUP"
    #row "  DEVICE      TEST   0123456789ABCDEF"
    #row "  LOW 4K      ..   0 OOOOOOOOOOOOOOOO"
    #row "  COLOR RAM   ..   1 ................"
    #row "  VIC RASTER  ..   2 ................"
    #row "  CIA1 A/B    ..   3 ................"
    #row "  CIA2 A/B    ..   4 ................"
    #row "  SID OSC/ENV ..   5 ................"
    #row "  VIC SPRITES ..   6 ................"
    #row "  ADDRESS BUS ..   7 ................"
    #row "  CHAR ROM    ..   8 ................"
    #row "  BASIC ROM   ..   9 ................"
    #row "  KERNAL ROM  ..   A ................"
    #row "  CIA1 TOD    ..   B ................"
    #row "  CIA2 TOD    ..   C ................"
    #row "  USER LOOP   ..   D ................"
    #row "  CIA1 IRQ    ..   E ................"
    #row "  CIA2 NMI    ..   F ................"
    #row "  VIC / SPRITE DMA              SID / 3V"
    #row ""
    #row ""
    #row ""
    #row ""
    #row "  PASS --   FIRST $----    BITS --"
    .fill 24,32

.include "font.asm"
.cerror * > $a000, "ROML overflow"
.fill $a000-*,$ff
.here
.enc "ascii"
#chip $a000              ; CRT ROMH socket. Ultimax reset maps it at $e000.
.logical $e000
cold_start:
    sei
    cld
    lda #$37
    sta $01
    lda #$2f
    sta $00
    lda #0
    sta vic+$11
    sta vic+$15
    sta vic+$1a
    sta vic+$21
    lda #6
    sta vic+$20
    ; One byte, no stack, both polarities on every lane.
    ldx #0
    .for i in range(8)
    lda #(1 << i)
    sta $02
    eor $02
    and #(1 << i)
    bne +
    lda #($ff ^ (1 << i))
    sta $02
    eor $02
    and #(1 << i)
    beq ++
+   txa
    ora #(1 << i)
    tax
+   .endfor
    txa
    bne flash_bits
    ; Verify every CPU-accessible byte of the Ultimax 4K before any JSR.
    ; Test tags include both address bytes. Alternate complete write/read
    ; sweeps and their inverses so aliases survive until the comparison.
    .for polarity in [0,$ff]
    ldx #0
-   txa
    eor #polarity
    cpx #2
    bcc +
    sta $00,x
+   .for p in range(1,16)
    eor #(p ^ (p-1))
    sta p*256,x
    .endfor
    inx
    bne -
    ldx #0
-   cpx #2
    bcc +
    txa
    eor #polarity
    eor $00,x
    bne flash_bits
+   .for p in range(1,16)
    txa
    eor #(polarity ^ p)
    eor p*256,x
    bne flash_bits
    .endfor
    inx
    bne -
    .endfor
    jmp start

; SP holds the flash count as a register. No PHA/JSR, stack memory, CIA,
; raster wait, character ROM or screen RAM is involved. Highest differing
; bit has priority. One flash = D7 ... eight = D0 (781220 convention).
flash_bits:
    tax
    txs                 ; keep the syndrome in SP while drawing, no pushes
    lda #$dc            ; VIC $3400 matrix and $3000 font, both in ROMH
    sta vic+$18
    lda #8
    sta vic+$16
    lda #$1b
    sta vic+$11
    ldx #0
    lda #15
-   .for k in range(4)
    sta color+k*256,x
    .endfor
    inx
    bne -
    .for i in range(8)
    tsx
    txa
    and #(128 >> i)
    beq +
    ldx #23
    lda #10
-   sta color+(6+i)*40+8,x
    dex
    bpl -
+   .endfor
    tsx
    txa
    ldx #0
-   inx
    asl
    bcc -
    txs
flash_series:
    tsx
flash_one:
    lda #$85            ; flash the cartridge LED as a second output
    sta mode
    lda #1
    sta vic+$21
    sta vic+$20
    ldy #100
-   lda #255
-   sec
    sbc #1
    bne -
    dey
    bne --
    lda #5
    sta mode
    lda #0
    sta vic+$21
    sta vic+$20
    ldy #100
-   lda #255
-   sec
    sbc #1
    bne -
    dey
    bne --
    dex
    bne flash_one
    ldx #3
-   ldy #200
-   lda #255
-   sec
    sbc #1
    bne -
    dey
    bne --
    dex
    bne ---
    jmp flash_series
; Nine flashes distinguish the sparse address probe from the D7..D0
; codes. X identifies the first conflicting probe. The CPU may still see
; good low RAM here even when the VIC sees a completely different bank.
address_report:
    txs
    lda #0
    sta vic+$15
    sta vic+$21
    lda #$fc            ; $3c00 matrix, $3000 character set, both ROMH
    sta vic+$18
    ldx #0
    lda #15
-   .for k in range(4)
    sta color+k*256,x
    .endfor
    inx
    bne -
    ; Unrolled absolute stores keep the reporting path independent of RAM.
    tsx
    .for i in range(17)
    cpx #i
    bne +
    ldx #20
    lda #10
-   sta color+(5+i)*40+8,x
    dex
    bpl -
    jmp address_lit
+   .endfor
address_lit:
    ldx #9
    txs
    jmp flash_series
irq_entry:
    pha
    txa
    pha
    lda cia1+13
    and #$81
    sta irq_seen
    tsx
    lda $0103,x
    ora #4
    sta $0103,x
    pla
    tax
    pla
    rti
nmi_entry:
    pha
    lda cia2+13
    and #$81
    sta nmi_seen
    pla
    rti

.fill $f000-*,$ff
repair_font .block
    .include "font.asm"
    .endblock
.fill $f400-*,$ff
.enc "panel"
    #row ""
    #row "        LIOTRI C64 HARDWARE TEST"
    #row ""
    #row "          STARTUP RAM FAULT"
    #row ""
    #row "        BIT   250407   FLASHES"
    #row "         D7     U12       1"
    #row "         D6     U24       2"
    #row "         D5     U11       3"
    #row "         D4     U23       4"
    #row "         D3     U10       5"
    #row "         D2     U22       6"
    #row "         D1      U9       7"
    #row "         D0     U21       8"
    #row ""
    #row "    HIGHLIGHT = DIFFERING DATA BIT"
    #row "    COUNT FLASHES FOR FIRST BIT"
    #row ""
    #row "    CHECK DRAM AND ITS BUS PATH"
    #row ""
    #row "    RESET TO TEST AGAIN"
    .fill 4*40,32
; The final 24 bytes of this VIC matrix are off-screen, leaving the vectors free.
.fill $fc00-*,$ff
    #row "        LIOTRI C64 HARDWARE TEST"
    #row ""
    #row "        ADDRESS PROBE FAILED"
    #row "        BASE $37B7 / NEIGHBOURS"
    #row ""
    #row "          A0        $37B6"
    #row "          A1        $37B5"
    #row "          A2        $37B3"
    #row "          A3        $37BF"
    #row "          A4        $37A7"
    #row "          A5        $3797"
    #row "          A6        $37F7"
    #row "          A7        $3737"
    #row "          A8        $36B7"
    #row "          A9        $35B7"
    #row "          A10       $33B7"
    #row "          A11       $3FB7"
    #row "          A12       $27B7"
    #row "          A13       $17B7"
    #row "          A14       $77B7"
    #row "          A15       $B7B7"
    #row "          BASE      $37B7"
    #row ""
    #row "    CHECK ADDRESS MUX AND DRAM PATH"
    #row "    NINE FLASHES / RESET TO RETEST"
.fill $fffa-*,$ff
.word nmi_entry,cold_start,irq_entry
.here
