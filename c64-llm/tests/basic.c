/* Check the text loader against BASIC's ROM tokenizer, then execute a program.
 * Reuse the frontend without opening SDL or an audio device.
 */
#define main frontend_main
#include "../c64-llm.c"
#undef main

static c64_t sys;
static uint8_t fb[_C64_SCREEN_WIDTH * _C64_SCREEN_HEIGHT * 3];

static void check_tokens(const char *source) {
    static uint8_t ram[65536];
    char text[512];
    assert(strlen(source) < sizeof(text));
    strcpy(text, source);
    assert(basic_petscii(text));
    assert(strlen(text) <= 250);
    uint8_t tokens[251];
    int len = basic_tokenize(text, tokens);
    assert(len >= 0);

    /* Run $a57c in isolation. Stop at its RTS, without needing a caller. */
    memset(ram, 0, sizeof(ram));
    memcpy(ram + 0xa000, dump_c64_basic_bin, sizeof(dump_c64_basic_bin));
    memcpy(ram + 0x200, text, strlen(text) + 1);
    ram[0x7b] = 2;
    ram[0xfffc] = 0x7c;
    ram[0xfffd] = 0xa5;
    m6502_t cpu;
    uint64_t pins = m6502_init(&cpu, &(m6502_desc_t){0});
    for (int i = 0; i < 5000000; i++) {
        pins = m6502_tick(&cpu, pins);
        uint16_t addr = M6502_GET_ADDR(pins);
        if (pins & M6502_RW) {
            M6502_SET_DATA(pins, ram[addr]);
        } else {
            ram[addr] = M6502_GET_DATA(pins);
        }
        if ((pins & M6502_SYNC) && addr == 0xa612) {
            if (memcmp(ram + 0x200, tokens, len + 1)) {
                fprintf(stderr, "ROM tokenization differs: %s\n", source);
                assert(0);
            }
            return;
        }
    }
    fprintf(stderr, "ROM tokenizer timed out: %s\n", source);
    assert(!"ROM tokenizer did not return");
}

static int load_text(const char *source, size_t len) {
    char path[] = "/tmp/c64-basic-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    assert(file);
    assert(fwrite(source, 1, len, file) == len);
    assert(fclose(file) == 0);
    int ok = load_basic_file(&sys, path);
    assert(unlink(path) == 0);
    return ok;
}

static void check_listing(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    char *line = NULL;
    size_t capacity = 0;
    unsigned lines = 0;
    while (getline(&line, &capacity, file) >= 0) {
        line[strcspn(line, "\r\n")] = 0;
        char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        if (!*text) continue;
        assert(isdigit((unsigned char)*text));
        strtoul(text, &text, 10);
        while (*text == ' ' || *text == '\t') text++;
        check_tokens(text);
        lines++;
    }
    assert(feof(file));
    fclose(file);
    free(line);
    assert(load_basic_file(&sys, path));
    printf("%s: %u lines match the ROM\n", path, lines);
}

