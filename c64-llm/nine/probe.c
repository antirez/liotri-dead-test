/* Deterministic Nine investigation harness. Build/run instructions in investigation.md.
 * Reuse the frontend's ROMs, boot, loader and PNG writer without starting SDL.
 */
#define main c64_llm_frontend_main
#include "../c64-llm.c"

/* The main macro also renames the border unit's main member. */
static bool probe_main_border(const m6569_t *vic) {
    return vic->brd.main;
}
#undef main

static unsigned probe_dma(const m6569_t *vic) {
    unsigned mask = 0;
    for (int i = 0; i < 8; i++) mask |= vic->sunit.dma_enabled[i] << i;
    return mask;
}

static bool probe_sample(int frame) {
    return frame % 500 == 0 || frame == 3800;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "Usage: %s PRG OUTPUT_PREFIX [FRAMES [basic|direct]]\n", argv[0]);
        return 1;
    }
    long frames = 6000;
    if (argc >= 4) {
        char *end;
        frames = strtol(argv[3], &end, 10);
        if (!*argv[3] || *end || frames < 1 || frames > 100000) {
            fprintf(stderr, "FRAMES must be between 1 and 100000\n");
            return 1;
        }
    }
    const char *mode = argc == 5 ? argv[4] : "basic";
    bool direct = !strcmp(mode, "direct");
    if (!direct && strcmp(mode, "basic")) {
        fprintf(stderr, "Unknown launch mode: %s\n", mode);
        return 1;
    }
    if (strlen(argv[2]) > 900) {
        fprintf(stderr, "OUTPUT_PREFIX is too long\n");
        return 1;
    }

    static c64_t sys;
    static uint8_t fb[_C64_SCREEN_WIDTH * _C64_SCREEN_HEIGHT * 3];
    c64_desc_t desc = {0};
    desc.roms.chars = (chips_range_t){dump_c64_char_bin, sizeof(dump_c64_char_bin)};
    desc.roms.basic = (chips_range_t){dump_c64_basic_bin, sizeof(dump_c64_basic_bin)};
    desc.roms.kernal = (chips_range_t){dump_c64_kernalv3_bin, sizeof(dump_c64_kernalv3_bin)};
    desc.crt_set_pixel = crt_set_pixel;
    desc.crt_set_pixel_fb = fb;
    c64_init(&sys, &desc);
    if (!llm_wait_basic_ready(&sys, NULL, 600)) {
        fprintf(stderr, "Boot did not reach BASIC READY\n");
        return 1;
    }
    uint16_t start, entry;
    if (!load_prg_file(&sys, argv[1], &start)) {
        fprintf(stderr, "Cannot load %s\n", argv[1]);
        return 1;
    }
    if (direct) {
        entry = start;
        llm_parse_basic_sys_start(&sys, start, &entry);
        llm_start_machine_code(&sys, entry);
    } else {
        /* Match the SDL --run path, including BASIC's SYS calling context. */
        c64_basic_run(&sys);
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s.csv", argv[2]);
    FILE *trace = fopen(path, "w");
    if (!trace) {
        perror(path);
        return 1;
    }
    fprintf(trace, "frame,line,cycle,pc,addr,data,vert,main,display,mc0,base0,expand0,dma,base0_before,expand0_before\n");
    for (int frame = 1; frame <= frames; frame++) {
        /* Exactly 63 * 312 PAL cycles, unlike FRAME's rounded 20 ms.
         * Blocks start at the boot's raster phase, not at vertical sync.
         */
        for (int tick = 0; tick < 63 * 312; tick++) {
            m6569_sprite_unit_t before = sys.vic.sunit;
            sys.pins = _c64_tick(&sys, sys.pins);
            uint16_t addr = M6502_GET_ADDR(sys.pins);
            uint8_t data = M6502_GET_DATA(sys.pins);
            bool write = sys.io_mapped && !(sys.pins & M6502_RW);

            if (write && (addr == 0xd017 ||
                (probe_sample(frame) && addr >= 0xd000 && addr <= 0xd02e))) {
                fprintf(trace, "%d,%d,%d,%04x,%04x,%02x,%d,%d,%d,%u,%u,%d,%02x,%u,%d\n",
                    frame, sys.vic.rs.v_count, sys.vic.rs.h_count, sys.cpu.PC,
                    addr, data, sys.vic.brd.vert, probe_main_border(&sys.vic),
                    sys.vic.rs.display_state, sys.vic.sunit.mc[0], sys.vic.sunit.mc_base[0],
                    sys.vic.sunit.expand[0], probe_dma(&sys.vic), before.mc_base[0], before.expand[0]);
            }
        }
        if (probe_sample(frame) || frame == frames) {
            snprintf(path, sizeof(path), "%s-%04d.png", argv[2], frame);
            if (!llm_write_screenshot_png(path, fb, _C64_SCREEN_WIDTH,
                                         0, 0, _C64_SCREEN_WIDTH, _C64_SCREEN_HEIGHT)) {
                fprintf(stderr, "Cannot write %s\n", path);
                fclose(trace);
                return 1;
            }
            printf("block=%d pc=$%04x d011=$%02x d016=$%02x\n",
                frame, sys.cpu.PC, sys.vic.reg.ctrl_1, sys.vic.reg.ctrl_2);
            fflush(stdout);
        }
    }
    int failed = ferror(trace);
    if (fclose(trace)) failed = 1;
    if (failed) {
        fprintf(stderr, "Cannot finish trace output\n");
        return 1;
    }
    return 0;
}
