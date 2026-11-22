#!/usr/bin/env python3
"""Build an autostarting 8 KiB C64 DRAM-test cartridge (.crt).

No cross-assembler is required.  The small assembler below only emits the
instructions used by this ROM, keeping the cartridge source auditable.
"""
from pathlib import Path
import struct


ROM_BASE = 0x8000
ROM_SIZE = 0x2000
OUT = Path(__file__).resolve().parents[1] / "dram-test.crt"


class Asm:
    def __init__(self):
        self.data = bytearray()
        self.labels = {}
        self.fixups = []

    def label(self, name):
        self.labels[name] = ROM_BASE + len(self.data)

    def b(self, *values): self.data.extend(values)
    def abs(self, opcode, address):
        self.b(opcode, address & 0xff, address >> 8)
    def imm(self, opcode, value): self.b(opcode, value)
    def branch(self, opcode, label):
        self.b(opcode, 0); self.fixups.append((len(self.data) - 1, label, "rel"))
    def ref(self, opcode, label):
        self.b(opcode, 0, 0); self.fixups.append((len(self.data) - 2, label, "abs"))
    def finish(self):
        for pos, label, kind in self.fixups:
            target = self.labels[label]
            if kind == "abs":
                self.data[pos:pos + 2] = struct.pack("<H", target)
            else:
                delta = target - (ROM_BASE + pos + 1)
                if not -128 <= delta <= 127:
                    raise ValueError(f"branch to {label} is out of range")
                self.data[pos] = delta & 0xff
        return self.data


def screen_codes(text):
    out = []
    for c in text:
        if "A" <= c <= "Z": out.append(ord(c) - ord("A") + 1)
        else: out.append(ord(c))
    return out


