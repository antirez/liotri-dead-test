/* IRQ timing while RDY holds a read cycle. No ROMs or SDL required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CHIPS_IMPL
#include "../m6502.h"

static uint8_t ram[65536];

static uint64_t tick(m6502_t *cpu, uint64_t pins) {
    pins = m6502_tick(cpu, pins);
    uint16_t addr = M6502_GET_ADDR(pins);
    if (pins & M6502_RW) {
        M6502_SET_DATA(pins, ram[addr]);
    } else {
        ram[addr] = M6502_GET_DATA(pins);
    }
    return pins;
}

static void test_rdy_irq(unsigned stall, unsigned irq, unsigned entry) {
    m6502_t cpu;
    uint64_t pins = m6502_init(&cpu, &(m6502_desc_t){0});
    memset(ram, 0xea, sizeof(ram));
    /* CLI; NOP; loop: LDA $15; BMI loop */
    static const uint8_t program[] = {0x58, 0xea, 0xa5, 0x15, 0x30, 0xfc};
    memcpy(ram + 0x2000, program, sizeof(program));
    ram[0x15] = 0xff;
    ram[0xfffc] = ram[0xfffe] = 0;
    ram[0xfffd] = 0x20;
    ram[0xffff] = 0x30;
    for (int i = 0; i < 7; i++) pins = tick(&cpu, pins);
    assert((pins & M6502_SYNC) && M6502_GET_ADDR(pins) == 0x2000);

    for (unsigned cycle = 0; cycle <= entry; cycle++) {
        pins = tick(&cpu, pins);
        bool handler = (pins & M6502_SYNC) && M6502_GET_ADDR(pins) == 0x3000;
        assert(handler == (cycle == entry));
        pins &= ~(M6502_RDY | M6502_IRQ);
        if (cycle >= stall && cycle < stall + 12) pins |= M6502_RDY;
        if (cycle >= irq) pins |= M6502_IRQ;
    }
}

int main(void) {
    /* Cycle counts checked against the perfect6502 transistor simulation.
     * IRQ during the held BMI fetch must let the branch finish first.
     * An earlier IRQ, or one during LDA's data read, interrupts before BMI.
     */
    test_rdy_irq(6, 8, 28);
    test_rdy_irq(6, 4, 25);
    test_rdy_irq(5, 7, 25);
    puts("CPU timing tests passed");
    return 0;
}
