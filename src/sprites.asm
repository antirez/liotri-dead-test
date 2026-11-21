; 24-bit sprite scan lines, in VIC byte order, 64-byte aligned in RAM.
shapes:
; Saturn. The near side of the ring cuts through the disc.
    .byte $00,$00,$00, $00,$3c,$00, $00,$ff,$00, $01,$ff,$80
    .byte $03,$fb,$c0, $07,$f7,$e0, $07,$ef,$ef, $0f,$df,$f3
    .byte $0f,$bd,$c7, $0f,$73,$1e, $1f,$cc,$f0, $78,$33,$e0
    .byte $c7,$cf,$e0, $ff,$bf,$c0, $03,$7f,$c0, $01,$ff,$80
    .byte $00,$ff,$00, $00,$3c,$00, $00,$00,$00, $00,$00,$00
    .byte $00,$00,$00, 0
; Solar observatory with twin gridded panels.
    .byte $00,$00,$00, $00,$02,$00, $00,$04,$00, $00,$08,$00
    .byte $00,$3c,$00, $00,$7e,$00, $ff,$ff,$ff, $89,$c3,$91
    .byte $ff,$bd,$ff, $89,$a5,$91, $ff,$a5,$ff, $89,$bd,$91
    .byte $ff,$c3,$ff, $00,$7e,$00, $00,$3c,$00, $00,$18,$00
    .byte $00,$18,$00, $00,$7e,$00, $00,$3c,$00, $00,$18,$00
    .byte $00,$00,$00, 0
; Delta-wing scout and exhaust.
    .byte $00,$18,$00, $00,$18,$00, $00,$3c,$00, $00,$3c,$00
    .byte $00,$7e,$00, $00,$66,$00, $00,$c3,$00, $00,$c3,$00
    .byte $01,$e7,$80, $03,$ff,$c0, $07,$ff,$e0, $0f,$bd,$f0
    .byte $1f,$bd,$f8, $3f,$3c,$fc, $7e,$3c,$7e, $78,$ff,$1e
    .byte $60,$66,$06, $00,$66,$00, $00,$24,$00, $00,$24,$00
    .byte $00,$00,$00, 0
; Navigation beacon.
    .byte $00,$18,$00, $00,$18,$00, $00,$18,$00, $01,$18,$80
    .byte $00,$bd,$00, $00,$7e,$00, $00,$ff,$00, $03,$c3,$c0
    .byte $0f,$99,$f0, $ff,$3c,$ff, $ff,$3c,$ff, $0f,$99,$f0
    .byte $03,$c3,$c0, $00,$ff,$00, $00,$7e,$00, $00,$bd,$00
    .byte $01,$18,$80, $00,$18,$00, $00,$18,$00, $00,$18,$00
    .byte $00,$00,$00, 0