def build_rom():
    a = Asm()
    # The KERNAL recognizes this signature on an 8 KiB generic cartridge.
    a.b(*b"CBM80")
    a.ref(0x4C, "start")                    # JMP start

    a.label("start")
    a.b(0x78)                                # SEI
    a.imm(0xA9, 0x00); a.abs(0x8D, 0xD020)  # black border
    a.abs(0x8D, 0xD021)                     # black background
    a.imm(0xA9, 0x20); a.imm(0xA2, 0x00)    # clear the 1 KiB screen RAM
    a.label("clear")
    a.abs(0x9D, 0x0400); a.abs(0x9D, 0x0500)
    a.abs(0x9D, 0x0600); a.abs(0x9D, 0x0700)
    a.b(0xE8); a.branch(0xD0, "clear")
    for i, c in enumerate(screen_codes("C64 DRAM TEST")):
        a.imm(0xA9, c); a.abs(0x8D, 0x0428 + i)
    for i, c in enumerate(screen_codes("TESTING $")):
        a.imm(0xA9, c); a.abs(0x8D, 0x0408 + i)
    a.imm(0xA9, 0x02); a.abs(0x85, 0xFB)    # test pointer = $0002
    a.imm(0xA9, 0x00); a.abs(0x85, 0xFC)
    a.imm(0xA9, 0xFF); a.abs(0x85, 0xFD)    # force first progress update
    a.abs(0x20, 0)                          # patched JSR display
    a.fixups.append((len(a.data) - 2, "display", "abs"))
    a.imm(0xA9, 0x30); a.abs(0x85, 0x01)    # RAM at BASIC/KERNAL/I/O addresses

    a.label("next")
    # $FB-$FD are the ROM's zero-page workspace.  $0400-$07FF is the live
    # display, deliberately reserved so the current address stays visible.
    a.abs(0xA5, 0xFC); a.branch(0xD0, "check_screen")
    a.abs(0xA5, 0xFB); a.imm(0xC9, 0xFB); a.branch(0x90, "check_screen")
    a.imm(0xA9, 0x00); a.abs(0x85, 0xFB); a.imm(0xA9, 0x01); a.abs(0x85, 0xFC)
    a.label("check_screen")
    a.abs(0xA5, 0xFC); a.imm(0xC9, 0x04); a.branch(0x90, "progress")
    a.imm(0xC9, 0x08); a.branch(0xB0, "progress")
    a.imm(0xA9, 0x00); a.abs(0x85, 0xFB); a.imm(0xA9, 0x08); a.abs(0x85, 0xFC)

    a.label("progress")
    a.abs(0xA5, 0xFC); a.abs(0xC5, 0xFD); a.branch(0xF0, "patterns")
    a.abs(0x85, 0xFD); a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "display", "abs"))
    a.label("patterns")
    a.imm(0xA0, 0x00); a.imm(0xA9, 0x55); a.b(0x91, 0xFB) # STA ($FB),Y
    a.b(0xD1, 0xFB); a.branch(0xD0, "failed")             # CMP ($FB),Y
    a.imm(0xA9, 0xAA); a.b(0x91, 0xFB); a.b(0xD1, 0xFB); a.branch(0xD0, "failed")
    a.b(0xE6, 0xFB); a.branch(0xD0, "next")              # INC ptr low
    a.b(0xE6, 0xFC); a.branch(0xD0, "next")              # INC ptr high
    a.label("passed")
    a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "pass_message", "abs"))
    a.label("halt"); a.ref(0x4C, "halt")

    a.label("failed")
    a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "fail_message", "abs"))
    a.ref(0x4C, "halt")

    # Show the pointer at screen $0411..$0414.  This routine is called only
    # between tested pages, so its stack use never masks a bad RAM byte.
    a.label("display")
    a.abs(0xA5, 0xFC); a.abs(0x85, 0xFE); a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "hex_hi", "abs"))
    a.abs(0xA5, 0xFC); a.abs(0x85, 0xFE); a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "hex_lo", "abs"))
    a.abs(0xA5, 0xFB); a.abs(0x85, 0xFE); a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "hex_hi2", "abs"))
    a.abs(0xA5, 0xFB); a.abs(0x85, 0xFE); a.abs(0x20, 0); a.fixups.append((len(a.data) - 2, "hex_lo2", "abs"))
    a.b(0x60)                                # RTS
    # A contains a saved byte in $FE; write its upper/lower nibble at fixed cells.
    def hex_routine(name, shifts, dest):
        a.label(name); a.abs(0xA5, 0xFE)
        for _ in range(shifts): a.b(0x4A)
        if shifts == 0: a.imm(0x29, 0x0f)
        a.b(0xAA); a.ref(0xBD, "hex_table"); a.abs(0x8D, dest); a.b(0x60)
    hex_routine("hex_hi", 4, 0x0411)
    hex_routine("hex_lo", 0, 0x0412)
    hex_routine("hex_hi2", 4, 0x0413)
    hex_routine("hex_lo2", 0, 0x0414)
    a.label("pass_message")
    for i, c in enumerate(screen_codes("PASS - DRAM OK")): a.imm(0xA9, c); a.abs(0x8D, 0x0440 + i)
    a.b(0x60)
    a.label("fail_message")
    for i, c in enumerate(screen_codes("FAIL AT $")): a.imm(0xA9, c); a.abs(0x8D, 0x0440 + i)
    a.b(0x60)
    a.label("hex_table"); a.b(*screen_codes("0123456789ABCDEF"))

    code = a.finish()
    rom = bytearray([0xff]) * ROM_SIZE
    rom[:len(code)] = code
    return rom


def main():
    rom = build_rom()
    header = bytearray(b"C64 CARTRIDGE   ")
    header += struct.pack(">IHHBB6s32s", 0x40, 0x0100, 0, 0, 1, b"\0" * 6,
                          b"C64 DRAM TEST".ljust(32, b"\0"))
    chip = b"CHIP" + struct.pack(">IHHHH", 0x2010, 0, 0, 0x8000, ROM_SIZE)
    OUT.write_bytes(header + chip + rom)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
