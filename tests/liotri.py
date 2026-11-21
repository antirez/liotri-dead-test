#!/usr/bin/env python3
"""Run the cartridge on real simulated pins, check its video and flash reports."""
import concurrent.futures
import os
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CART = ROOT / "liotri.crt"
BIN = os.environ.get("C64_HW_SIM", "c64-hw-sim")


def run(frames, *args, cart=CART, keys="", simulator=BIN):
    command = [simulator, "--headless", str(frames), "--text", *args, cart]
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          check=True, env={**os.environ, "C64HW_TYPE": keys}).stdout


def expect(report, *patterns):
    for pattern in patterns:
        if not re.search(pattern, report):
            raise AssertionError(f"Missing {pattern!r}\n{report}")


def quick(work):
    # An empty character socket from power-on. This is before the ROM CRCs
    # finish. The sprite/background test must still have a known target.
    shot = work / "quick.png"
    run(25, "--fault", "U5:removed", "--shot", shot)
    report, _ = repair_screen(shot)
    expect(report, "S:SOUND ON", "A:SPRITES ON", r"ALU\+ MAP\+ VIC IRQ\+")
    assert "CHECKS OK" not in report
    for label in ("LOW 4K", "ADDRESS BUS", "COLOR RAM", "VIC RASTER",
                  "CIA1 A/B", "CIA2 A/B", "CIA1 IRQ", "CIA2 NMI",
                  "SID OSC/ENV", "VIC SPRITES"):
        expect(report, label + r" +OK")


def healthy():
    report = run(480)
    expect(report, r"SID / OSC3 +CHECKS OK +PASS", r"PASS 01 +FIRST \$---- +BITS --",
           "NO FAULTS FOUND", "RAM TESTS REPEAT UNTIL RESET", "TO CATCH INTERMITTENT FAULTS",
           "S:SOUND ON", "A:SPRITES ON")
    for label in ("CHAR ROM", "BASIC ROM", "KERNAL ROM", "CIA1 TOD", "CIA2 TOD"):
        expect(report, label + r" +OK")
    expect(report, r"USER LOOP +OPEN", r"VIC BANK +OK")


def march():
    # A third pass requires completing both the address and March C- sweeps.
    report = run(1500, keys="sa")
    expect(report, r"PASS 03 +FIRST \$---- +BITS --", r"MEMORY SWEEP +CHECKS OK",
           "S:SOUND OFF", "A:SPRITES OFF")


def shortcuts():
    # Pressing S twice must unmute even if A was pressed between the two.
    report = run(205, keys="sas")
    expect(report, "S:SOUND ON ", "A:SPRITES OFF", r"VIC SPRITES +OK", r"SID OSC/ENV +OK")
    report = run(480, keys="saa")
    expect(report, "S:SOUND OFF", "A:SPRITES ON", "NO FAULTS FOUND")


def peripherals():
    report = run(155, "--fault", "U6.17:open", "--fault", "U18:removed",
                 "--fault", "R100:open", "--fault", "U3:removed", "--fault", "U4:removed")
    expect(report, r"COLOR RAM +BAD", r"SID OSC/ENV +BAD", r"CIA1 TOD +BAD",
           r"CIA2 TOD +BAD", r"BASIC ROM +DIFF", r"KERNAL ROM +DIFF", r"PASS 01")


def interrupts():
    # The counters and ICR can pass with the physical interrupt outputs lifted.
    report = run(40, "--fault", "U1.21:open", "--fault", "U2.21:open")
    expect(report, r"CIA1 IRQ +BAD", r"CIA2 NMI +BAD", r"CIA1 A/B +OK", r"CIA2 A/B +OK")


def banking(work):
    # Open each U7 banking pin, then the same path at the PLA. An early FAIL
    # must remain readable even when CHAREN is held low and I/O disappears.
    for fault, setting, window in [
        ("U7.29:open", "36", "8K CART / RAM"),
        ("U7.28:open", "35", "8K CART / RAM"),
        ("U7.27:open", "33", "VIC $D000/$D040"),
        ("U7.27:gnd", "37", "VIC $D000/$D040"),
        ("U17.8:open", "36", "8K CART / RAM"),
    ]:
        shot = work / f"bank-{fault.split(':')[0]}-{setting}.png"
        run(25, "--fault", fault, "--shot", shot)
        display, _ = repair_screen(shot)
        expect(display, re.escape("BANK MAP FAILED  $01=$" + setting), re.escape(window),
               "CHECK 6510 PORT / PLA / DECODE")