int main(int argc, char **argv) {
    assert(sizeof(basic_tokens)/sizeof(*basic_tokens) == 76);
    for (unsigned i = 0; i < 76; i++) check_tokens(basic_tokens[i]);
    static const char *cases[] = {
        "fori=1to9:nexti", "?\"PRINT:GOTO\":PRINT#1,A$", "GO TO 100",
        "DATA PRINT,\"REM:IF\",FOR,-2:READA$,B$,C$,D:REM:?DATA",
        "DEF FNF(X)=X^2", "A=1E-5:IFA<>2ANDNOTBTHEN100",
        "PRINT{pi}", "PRINT\"{clear}{147}{white}ABC\"",
        "G{$cf}10:R{$c5}", "?\"UNCLOSED", "REM \"GOTO", "DATA{193}:?1",
        "INPUT#1,A$:GET#1,B$:CMD1:OPEN1,8,2,\"NAME\":CLOSE1",
        "IFA$=\"DATA\"THEN?MID$(A$,2,2);SPC(3);TAB(9):GO TO100",
        "ONXGOSUB10,20,30:WAIT53265,128,128:POKE1024,PEEK(1025)"
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); i++) check_tokens(cases[i]);
    char longline[252];
    memset(longline, 'Z', 250);
    longline[250] = 0;
    check_tokens(longline);
    longline[250] = 'Z';
    longline[251] = 0;
    uint8_t tokens[251];
    assert(basic_tokenize(longline, tokens) == -1);

    c64_desc_t desc = {0};
    desc.roms.chars = (chips_range_t){dump_c64_char_bin, sizeof(dump_c64_char_bin)};
    desc.roms.basic = (chips_range_t){dump_c64_basic_bin, sizeof(dump_c64_basic_bin)};
    desc.roms.kernal = (chips_range_t){dump_c64_kernalv3_bin, sizeof(dump_c64_kernalv3_bin)};
    desc.crt_set_pixel = crt_set_pixel;
    desc.crt_set_pixel_fb = fb;
    c64_init(&sys, &desc);
    assert(llm_wait_basic_ready(&sys, NULL, 600));

    /* Out-of-order lines, replacement, deletion, BOM, CRLF and no final newline. */
    const char *source = "\xef\xbb\xbf 30 end\r\n10 print\"OLD\"\n20 rem delete me\n"
                         "10 ?\"NEW\"\n20\n\n0 rem first\n63999 rem last";
    assert(load_text(source, strlen(source)));
    static const unsigned numbers[] = {0, 10, 30, 63999};
    uint16_t addr = 0x0801;
    for (unsigned i = 0; i < sizeof(numbers)/sizeof(*numbers); i++) {
        assert(mem_rd16(&sys.mem_cpu, addr + 2) == numbers[i]);
        uint16_t next = mem_rd16(&sys.mem_cpu, addr);
        assert(next == addr + 5 + strlen((char *)sys.ram + addr + 4));
        addr = next;
    }
    assert(mem_rd16(&sys.mem_cpu, addr) == 0);
    assert(mem_rd16(&sys.mem_cpu, 0x2d) == addr + 2);

    uint8_t saved[128];
    memcpy(saved, sys.ram + 0x0801, sizeof(saved));
    static const char *invalid[] = {"PRINT 1\n", "64000 END\n", "10 ?\"{oops}\"\n", "10 ?\"{0}\"\n"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
        assert(!load_text(invalid[i], strlen(invalid[i])));
        assert(!memcmp(saved, sys.ram + 0x0801, sizeof(saved)));
    }
    assert(!load_text("10 END\0ignored", 14));
    assert(!memcmp(saved, sys.ram + 0x0801, sizeof(saved)));
    char oversized[50000];
    size_t used = 0;
    for (unsigned i = 0; i < 1000; i++) {
        used += snprintf(oversized + used, sizeof(oversized) - used,
                         "%u REM 0123456789012345678901234567890123456789\n", i);
    }
    assert(!load_text(oversized, used));
    assert(!memcmp(saved, sys.ram + 0x0801, sizeof(saved)));

    source = "10 dim a(3):def fnf(x)=x^2\n"
             "20 data 2,3,4,\"GOTO:REM\":fori=1to3:reada(i):next:readb$\n"
             "30 restore:readx:ifx<>2orlen(b$)<>8then200\n"
             "40 s=0:fori=3to1step-1:s=s+fnf(a(i)):next\n"
             "50 ifs<>29orleft$(b$,4)<>\"GOTO\"then200\n"
             "60 ifmid$(b$,6)<>\"REM\"orright$(b$,3)<>\"REM\"then200\n"
             "70 ifabs(sin({pi}/2)-1)>1e-5then200\n"
             "80 ifval(str$(s))<>29orasc(chr$(65))<>65then200\n"
             "90 on2gosub200,150:go to100\n"
             "100 ifpeek(49153)<>7then200\n"
             "110 poke49152,1:end\n150 poke49153,7:return\n200 poke49152,255:end\n";
    assert(load_text(source, strlen(source)));
    c64_basic_run(&sys);
    for (int frame = 0; frame < 1000 && !sys.ram[49152]; frame++) c64_exec(&sys, FRAME_USEC);
    assert(sys.ram[49152] == 1);

    for (int i = 1; i < argc; i++) check_listing(argv[i]);
    puts("BASIC loader tests passed");
    return 0;
}
