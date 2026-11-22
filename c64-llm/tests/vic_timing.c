/* Focused PAL VIC regressions. No SDL or C64 ROMs required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CHIPS_IMPL
#include "../chips_common.h"
#include "../m6569.h"

static uint32_t pixels[272][392];
static uint8_t ram[16384];

static uint16_t fetch(uint16_t addr, void *user_data) {
    (void)user_data;
    return ram[addr & 0x3fff];
}

static void pixel(void *fb, int x, int y, uint32_t color) {
    (void)fb;
    assert(x >= 0 && x < 392 && y >= 0 && y < 272);
    pixels[y][x] = color;
}

static void init(m6569_t *vic) {
    memset(ram, 0, sizeof(ram));
    memset(pixels, 0xff, sizeof(pixels));
    m6569_init(vic, &(m6569_desc_t){
        .fetch_cb = fetch,
        .screen = {.x = 64, .y = 24, .width = 392, .height = 272},
        .crt_set_pixel = pixel,
    });
}

static uint64_t write_pins(uint8_t reg, uint8_t data) {
    uint64_t pins = M6569_CS | reg;
    M6569_SET_DATA(pins, data);
    return pins;
}

static void ticks(m6569_t *vic, unsigned count) {
    while (count--) m6569_tick(vic, M6569_RW);
}

static void test_raster_and_irq(void) {
    m6569_t vic;
    init(&vic);
    vic.rs.v_count = 42;
    vic.rs.h_count = 62;
    _m6569_write(&vic, write_pins(0x12, 43));
    _m6569_write(&vic, write_pins(0x1a, 1));
    uint64_t pins = m6569_tick(&vic, M6569_RW);
    assert(vic.rs.v_count == 42 && !(pins & M6569_IRQ));
    pins = m6569_tick(&vic, M6569_RW);
    assert(vic.rs.v_count == 43 && vic.rs.h_count == 1);
    assert(pins & M6569_IRQ);
}

static void test_crunch(void) {
    /* Known crunch transitions, including the $2d -> $35 MISC example. */
    static const uint8_t cases[][3] = {{0, 3, 1}, {3, 6, 7}, {6, 9, 5}, {0x2d, 0x30, 0x35}};
    m6569_t vic;
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        init(&vic);
        vic.rs.h_count = 14;
        vic.sunit.mc_base[0] = cases[i][0];
        vic.sunit.mc[0] = cases[i][1];
        vic.sunit.expand[0] = false;
        vic.reg.mye = 1;
        m6569_tick(&vic, write_pins(0x17, 0));
        assert(vic.sunit.mc[0] == cases[i][2]);
        ticks(&vic, 1);
        assert(vic.sunit.mc_base[0] == cases[i][2]);
    }

    /* The same write one cycle early must not crunch. */
    init(&vic);
    vic.rs.h_count = 13;
    vic.reg.mye = 1;
    vic.sunit.mc_base[0] = 6;
    vic.sunit.mc[0] = 9;
    m6569_tick(&vic, write_pins(0x17, 0));
    ticks(&vic, 2);
    assert(vic.sunit.mc_base[0] == 9);

    /* Nine's 6/9 -> 5 crunch must keep DMA alive through two wraps. */
    init(&vic);
    vic.rs.v_count = 12;
    vic.rs.h_count = 14;
    vic.reg.mye = 1;
    vic.sunit.dma_enabled[0] = true;
    vic.sunit.disp_enabled[0] = true;
    vic.sunit.mc_base[0] = 6;
    vic.sunit.mc[0] = 9;
    m6569_tick(&vic, write_pins(0x17, 0));
    ticks(&vic, 1);
    ticks(&vic, 61 * 63);
    assert(vic.sunit.dma_enabled[0]);
    ticks(&vic, 63);
    assert(vic.sunit.mc_base[0] == 63 && !vic.sunit.dma_enabled[0]);
    assert(vic.sunit.disp_enabled[0]);
    ticks(&vic, 42);
    assert(!vic.sunit.disp_enabled[0]);
}

static void test_sprite_start(void) {
    m6569_t vic;
    init(&vic);
    vic.rs.v_count = vic.reg.mxy[0][1] = 100;
    vic.rs.h_count = 54;
    vic.reg.me = 1;
    uint64_t pins = m6569_tick(&vic, M6569_RW);
    assert(vic.sunit.dma_enabled[0] && (pins & M6569_BA));

    init(&vic);
    vic.rs.v_count = vic.reg.mxy[0][1] = 100;
    vic.rs.h_count = 54;
    m6569_tick(&vic, write_pins(0x15, 1));
    assert(!vic.sunit.dma_enabled[0]);
    ticks(&vic, 1);
    assert(vic.sunit.dma_enabled[0]);
}

static void test_sprite_rows(void) {
    for (int expanded = 0; expanded <= 1; expanded++) {
        m6569_t vic;
        init(&vic);
        _m6569_write(&vic, write_pins(0x01, 40));
        _m6569_write(&vic, write_pins(0x15, 1));
        _m6569_write(&vic, write_pins(0x17, expanded));
        vic.rs.v_count = 40;
        vic.rs.h_count = 54;
        ticks(&vic, 25); // first row's cycle-16 counter update
        int rows = expanded ? 42 : 21;
        for (int row = 1; row <= rows; row++) {
            assert(vic.sunit.mc_base[0] == 3 * (row / (expanded + 1)));
            assert(vic.sunit.dma_enabled[0] == (row < rows));
            assert(vic.sunit.disp_enabled[0]);
            if (row < rows) ticks(&vic, 63);
        }
        ticks(&vic, 42);
        assert(!vic.sunit.disp_enabled[0]);
    }
}

static void test_wrapped_sprite(void) {
    m6569_t vic;
    init(&vic);
    _m6569_write(&vic, write_pins(0x00, 0xe0));
    _m6569_write(&vic, write_pins(0x10, 1));
    _m6569_write(&vic, write_pins(0x1d, 1));
    _m6569_write(&vic, write_pins(0x21, 6));
    vic.sunit.disp_enabled[0] = true;
    vic.sunit.shift[0] = 0xffffff00;
    vic.rs.h_count = 9;
    vic.rs.v_count = 100;
    vic.crt.x = 5;
    vic.crt.y = 40;
    ticks(&vic, 8);
    /* X=480 starts before the host crop, then covers its leftmost 32 pixels. */
    for (int x = 0; x < 32; x++) assert(pixels[16][x] == m6569_color(0));
    assert(pixels[16][32] == m6569_color(6));

    /* The PAL counter skips X=504..511: these must not alias X=0. */
    _m6569_write(&vic, write_pins(0x00, 0xf8));
    assert(vic.sunit.h_first[0] == 0xff);
}

static void test_border_write(void) {
    m6569_t vic;
    init(&vic);
    _m6569_write(&vic, write_pins(0x16, 8));
    vic.rs.v_count = 100;
    vic.rs.h_count = 54;
    ticks(&vic, 1);
    m6569_tick(&vic, write_pins(0x16, 0));
    assert(!vic.brd.main);

    init(&vic);
    _m6569_write(&vic, write_pins(0x16, 8));
    vic.rs.v_count = 100;
    vic.rs.h_count = 54;
    ticks(&vic, 2);
    assert(vic.brd.main);
}

int main(void) {
    test_raster_and_irq();
    test_crunch();
    test_sprite_start();
    test_sprite_rows();
    test_wrapped_sprite();
    test_border_write();
    puts("PAL VIC timing tests passed");
    return 0;
}