def sid_socket():
    for fault in ("U18:removed", "U18.8:vcc"):
        report = run(58, "--fault", fault)
        expect(report, r"SID OSC/ENV +BAD", r"VIC SPRITES +OK", r"ALU\+ MAP\+")


def irq_routes():
    for fault, vic, cia1, cia2 in [
        ("U19.8:open", "!", "OK", "OK"),
        ("U7.3:vcc", "!", "BAD", "OK"),
        ("U7.3:gnd", "!", "BAD", "OK"),
        ("U7.4:vcc", "+", "OK", "BAD"),
    ]:
        report = run(52, "--fault", fault)
        expect(report, "VIC IRQ" + re.escape(vic), r"VIC RASTER +OK",
               "CIA1 IRQ +" + cia1, "CIA2 NMI +" + cia2, r"CIA1 A/B +OK")


def vic_banks():
    for fault in ("U2.2:vcc", "U14.2:open"):
        report = run(465, "--fault", fault)
        expect(report, r"VIC BANK +BAD", r"VIC SPRITES +OK", r"VIC RASTER +OK",
               r"CIA2 A/B +OK", r"MEMORY SWEEP +FAULT")
        assert "CHECKS OK" not in report


def decimal_disabled():
    # Optional alternate CPU core. This is exactly the release cartridge.
    report = run(465, simulator=os.environ["C64_HW_SIM_NO_BCD"])
    expect(report, r"ALU! MAP\+ VIC IRQ\+", r"MEMORY SWEEP +FAULT", r"VIC BANK +OK")
    assert "NO FAULTS FOUND" not in report


def harness():
    expect(run(40, "--harness"), r"USER LOOP +OK", r"KEY \+  JOY \+  POT \+  IEC \+  TAPE \+")
    report = run(40, "--harness", "--fault", "CN2.C:open", "--fault", "CN1.13:open",
                 "--fault", "CN8.1:open", "--fault", "U28:removed", "--fault", "CN4.4:open",
                 "--fault", "CN3.F:open")
    expect(report, r"USER LOOP +BAD", "KEY !  JOY !  POT !  IEC !  TAPE !")


def flipped_byte(work):
    # The CLI's normal PRG loader writes physical DRAM at three seconds.
    # $d34a should contain $d3 XOR $4a XOR $5a. Flip exactly D2 and D0.
    prg = work / "retention.prg"
    prg.write_bytes(bytes([0x4a, 0xd3, 0xd3 ^ 0x4a ^ 0x5a ^ 5]))
    report = run(440, prg)
    expect(report, r"FIRST \$D34A +BITS 05", r"MEMORY SWEEP +RAM FAULT +FAULT")
    assert "NO FAULTS FOUND" not in report and "CHECKS OK" not in report


def repair_screen(path):
    # --text recognizes Commodore's font. For the cartridge's own repair
    # font, match the actual monitor pixels against glyphs in its ROMH.
    data = path.read_bytes()
    width, height = struct.unpack_from(">II", data, 16)
    assert (width, height) == (392, 272)
    offset = 8
    packed = bytearray()
    while offset < len(data):
        size, kind = struct.unpack_from(">I4s", data, offset)
        if kind == b"IDAT":
            packed.extend(data[offset+8:offset+8+size])
        offset += size + 12
    pixels = zlib.decompress(packed)
    stride = width * 4 + 1

    def pixel(x, y):
        pos = stride*y + 1 + x*4
        return pixels[pos:pos+3]

    romh = CART.read_bytes()[-8192:]
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 /$:=!+."
    glyphs = {romh[0x1000+8*(ord(ch) & 63):0x1008+8*(ord(ch) & 63)]: ch
              for ch in alphabet}
    bg = pixel(32, 36+4*8)
    rows, inks = [], []
    for row in range(25):
        line, colors = [], set()
        for col in range(40):
            glyph = bytearray()
            for y in range(8):
                bits = 0
                for x in range(8):
                    ink = pixel(32+8*col+x, 36+8*row+y)
                    bits = bits*2 + (ink != bg)
                    if ink != bg:
                        colors.add(ink)
                glyph.append(bits)
            # The dashboard shortcut bar uses reverse video.
            normal = bytes(glyph)
            inverse = bytes(value ^ 255 for value in glyph)
            line.append(glyphs.get(normal, glyphs.get(inverse, "?")))
        rows.append("".join(line))
        inks.append(colors)
    return "\n".join(rows), inks


def dead_ram(work):
    shot = work / "dead.png"
    report = run(215, "--flashes", "--fault", "U24:removed", "--fault", "U21:removed",
                 "--shot", shot)
    expect(report, r"flash series: 2 2(?: |\n)")
    text, inks = repair_screen(shot)
    expect(text, "STARTUP RAM FAULT", "D6 +U24", "D0 +U21")
    assert inks[7] == inks[13] and inks[7] != inks[6] and inks[6] == inks[8], inks[6:14]


def address_alias(work):
    # U13 pin 14 is the A12 column input on this ASSY. The VIC's ordinary
    # RAM fetches are scrambled too, so read the ROM-backed fault picture.
    shot = work / "alias.png"
    run(130, "--fault", "U13.14:open", "--shot", shot)
    text, inks = repair_screen(shot)
    expect(text, "ADDRESS PROBE FAILED", r"A12 +\$27B7", "NINE FLASHES")
    assert inks[17] != inks[16] and inks[16] == inks[18], inks[16:19]


def missing_roms(work):
    shot = work / "roms.png"
    run(440, "--fault", "U3:removed", "--fault", "U4:removed", "--fault", "U5:removed",
        "--shot", shot, keys="sa")
    text, _ = repair_screen(shot)
    expect(text, r"CHAR ROM +DIFF", r"BASIC ROM +DIFF", r"KERNAL ROM +DIFF",
           r"VIC SPRITES +OK", r"PASS 01", r"MEMORY SWEEP +FAULT",
           "S:SOUND OFF", "A:SPRITES OFF")
    assert "NO FAULTS FOUND" not in text and "CHECKS OK" not in text


def mapper(work):
    # Reset into a tiny probe, then boot the unmodified diagnostic once the
    # cartridge SRAM, bank 63, and both halves of a joined 16K CHIP respond.
    image = bytearray(CART.read_bytes())
    romh = len(image)-8192
    saved_reset = image[-4:-2]
    probe = bytes.fromhex("""
        a9 5a 8d a9 df a9 a5 8d 29 df ad a9 df c9 5a d0 fe
        ad 29 df c9 a5 d0 fe a9 3f 8d 00 de ad 00 80 c9 32 d0 fe
        ad 00 e0 c9 a6 d0 fe a9 00 8d 00 de 4c
    """) + saved_reset
    # Changing banks while executing from ROM needs matching instructions
    # in the new bank. Install the same stub at $fc00 on both sides.
    image[romh+0x1c00:romh+0x1c00+len(probe)] = probe
    image[-4:-2] = b"\x00\xfc"
    bank = bytearray([0xff])*16384
    bank[0], bank[8192] = 0x32, 0xa6
    bank[0x3c00:0x3c00+len(probe)] = probe
    image.extend(struct.pack(">4sIHHHH", b"CHIP", 16400, 2, 63, 0x8000, 16384))
    image.extend(bank)
    fixture = work / "mapper.crt"
    fixture.write_bytes(image)
    expect(run(40, cart=fixture), r"ADDRESS BUS +OK", r"VIC SPRITES +OK")


def main():
    with tempfile.TemporaryDirectory(prefix="liotri-") as temp:
        work = Path(temp)
        cases = [healthy, march, shortcuts, peripherals, interrupts, harness, irq_routes, vic_banks, sid_socket]
        if os.environ.get("C64_HW_SIM_NO_BCD"):
            cases.append(decimal_disabled)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = {pool.submit(case): case.__name__ for case in cases}
            for case in [quick, banking, flipped_byte, dead_ram, address_alias, missing_roms, mapper]:
                jobs[pool.submit(case, work)] = case.__name__
            for job in concurrent.futures.as_completed(jobs):
                job.result()
                print("ok   ", jobs[job], flush=True)


if __name__ == "__main__":
    main()
