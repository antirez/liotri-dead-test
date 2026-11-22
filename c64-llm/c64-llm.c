/* c64-llm.c
 * Commodore 64 emulator front-end with:
 * - SDL interactive mode (default)
 * - --llm socket control/debug mode for automated development workflows
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include <SDL.h>

#if defined(__APPLE__) && defined(USE_MACOS_APPKIT_ACTIVATE)
extern void c64_llm_macos_activate_app(void);
#endif

/* Global configuration (mostly from command line options). */
struct {
    int sdl_mode;                   /* Render using SDL window (default). */
    int llm_mode;                   /* Socket-controlled debug mode. */
    int auto_run;                   /* SDL: fast boot and inject RUN for PRG. */
    int bas_mode;                   /* Load BASIC source and run at startup. */
    char llm_socket_path[108];      /* AF_UNIX socket path. */
    char *prg_filename;             /* Startup file: PRG or BASIC source. */
    float zoom;                     /* C64 display zoom level (SDL mode). */
} EmuConfig;

/* SDL keyboard -> virtual joystick (port 2) state. */
static uint8_t sdl_joy2_mask = 0;

#define C64_MIN_ZOOM 0.25
#define C64_MAX_ZOOM 10

/* PAL frame cadence for C64 gameplay timing. */
#define FRAME_USEC 20000
#define LLM_PROFILE_ADDR_COUNT 65536
#define LLM_HYBRID_DEBUG_SLICE_INSTR 12000
#define LLM_AUDIO_RING_SAMPLES 32768
#define LLM_CIA_TRACE_CAP 256

void llm_cia1_trace_hook(void *sys_ptr, uint16_t addr, uint64_t cia1_pins);
#define C64_CIA1_TRACE_HOOK(sys, addr, cia1_pins) \
    llm_cia1_trace_hook((void *)(sys), (addr), (cia1_pins))

typedef struct {
    uint16_t pc;
    uint16_t addr;
    uint8_t rw;
    uint8_t data;
    uint8_t irq_pin;
    uint16_t ta_counter;
    uint16_t ta_latch;
    uint8_t ta_cr;
    uint8_t intr_icr;
    uint8_t intr_imr;
} llm_cia_trace_entry_t;

static llm_cia_trace_entry_t llm_cia1_trace[LLM_CIA_TRACE_CAP];
static uint32_t llm_cia1_trace_head = 0;
static uint32_t llm_cia1_trace_count = 0;

static void llm_cia_trace_reset(void) {
    memset(llm_cia1_trace, 0, sizeof(llm_cia1_trace));
    llm_cia1_trace_head = 0;
    llm_cia1_trace_count = 0;
}

#define CHIPS_IMPL
#include "chips_common.h"
#include "m6502.h"
#include "m6526.h"
#include "m6569.h"
#include "m6581.h"
#include "beeper.h"
#include "kbd.h"
#include "mem.h"
#include "clk.h"
#include "c1530.h"
#include "m6522.h"
#include "c1541.h"
#include "c64.h"
#include "c64-roms.h"

#ifdef USE_AUDIO
void *audio_init(void);
void audio_from_emulator(const float *samples, int num_samples, void *user_data);
void audio_cleanup(void *user_data);

static float llm_audio_ring[LLM_AUDIO_RING_SAMPLES];
static uint32_t llm_audio_ring_pos = 0;
static uint32_t llm_audio_ring_count = 0;
static uint64_t llm_audio_total_samples = 0;

static void llm_audio_reset_metrics(void) {
    memset(llm_audio_ring, 0, sizeof(llm_audio_ring));
    llm_audio_ring_pos = 0;
    llm_audio_ring_count = 0;
    llm_audio_total_samples = 0;
}

static void llm_audio_capture_samples(const float *samples, int num_samples) {
    if (!samples || num_samples <= 0) return;
    for (int i = 0; i < num_samples; i++) {
        llm_audio_ring[llm_audio_ring_pos] = samples[i];
        llm_audio_ring_pos = (llm_audio_ring_pos + 1) % LLM_AUDIO_RING_SAMPLES;
        if (llm_audio_ring_count < LLM_AUDIO_RING_SAMPLES) llm_audio_ring_count++;
    }
    llm_audio_total_samples += (uint64_t)num_samples;
}

static void llm_audio_tap_from_emulator(const float *samples, int num_samples, void *user_data) {
    llm_audio_capture_samples(samples, num_samples);
    audio_from_emulator(samples, num_samples, user_data);
}
#endif

static uint64_t time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void llm_cia1_trace_record(c64_t *c64, uint16_t addr, uint8_t rw, uint8_t data, uint8_t irq_pin) {
    llm_cia_trace_entry_t *e = &llm_cia1_trace[llm_cia1_trace_head];
    e->pc = m6502_pc(&c64->cpu);
    e->addr = addr;
    e->rw = rw;
    e->data = data;
    e->irq_pin = irq_pin;
    e->ta_counter = c64->cia_1.ta.counter;
    e->ta_latch = c64->cia_1.ta.latch;
    e->ta_cr = c64->cia_1.ta.cr;
    e->intr_icr = c64->cia_1.intr.icr;
    e->intr_imr = c64->cia_1.intr.imr;
    llm_cia1_trace_head = (llm_cia1_trace_head + 1) % LLM_CIA_TRACE_CAP;
    if (llm_cia1_trace_count < LLM_CIA_TRACE_CAP) llm_cia1_trace_count++;
}

void llm_cia1_trace_hook(void *sys_ptr, uint16_t addr, uint64_t cia1_pins) {
    c64_t *c64 = (c64_t *)sys_ptr;
    llm_cia1_trace_record(c64, addr, (cia1_pins & M6526_RW) ? 1 : 0, M6526_GET_DATA(cia1_pins),
                          (cia1_pins & M6526_IRQ) ? 1 : 0);
}

static void crt_set_pixel(void *fbptr, int x, int y, uint32_t color) {
    uint8_t *fb = fbptr;
    if (x < 0 || x >= _C64_SCREEN_WIDTH || y < 0 || y >= _C64_SCREEN_HEIGHT)
        return;

    uint8_t *dst = fb + (y * _C64_SCREEN_WIDTH + x) * 3;
    dst[0] = color & 0xff;
    dst[1] = (color >> 8) & 0xff;
    dst[2] = (color >> 16) & 0xff;
}

/* Load a PRG file in C64 RAM via c64_quickload(). */
static int load_prg_file(c64_t *sys, const char *filename, uint16_t *start_addr) {
    FILE *file = fopen(filename, "rb");
    uint8_t *buffer = NULL;
    size_t file_size = 0;
    int ok = 0;

    if (!file) {
        perror("fopen");
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) goto cleanup;
    file_size = (size_t)ftell(file);
    if (file_size < 2) goto cleanup;
    if (fseek(file, 0, SEEK_SET) != 0) goto cleanup;

    buffer = malloc(file_size);
    if (!buffer) goto cleanup;

    if (fread(buffer, 1, file_size, file) != file_size) goto cleanup;

    chips_range_t prg_data = {
        .ptr = buffer,
        .size = file_size,
    };

    if (!c64_quickload(sys, prg_data)) goto cleanup;

    if (start_addr)
        *start_addr = (uint16_t)((buffer[1] << 8) | buffer[0]);
    ok = 1;

cleanup:
    if (file) fclose(file);
    if (buffer) free(buffer);
    return ok;
}

/* BASIC V2 tokens, in ROM order ($80..$cb). Order matters: PRINT# before PRINT. */
static const char *basic_tokens[] = {
    "END", "FOR", "NEXT", "DATA", "INPUT#", "INPUT", "DIM", "READ",
    "LET", "GOTO", "RUN", "IF", "RESTORE", "GOSUB", "RETURN", "REM",
    "STOP", "ON", "WAIT", "LOAD", "SAVE", "VERIFY", "DEF", "POKE",
    "PRINT#", "PRINT", "CONT", "LIST", "CLR", "CMD", "SYS", "OPEN",
    "CLOSE", "GET", "NEW", "TAB(", "TO", "FN", "SPC(", "THEN",
    "NOT", "STEP", "+", "-", "*", "/", "^", "AND", "OR", ">", "=", "<",
    "SGN", "INT", "ABS", "USR", "FRE", "POS", "SQR", "RND", "LOG", "EXP",
    "COS", "SIN", "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$",
    "LEFT$", "RIGHT$", "MID$", "GO"
};

/* Convert ASCII listings to the C64's uppercase/graphics character set.
 * Brace escapes provide control and graphics characters without binary source.
 */
static int basic_petscii(char *text) {
    static const struct { const char *name; uint8_t code; } escapes[] = {
        {"clear", 147}, {"clr", 147}, {"home", 19}, {"return", 13},
        {"down", 17}, {"up", 145}, {"right", 29}, {"left", 157},
        {"delete", 20}, {"insert", 148}, {"rvon", 18}, {"rvof", 146},
        {"black", 144}, {"white", 5}, {"red", 28}, {"cyan", 159},
        {"purple", 156}, {"green", 30}, {"blue", 31}, {"yellow", 158},
        {"orange", 129}, {"brown", 149}, {"pink", 150},
        {"dark gray", 151}, {"gray", 152}, {"light green", 153},
        {"light blue", 154}, {"light gray", 155}, {"pi", 255}
    };
    char *out = text;
    while (*text) {
        unsigned char c = *text++;
        if (c == '{') {
            char *end = strchr(text, '}');
            if (!end) return 0;
            size_t len = end - text;
            unsigned value = 0;
            for (size_t i = 0; i < sizeof(escapes)/sizeof(*escapes); i++) {
                if (strlen(escapes[i].name) == len && !strncasecmp(text, escapes[i].name, len)) {
                    value = escapes[i].code;
                    break;
                }
            }
            if (!value) {
                char *tail;
                int base = *text == '$' ? 16 : 10;
                char *number = text + (base == 16);
                if (!isxdigit((unsigned char)*number)) return 0;
                unsigned long n = strtoul(number, &tail, base);
                if (tail != end || n < 1 || n > 255) return 0;
                value = n;
            }
            c = value;
            text = end + 1;
        } else if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        } else if (c == '\t') {
            c = ' ';
        } else if (c < 32 || c > 126) {
            return 0;
        }
        *out++ = c;
    }
    *out = 0;
    return 1;
}

static int basic_tokenize(const char *text, uint8_t *out) {
    int quoted = 0, data = 0, rem = 0, len = 0;
    while (*text) {
        uint8_t c = *text++;
        if (!quoted && !rem && c >= 0x80 && c != 0xff) continue;
        if (!quoted && !data && !rem) {
            if (c == '?') {
                c = 0x99;
            } else if (c < 0x80) {
                const char *start = text - 1;
                for (size_t i = 0; i < sizeof(basic_tokens)/sizeof(*basic_tokens); i++) {
                    const char *word = basic_tokens[i];
                    for (size_t n = 0; word[n]; n++) {
                        /* The ROM marks the final letter with bit 7. A shifted
                         * letter earlier in the word is also an abbreviation.
                         */
                        uint8_t key = word[n] | (word[n+1] ? 0 : 0x80);
                        uint8_t ch = start[n];
                        if ((uint8_t)(ch - key) == 0x80) {
                            c = 0x80 + i;
                            text = start + n + 1;
                            break;
                        }
                        if (ch != key) break;
                    }
                    if (c >= 0x80) break;
                }
            }
            if (c == 0x83) data = 1;
            if (c == 0x8f) rem = 1;
        }
        if (c == '"' && !rem) quoted = !quoted;
        if (c == ':' && !quoted) data = 0;
        if (len == 250) return -1;
        out[len++] = c;
    }
    out[len] = 0;
    return len;
}

static int load_basic_file(c64_t *sys, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror(filename);
        return 0;
    }
    /* Build a complete PRG before touching the machine. */
    uint8_t prg[0xa000 - 0x0801 + 2] = {1, 8, 0, 0};
    size_t size = 4, capacity = 0;
    char *line = NULL;
    const char *error = NULL;
    unsigned row = 0;
    ssize_t count;
    while ((count = getline(&line, &capacity, file)) >= 0) {
        row++;
        if (memchr(line, 0, count)) { error = "NUL byte in source"; break; }
        while (count && (line[count-1] == '\n' || line[count-1] == '\r')) line[--count] = 0;
        char *text = line;
        if (row == 1 && !strncmp(text, "\xef\xbb\xbf", 3)) text += 3;
        while (*text == ' ' || *text == '\t') text++;
        if (!*text) continue;
        if (!isdigit((unsigned char)*text)) { error = "expected a BASIC line number"; break; }
        unsigned long number = strtoul(text, &text, 10);
        if (number > 63999) { error = "line number must be 0..63999"; break; }
        while (*text == ' ' || *text == '\t') text++;
        if (!basic_petscii(text)) { error = "invalid character or PETSCII escape"; break; }
        uint8_t tokens[251];
        int len = basic_tokenize(text, tokens);
        if (len < 0) { error = "tokenized line exceeds 250 bytes"; break; }

        /* Insert in line-number order; repeated numbers replace or delete. */
        size_t pos = 2, oldlen = 0;
        while (pos < size - 2) {
            unsigned n = prg[pos+2] | (prg[pos+3] << 8);
            size_t next = (prg[pos] | (prg[pos+1] << 8)) - 0x0801 + 2;
            if (n >= number) {
                if (n == number) oldlen = next - pos;
                break;
            }
            pos = next;
        }
        size_t newlen = len ? len + 5 : 0;
        if (size - oldlen + newlen > sizeof(prg)) { error = "program exceeds BASIC memory"; break; }
        memmove(prg + pos + newlen, prg + pos + oldlen, size - pos - oldlen);
        size = size - oldlen + newlen;
        if (newlen) {
            prg[pos+2] = number;
            prg[pos+3] = number >> 8;
            memcpy(prg + pos + 4, tokens, len + 1);
        }
        for (pos = 2; pos < size - 2;) {
            size_t next = pos + 5 + strlen((char *)prg + pos + 4);
            unsigned addr = 0x0801 + next - 2;
            prg[pos] = addr;
            prg[pos+1] = addr >> 8;
            pos = next;
        }
    }
    if (!error && !feof(file)) error = "cannot read source";
    fclose(file);
    free(line);
    if (error) {
        fprintf(stderr, "%s:%u: %s\n", filename, row, error);
        return 0;
    }
    return c64_quickload(sys, (chips_range_t){.ptr = prg, .size = size});
}

static int parse_u32(const char *s, uint32_t *out) {
    if (!s || !*s) return 0;

    char *end = NULL;
    unsigned long value = 0;

    if (s[0] == '$') {
        value = strtoul(s + 1, &end, 16);
    } else if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        value = strtoul(s + 2, &end, 16);
    } else {
        int has_hex_alpha = 0;
        for (const char *p = s; *p; p++) {
            if (!isxdigit((unsigned char)*p)) return 0;
            if ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
                has_hex_alpha = 1;
        }
        value = strtoul(s, &end, has_hex_alpha ? 16 : 10);
    }

    if (!end || *end != '\0') return 0;
    *out = (uint32_t)value;
    return 1;
}

static void parse_config(int argc, char **argv) {
    EmuConfig.sdl_mode = 1;
    EmuConfig.llm_mode = 0;
    EmuConfig.auto_run = 0;
    EmuConfig.bas_mode = 0;
    EmuConfig.prg_filename = NULL;
    EmuConfig.zoom = 2.0f;
    snprintf(EmuConfig.llm_socket_path, sizeof(EmuConfig.llm_socket_path),
             "/tmp/c64-llm.sock");

    int saw_llm = 0;
    int saw_sdl_toggle = 0;

    for (int i = 1; i < argc; i++) {
        int left = argc - i - 1;

        if (!strcasecmp(argv[i], "--llm")) {
            EmuConfig.llm_mode = 1;
            saw_llm = 1;
        } else if (!strcasecmp(argv[i], "--sdl")) {
            EmuConfig.sdl_mode = 1;
            saw_sdl_toggle = 1;
        } else if (!strcasecmp(argv[i], "--headless") ||
                   !strcasecmp(argv[i], "--no-sdl")) {
            EmuConfig.sdl_mode = 0;
            saw_sdl_toggle = 1;
        } else if (!strcasecmp(argv[i], "--llm-socket") && left > 0) {
            i++;
            snprintf(EmuConfig.llm_socket_path, sizeof(EmuConfig.llm_socket_path),
                     "%s", argv[i]);
        } else if (!strcasecmp(argv[i], "--zoom") && left > 0) {
            i++;
            EmuConfig.zoom = strtod(argv[i], NULL);
            if (EmuConfig.zoom < C64_MIN_ZOOM) EmuConfig.zoom = C64_MIN_ZOOM;
            if (EmuConfig.zoom > C64_MAX_ZOOM) EmuConfig.zoom = C64_MAX_ZOOM;
        } else if (!strcasecmp(argv[i], "--run")) {
            EmuConfig.auto_run = 1;
        } else if (!strcasecmp(argv[i], "--bas")) {
            EmuConfig.bas_mode = 1;
        } else if (argv[i][0] != '-' && EmuConfig.prg_filename == NULL) {
            EmuConfig.prg_filename = strdup(argv[i]);
        } else {
            fprintf(stderr,
                "Usage: %s [--sdl] [--llm] [--headless|--no-sdl] "
                "[--llm-socket /tmp/c64-llm.sock] [--zoom N] [--run] [--bas] [file]\n",
                argv[0]);
            exit(1);
        }
    }

    if (EmuConfig.bas_mode && !EmuConfig.prg_filename) {
        fprintf(stderr, "--bas requires a BASIC source file\n");
        exit(1);
    }

    /* Keep backward compatibility: --llm alone is headless unless SDL toggled. */
    if (saw_llm && !saw_sdl_toggle) EmuConfig.sdl_mode = 0;

    if (!EmuConfig.sdl_mode && !EmuConfig.llm_mode) {
        fprintf(stderr, "Invalid mode: at least one of SDL or --llm must be enabled\n");
        exit(1);
    }
}

static int map_sdl_key_to_c64(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: return C64_KEY_CSRUP;
    case SDLK_DOWN: return C64_KEY_CSRDOWN;
    case SDLK_LEFT: return C64_KEY_CSRLEFT;
    case SDLK_RIGHT: return C64_KEY_CSRRIGHT;
    case SDLK_BACKSPACE: return C64_KEY_DEL;
    case SDLK_RETURN: return C64_KEY_RETURN;
    case SDLK_HOME: return C64_KEY_HOME;
    case SDLK_F1: return C64_KEY_F1;
    case SDLK_F2: return C64_KEY_F2;
    case SDLK_F3: return C64_KEY_F3;
    case SDLK_F4: return C64_KEY_F4;
    case SDLK_F5: return C64_KEY_F5;
    case SDLK_F6: return C64_KEY_F6;
    case SDLK_F7: return C64_KEY_F7;
    case SDLK_F8: return C64_KEY_F8;
    default:
        break;
    }

    if (key >= 32 && key <= 126) {
        int c64_key = (int)key;
        if (islower(c64_key)) c64_key = toupper(c64_key);
        else if (isupper(c64_key)) c64_key = tolower(c64_key);
        return c64_key;
    }
    return 0;
}

static int normalize_ascii_to_c64(int key) {
    if (key >= 32 && key <= 126) {
        if (islower(key)) return toupper(key);
        if (isupper(key)) return tolower(key);
        return key;
    }
    return 0;
}

static int parse_key_token(const char *tok, int *out_key) {
    if (!tok || !*tok || !out_key) return 0;

    if (strlen(tok) == 1) {
        int k = normalize_ascii_to_c64((unsigned char)tok[0]);
        if (!k) return 0;
        *out_key = k;
        return 1;
    }

    if (!strcasecmp(tok, "SPACE"))   { *out_key = C64_KEY_SPACE; return 1; }
    if (!strcasecmp(tok, "RETURN"))  { *out_key = C64_KEY_RETURN; return 1; }
    if (!strcasecmp(tok, "ENTER"))   { *out_key = C64_KEY_RETURN; return 1; }
    if (!strcasecmp(tok, "DEL"))     { *out_key = C64_KEY_DEL; return 1; }
    if (!strcasecmp(tok, "BACKSPACE")) { *out_key = C64_KEY_DEL; return 1; }
    if (!strcasecmp(tok, "HOME"))    { *out_key = C64_KEY_HOME; return 1; }
    if (!strcasecmp(tok, "LEFT"))    { *out_key = C64_KEY_CSRLEFT; return 1; }
    if (!strcasecmp(tok, "RIGHT"))   { *out_key = C64_KEY_CSRRIGHT; return 1; }
    if (!strcasecmp(tok, "UP"))      { *out_key = C64_KEY_CSRUP; return 1; }
    if (!strcasecmp(tok, "DOWN"))    { *out_key = C64_KEY_CSRDOWN; return 1; }
    if (!strcasecmp(tok, "F1"))      { *out_key = C64_KEY_F1; return 1; }
    if (!strcasecmp(tok, "F2"))      { *out_key = C64_KEY_F2; return 1; }
    if (!strcasecmp(tok, "F3"))      { *out_key = C64_KEY_F3; return 1; }
    if (!strcasecmp(tok, "F4"))      { *out_key = C64_KEY_F4; return 1; }
    if (!strcasecmp(tok, "F5"))      { *out_key = C64_KEY_F5; return 1; }
    if (!strcasecmp(tok, "F6"))      { *out_key = C64_KEY_F6; return 1; }
    if (!strcasecmp(tok, "F7"))      { *out_key = C64_KEY_F7; return 1; }
    if (!strcasecmp(tok, "F8"))      { *out_key = C64_KEY_F8; return 1; }
    if (!strcasecmp(tok, "STOP"))    { *out_key = C64_KEY_STOP; return 1; }
    if (!strcasecmp(tok, "RUN"))     { *out_key = C64_KEY_RUN; return 1; }
    if (!strcasecmp(tok, "CTRL"))    { *out_key = C64_KEY_CTRL; return 1; }
    if (!strcasecmp(tok, "CBM"))     { *out_key = C64_KEY_CBM; return 1; }
    if (!strcasecmp(tok, "RESTORE")) { *out_key = C64_KEY_RESTORE; return 1; }

    uint32_t raw;
    if (parse_u32(tok, &raw) && raw <= 0xFF) {
        *out_key = (int)raw;
        return 1;
    }
    return 0;
}

static uint8_t sdl_key_to_joy_mask(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: return C64_JOYSTICK_UP;
    case SDLK_DOWN: return C64_JOYSTICK_DOWN;
    case SDLK_LEFT: return C64_JOYSTICK_LEFT;
    case SDLK_RIGHT: return C64_JOYSTICK_RIGHT;
    case SDLK_TAB: return C64_JOYSTICK_BTN;
    default: return 0;
    }
}

static int sdl_is_manual_debug_stop(const SDL_KeyboardEvent *key) {
    if (!key) return 0;
    if (key->repeat) return 0;

    SDL_Keycode sym = key->keysym.sym;
    int is_ctrl = (sym == SDLK_LCTRL || sym == SDLK_RCTRL);
    int is_shift = (sym == SDLK_LSHIFT || sym == SDLK_RSHIFT);
    if (!is_ctrl && !is_shift) return 0;

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int ctrl_down = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    int shift_down = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    if (is_ctrl) ctrl_down = 1;
    if (is_shift) shift_down = 1;
    return ctrl_down && shift_down;
}

static int process_sdl_events(c64_t *c64, int *manual_stop_requested) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) return 1;
        if (event.type == SDL_KEYDOWN) {
            if (sdl_is_manual_debug_stop(&event.key)) {
                if (manual_stop_requested) *manual_stop_requested = 1;
                continue;
            }
            if (event.key.repeat) continue;
            if (event.key.keysym.sym == SDLK_ESCAPE) return 1;

            uint8_t joy_mask = sdl_key_to_joy_mask(event.key.keysym.sym);
            if (joy_mask) {
                sdl_joy2_mask |= joy_mask;
                c64_joystick(c64, 0, sdl_joy2_mask);
            }

            int c64_key = map_sdl_key_to_c64(event.key.keysym.sym);
            if (c64_key) c64_key_down(c64, c64_key);
        } else if (event.type == SDL_KEYUP) {
            uint8_t joy_mask = sdl_key_to_joy_mask(event.key.keysym.sym);
            if (joy_mask) {
                sdl_joy2_mask &= (uint8_t)~joy_mask;
                c64_joystick(c64, 0, sdl_joy2_mask);
            }

            int c64_key = map_sdl_key_to_c64(event.key.keysym.sym);
            if (c64_key) c64_key_up(c64, c64_key);
        }
    }
    return 0;
}

static int c64_is_basic_ready_pc(uint16_t pc) {
    /* KERNAL/BASIC idle loop PCs where READY prompt is active on common ROMs. */
    switch (pc) {
    case 0xE5CD:
    case 0xE5CF:
    case 0xE5D1:
    case 0xE5D4:
    case 0xE5D6:
    case 0xE5D8:
        return 1;
    default:
        return 0;
    }
}

typedef struct llm_state_t llm_state_t;
static void llm_sendf(llm_state_t *st, const char *fmt, ...);

typedef enum {
    AM_IMP = 0,
    AM_ACC,
    AM_IMM,
    AM_ZP,
    AM_ZPX,
    AM_ZPY,
    AM_ABS,
    AM_ABSX,
    AM_ABSY,
    AM_IND,
    AM_INDX,
    AM_INDY,
    AM_REL,
} llm_addr_mode_t;

typedef struct {
    const char *mnemonic;
    llm_addr_mode_t mode;
    uint8_t len;
} llm_insn_t;

static uint16_t llm_mem_rd_u16(c64_t *c64, uint16_t addr) {
    uint8_t lo = mem_rd(&c64->mem_cpu, addr);
    uint8_t hi = mem_rd(&c64->mem_cpu, (uint16_t)(addr + 1));
    return (uint16_t)((hi << 8) | lo);
}

/* Parse first BASIC line "SYS nnnn" when program starts at $0801. */
static int llm_parse_basic_sys_start(c64_t *c64, uint16_t start_addr, uint16_t *out_sys_addr) {
    if (start_addr != 0x0801 || !out_sys_addr) return 0;

    uint16_t line = start_addr;
    uint16_t next_line = llm_mem_rd_u16(c64, line);
    if (next_line == 0) return 0;

    line += 4; /* skip next-line ptr + line number */
    if (mem_rd(&c64->mem_cpu, line) != 0x9E) return 0; /* BASIC token SYS */
    line++;

    char buf[8];
    int n = 0;
    while (n < (int)(sizeof(buf) - 1)) {
        uint8_t c = mem_rd(&c64->mem_cpu, line++);
        if (c == 0) break;
        if (c < '0' || c > '9') return 0;
        buf[n++] = (char)c;
    }
    if (n == 0) return 0;
    buf[n] = '\0';

    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (!end || *end != '\0' || v < 0 || v > 0xFFFF) return 0;
    *out_sys_addr = (uint16_t)v;
    return 1;
}

/* 6502 indirect JMP page-wrap behavior. */
static uint16_t llm_mem_rd_u16_jmp_bug(c64_t *c64, uint16_t addr) {
    uint8_t lo = mem_rd(&c64->mem_cpu, addr);
    uint16_t hi_addr = (uint16_t)((addr & 0xFF00) | ((addr + 1) & 0x00FF));
    uint8_t hi = mem_rd(&c64->mem_cpu, hi_addr);
    return (uint16_t)((hi << 8) | lo);
}

static llm_insn_t llm_decode_opcode(uint8_t op) {
#define RET(MN, MODE, LEN) \
    do {                   \
        d.mnemonic = (MN); \
        d.mode = (MODE);   \
        d.len = (LEN);     \
        return d;          \
    } while (0)
    llm_insn_t d = { "???", AM_IMP, 1 };

    switch (op) {
    case 0x00: RET("BRK", AM_IMP, 1);
    case 0x08: RET("PHP", AM_IMP, 1);
    case 0x10: RET("BPL", AM_REL, 2);
    case 0x18: RET("CLC", AM_IMP, 1);
    case 0x20: RET("JSR", AM_ABS, 3);
    case 0x24: RET("BIT", AM_ZP, 2);
    case 0x28: RET("PLP", AM_IMP, 1);
    case 0x2C: RET("BIT", AM_ABS, 3);
    case 0x30: RET("BMI", AM_REL, 2);
    case 0x38: RET("SEC", AM_IMP, 1);
    case 0x40: RET("RTI", AM_IMP, 1);
    case 0x48: RET("PHA", AM_IMP, 1);
    case 0x4C: RET("JMP", AM_ABS, 3);
    case 0x50: RET("BVC", AM_REL, 2);
    case 0x58: RET("CLI", AM_IMP, 1);
    case 0x60: RET("RTS", AM_IMP, 1);
    case 0x68: RET("PLA", AM_IMP, 1);
    case 0x6C: RET("JMP", AM_IND, 3);
    case 0x70: RET("BVS", AM_REL, 2);
    case 0x78: RET("SEI", AM_IMP, 1);
    case 0x84: RET("STY", AM_ZP, 2);
    case 0x88: RET("DEY", AM_IMP, 1);
    case 0x8A: RET("TXA", AM_IMP, 1);
    case 0x8C: RET("STY", AM_ABS, 3);
    case 0x90: RET("BCC", AM_REL, 2);
    case 0x94: RET("STY", AM_ZPX, 2);
    case 0x98: RET("TYA", AM_IMP, 1);
    case 0x9A: RET("TXS", AM_IMP, 1);
    case 0xA0: RET("LDY", AM_IMM, 2);
    case 0xA4: RET("LDY", AM_ZP, 2);
    case 0xA8: RET("TAY", AM_IMP, 1);
    case 0xAA: RET("TAX", AM_IMP, 1);
    case 0xAC: RET("LDY", AM_ABS, 3);
    case 0xB0: RET("BCS", AM_REL, 2);
    case 0xB4: RET("LDY", AM_ZPX, 2);
    case 0xB8: RET("CLV", AM_IMP, 1);
    case 0xBA: RET("TSX", AM_IMP, 1);
    case 0xBC: RET("LDY", AM_ABSX, 3);
    case 0xC0: RET("CPY", AM_IMM, 2);
    case 0xC4: RET("CPY", AM_ZP, 2);
    case 0xC8: RET("INY", AM_IMP, 1);
    case 0xCA: RET("DEX", AM_IMP, 1);
    case 0xCC: RET("CPY", AM_ABS, 3);
    case 0xD0: RET("BNE", AM_REL, 2);
    case 0xD8: RET("CLD", AM_IMP, 1);
    case 0xE0: RET("CPX", AM_IMM, 2);
    case 0xE4: RET("CPX", AM_ZP, 2);
    case 0xE8: RET("INX", AM_IMP, 1);
    case 0xEA: RET("NOP", AM_IMP, 1);
    case 0xEC: RET("CPX", AM_ABS, 3);
    case 0xF0: RET("BEQ", AM_REL, 2);
    case 0xF8: RET("SED", AM_IMP, 1);
    default:
        break;
    }

    uint8_t cc = op & 0x03;
    uint8_t aaa = (op >> 5) & 0x07;
    uint8_t bbb = (op >> 2) & 0x07;

    if (cc == 0x01) {
        static const char *mn[8] = { "ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC" };
        d.mnemonic = mn[aaa];
        switch (bbb) {
        case 0: d.mode = AM_INDX; d.len = 2; break;
        case 1: d.mode = AM_ZP;   d.len = 2; break;
        case 2:
            if (aaa == 4) return d; /* STA #imm invalid */
            d.mode = AM_IMM; d.len = 2; break;
        case 3: d.mode = AM_ABS;  d.len = 3; break;
        case 4: d.mode = AM_INDY; d.len = 2; break;
        case 5: d.mode = AM_ZPX;  d.len = 2; break;
        case 6: d.mode = AM_ABSY; d.len = 3; break;
        case 7: d.mode = AM_ABSX; d.len = 3; break;
        default: break;
        }
        return d;
    }

    if (cc == 0x02) {
        static const char *mn[8] = { "ASL", "ROL", "LSR", "ROR", "STX", "LDX", "DEC", "INC" };
        d.mnemonic = mn[aaa];
        switch (bbb) {
        case 0:
            if (aaa == 5) { d.mode = AM_IMM; d.len = 2; } /* LDX #imm */
            break;
        case 1:
            d.mode = AM_ZP;
            d.len = 2;
            break;
        case 2:
            if (aaa <= 3) { d.mode = AM_ACC; d.len = 1; } /* shifts/rotates A */
            break;
        case 3:
            d.mode = AM_ABS;
            d.len = 3;
            break;
        case 5:
            if (aaa == 4 || aaa == 5) d.mode = AM_ZPY;   /* STX/LDX */
            else d.mode = AM_ZPX;
            d.len = 2;
            break;
        case 7:
            if (aaa == 5) { d.mode = AM_ABSY; d.len = 3; } /* LDX abs,Y */
            else if (aaa <= 3 || aaa >= 6) { d.mode = AM_ABSX; d.len = 3; }
            break;
        default:
            break;
        }
        return d;
    }

    return d;
#undef RET
}

static void llm_format_operand(c64_t *c64, uint16_t pc, llm_insn_t insn, char *buf, size_t n) {
    uint8_t b1 = mem_rd(&c64->mem_cpu, (uint16_t)(pc + 1));
    uint8_t b2 = mem_rd(&c64->mem_cpu, (uint16_t)(pc + 2));
    uint16_t w = (uint16_t)(b1 | (b2 << 8));
    uint16_t target = (uint16_t)(pc + 2 + (int8_t)b1);
    switch (insn.mode) {
    case AM_IMP:  buf[0] = '\0'; break;
    case AM_ACC:  snprintf(buf, n, "A"); break;
    case AM_IMM:  snprintf(buf, n, "#$%02X", b1); break;
    case AM_ZP:   snprintf(buf, n, "$%02X", b1); break;
    case AM_ZPX:  snprintf(buf, n, "$%02X,X", b1); break;
    case AM_ZPY:  snprintf(buf, n, "$%02X,Y", b1); break;
    case AM_ABS:  snprintf(buf, n, "$%04X", w); break;
    case AM_ABSX: snprintf(buf, n, "$%04X,X", w); break;
    case AM_ABSY: snprintf(buf, n, "$%04X,Y", w); break;
    case AM_IND:  snprintf(buf, n, "($%04X)", w); break;
    case AM_INDX: snprintf(buf, n, "($%02X,X)", b1); break;
    case AM_INDY: snprintf(buf, n, "($%02X),Y", b1); break;
    case AM_REL:  snprintf(buf, n, "$%04X", target); break;
    default:      buf[0] = '\0'; break;
    }
}

static uint16_t llm_predict_next_pc(c64_t *c64, uint16_t pc, llm_insn_t insn) {
    uint8_t p = m6502_p(&c64->cpu);
    uint8_t b1 = mem_rd(&c64->mem_cpu, (uint16_t)(pc + 1));
    uint16_t w = llm_mem_rd_u16(c64, (uint16_t)(pc + 1));
    uint16_t seq = (uint16_t)(pc + (insn.len ? insn.len : 1));

    if (!strcmp(insn.mnemonic, "JMP")) {
        if (insn.mode == AM_ABS) return w;
        if (insn.mode == AM_IND) return llm_mem_rd_u16_jmp_bug(c64, w);
    }
    if (!strcmp(insn.mnemonic, "JSR") && insn.mode == AM_ABS) return w;

    if (insn.mode == AM_REL && insn.mnemonic[0] == 'B' && insn.mnemonic[1] != 'R') {
        bool taken = false;
        switch (insn.mnemonic[1]) {
        case 'P': taken = (p & M6502_NF) == 0; break; /* BPL */
        case 'M': taken = (p & M6502_NF) != 0; break; /* BMI */
        case 'V':
            if (insn.mnemonic[2] == 'C') taken = (p & M6502_VF) == 0; /* BVC */
            else taken = (p & M6502_VF) != 0;                          /* BVS */
            break;
        case 'C':
            if (insn.mnemonic[2] == 'C') taken = (p & M6502_CF) == 0; /* BCC */
            else taken = (p & M6502_CF) != 0;                          /* BCS */
            break;
        case 'N': taken = (p & M6502_ZF) == 0; break; /* BNE */
        case 'E': taken = (p & M6502_ZF) != 0; break; /* BEQ */
        default: break;
        }
        if (taken) return (uint16_t)(pc + 2 + (int8_t)b1);
    }
    return seq;
}

static void llm_send_disasm_line(c64_t *c64, llm_state_t *st, const char *tag, uint16_t pc) {
    llm_insn_t insn = llm_decode_opcode(mem_rd(&c64->mem_cpu, pc));
    uint8_t b0 = mem_rd(&c64->mem_cpu, pc);
    uint8_t b1 = mem_rd(&c64->mem_cpu, (uint16_t)(pc + 1));
    uint8_t b2 = mem_rd(&c64->mem_cpu, (uint16_t)(pc + 2));
    char bytes[16];
    char operand[32];

    if (insn.len <= 1) snprintf(bytes, sizeof(bytes), "%02X      ", b0);
    else if (insn.len == 2) snprintf(bytes, sizeof(bytes), "%02X %02X   ", b0, b1);
    else snprintf(bytes, sizeof(bytes), "%02X %02X %02X", b0, b1, b2);

    llm_format_operand(c64, pc, insn, operand, sizeof(operand));
    if (operand[0]) {
        llm_sendf(st, "OK %s $%04X: %-8s %s %s\n", tag, pc, bytes, insn.mnemonic, operand);
    } else {
        llm_sendf(st, "OK %s $%04X: %-8s %s\n", tag, pc, bytes, insn.mnemonic);
    }
}

/* ======================= LLM Debug Server ======================= */

typedef enum {
    BP_PC = 1,
    BP_READ = 2,
    BP_WRITE = 3,
} bp_type_t;

typedef struct {
    int id;
    bp_type_t type;
    uint16_t addr;
    int enabled;
} breakpoint_t;

typedef struct {
    int hit;
    int id;
    bp_type_t type;
    uint16_t addr;
} break_hit_t;

struct llm_state_t {
    int listen_fd;
    int client_fd;
    char sock_path[108];
    char rxbuf[8192];
    size_t rxlen;

    int stopped;
    uint64_t frame_counter;
    uint64_t instr_counter;
    int profiling_enabled;
    uint64_t *profile_hits;
    uint8_t joy1_mask;
    uint8_t joy2_mask;

    breakpoint_t bps[256];
    int bp_count;
    int next_bp_id;
};

typedef struct {
    uint16_t addr;
    uint64_t hits;
} profile_entry_t;

static int llm_profile_entry_cmp_desc(const void *pa, const void *pb) {
    const profile_entry_t *a = (const profile_entry_t *)pa;
    const profile_entry_t *b = (const profile_entry_t *)pb;

    if (a->hits > b->hits) return -1;
    if (a->hits < b->hits) return 1;
    if (a->addr < b->addr) return -1;
    if (a->addr > b->addr) return 1;
    return 0;
}

static int llm_profile_init(llm_state_t *st) {
    st->profiling_enabled = 0;
    st->profile_hits = calloc(LLM_PROFILE_ADDR_COUNT, sizeof(uint64_t));
    return st->profile_hits != NULL;
}

static void llm_profile_reset(llm_state_t *st) {
    if (!st->profile_hits) return;
    memset(st->profile_hits, 0, LLM_PROFILE_ADDR_COUNT * sizeof(uint64_t));
}

static void llm_profile_shutdown(llm_state_t *st) {
    if (!st->profile_hits) return;
    free(st->profile_hits);
    st->profile_hits = NULL;
    st->profiling_enabled = 0;
}

static int llm_wait_basic_ready(c64_t *c64, llm_state_t *st, int max_frames) {
    for (int i = 0; i < max_frames; i++) {
        if (c64_is_basic_ready_pc(m6502_pc(&c64->cpu))) return 1;
        c64_exec(c64, FRAME_USEC);
        if (st) st->frame_counter++;
    }
    return c64_is_basic_ready_pc(m6502_pc(&c64->cpu));
}

static int llm_prepare_program_boot(c64_t *c64, llm_state_t *st, int max_frames) {
    c64_reset(c64);
    llm_cia_trace_reset();
    st->joy1_mask = 0;
    st->joy2_mask = 0;
    c64_joystick(c64, 0, 0);
    st->stopped = 1;
    st->instr_counter = 0;
    st->frame_counter = 0;
    return llm_wait_basic_ready(c64, st, max_frames);
}

static void llm_start_machine_code(c64_t *c64, uint16_t start_addr) {
    m6502_set_a(&c64->cpu, 0x00);
    m6502_set_x(&c64->cpu, 0x00);
    m6502_set_y(&c64->cpu, 0x00);
    m6502_set_s(&c64->cpu, 0xFF);
    /* BASIC SYS enters with IRQs enabled. Depackers may rely on the KERNAL
     * servicing CIA interrupts until they install their own handlers. */
    m6502_set_p(&c64->cpu, 0x20);
    m6502_set_pc(&c64->cpu, start_addr);
    c64->pins = M6502_SYNC;
    M6502_SET_ADDR(c64->pins, start_addr);
    M6502_SET_DATA(c64->pins, mem_rd(&c64->mem_cpu, start_addr));
}

static const char *bp_type_name(bp_type_t t) {
    switch (t) {
    case BP_PC: return "PC";
    case BP_READ: return "READ";
    case BP_WRITE: return "WRITE";
    default: return "?";
    }
}

static void llm_close_client(llm_state_t *st);

static void llm_sendf(llm_state_t *st, const char *fmt, ...) {
    if (st->client_fd < 0) return;

    char out[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(out), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    if (n >= (int)sizeof(out)) n = (int)sizeof(out) - 1;

    size_t sent = 0;
    size_t total = (size_t)n;
    while (sent < total) {
        ssize_t wr = write(st->client_fd, out + sent, total - sent);
        if (wr > 0) {
            sent += (size_t)wr;
            continue;
        }
        if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (wr < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)) {
            llm_close_client(st);
            return;
        }
        break;
    }
}

static void llm_close_client(llm_state_t *st) {
    if (st->client_fd >= 0) {
        close(st->client_fd);
        st->client_fd = -1;
    }
    st->rxlen = 0;
}

static int llm_setup_socket(llm_state_t *st, const char *path) {
    struct sockaddr_un addr;

    st->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (st->listen_fd < 0) return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);

    if (bind(st->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(st->listen_fd);
        st->listen_fd = -1;
        return 0;
    }

    if (listen(st->listen_fd, 1) != 0) {
        close(st->listen_fd);
        st->listen_fd = -1;
        return 0;
    }

    int flags = fcntl(st->listen_fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(st->listen_fd, F_SETFL, flags | O_NONBLOCK);

    snprintf(st->sock_path, sizeof(st->sock_path), "%s", path);
    return 1;
}

static void llm_accept_client(llm_state_t *st) {
    while (1) {
        int fd = accept(st->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            return;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        /* Single active client: newest connection replaces stale one. */
        if (st->client_fd >= 0) close(st->client_fd);
        st->client_fd = fd;
        st->rxlen = 0;
        llm_sendf(st,
            "OK C64-LLM READY socket=%s stopped=%d\n"
            "OK Type HELP for command list\n",
            st->sock_path, st->stopped);
    }
}

static int llm_add_breakpoint(llm_state_t *st, bp_type_t type, uint16_t addr) {
    if (st->bp_count >= (int)(sizeof(st->bps) / sizeof(st->bps[0]))) return -1;

    breakpoint_t *bp = &st->bps[st->bp_count++];
    bp->id = st->next_bp_id++;
    bp->type = type;
    bp->addr = addr;
    bp->enabled = 1;
    return bp->id;
}

static int llm_delete_breakpoint(llm_state_t *st, int id) {
    for (int i = 0; i < st->bp_count; i++) {
        if (st->bps[i].id == id) {
            for (int j = i; j < st->bp_count - 1; j++) st->bps[j] = st->bps[j + 1];
            st->bp_count--;
            return 1;
        }
    }
    return 0;
}

static int llm_find_bp_hit(llm_state_t *st, bp_type_t type, uint16_t addr) {
    for (int i = 0; i < st->bp_count; i++) {
        breakpoint_t *bp = &st->bps[i];
        if (!bp->enabled) continue;
        if (bp->type == type && bp->addr == addr) return bp->id;
    }
    return 0;
}

static void llm_cpu_dump(c64_t *c64, llm_state_t *st) {
    uint16_t pc = m6502_pc(&c64->cpu);
    llm_insn_t insn = llm_decode_opcode(mem_rd(&c64->mem_cpu, pc));
    uint16_t next_pc = llm_predict_next_pc(c64, pc, insn);

    llm_sendf(st,
        "OK PC=$%04X A=$%02X X=$%02X Y=$%02X S=$%02X P=$%02X STOPPED=%d FRAMES=%llu INSTR=%llu\n",
        pc,
        m6502_a(&c64->cpu),
        m6502_x(&c64->cpu),
        m6502_y(&c64->cpu),
        m6502_s(&c64->cpu),
        m6502_p(&c64->cpu),
        st->stopped,
        (unsigned long long)st->frame_counter,
        (unsigned long long)st->instr_counter);
    llm_sendf(st,
        "OK PORT6510 DDR=$%02X OUT=$%02X IN=$%02X PINS=$%02X C64PORT=$%02X IOMAP=%d VICBANK=$%04X\n",
        c64->cpu.io_ddr,
        c64->cpu.io_out,
        c64->cpu.io_inp,
        c64->cpu.io_pins,
        c64->cpu_port,
        c64->io_mapped ? 1 : 0,
        c64->vic_bank_select);
    llm_send_disasm_line(c64, st, "CUR", pc);
    llm_send_disasm_line(c64, st, "NEXT", next_pc);
}

static void llm_vic_dump(c64_t *c64, llm_state_t *st) {
    const uint8_t *r = c64->vic.reg.regs;
    llm_sendf(st,
        "OK VIC D011=$%02X D016=$%02X D018=$%02X D010=$%02X D015=$%02X D017=$%02X D01D=$%02X D01C=$%02X\n",
        r[0x11], r[0x16], r[0x18], r[0x10], r[0x15], r[0x17], r[0x1d], r[0x1c]);

    for (int i = 0; i < 8; i++) {
        uint16_t sx = (uint16_t)r[i * 2] + ((r[0x10] & (1u << i)) ? 256u : 0u);
        uint8_t sy = r[i * 2 + 1];
        uint8_t ptr = mem_rd(&c64->mem_cpu, (uint16_t)(0x07f8 + i));
        llm_sendf(st,
            "OK SPR%d x=%u y=%u color=$%02X ptr=$%02X en=%d xexp=%d yexp=%d mc=%d\n",
            i, (unsigned)sx, (unsigned)sy, r[0x27 + i], (unsigned)ptr,
            (r[0x15] & (1u << i)) ? 1 : 0,
            (r[0x1d] & (1u << i)) ? 1 : 0,
            (r[0x17] & (1u << i)) ? 1 : 0,
            (r[0x1c] & (1u << i)) ? 1 : 0);
    }
}

static void llm_cpuint_dump(c64_t *c64, llm_state_t *st) {
    llm_sendf(st,
        "OK CPUINT cpu_pins=$%016llX sys_pins=$%016llX irq_pip=$%04X nmi_pip=$%04X brk=$%02X ir=$%04X p=$%02X\n",
        (unsigned long long)c64->cpu.PINS,
        (unsigned long long)c64->pins,
        (unsigned)c64->cpu.irq_pip,
        (unsigned)c64->cpu.nmi_pip,
        c64->cpu.brk_flags,
        (unsigned)c64->cpu.IR,
        c64->cpu.P);
}

static void llm_cia_trace_dump(llm_state_t *st, uint32_t limit) {
    if (limit == 0 || limit > llm_cia1_trace_count) limit = llm_cia1_trace_count;
    llm_sendf(st, "OK CIATRACE count=%u total=%u\n",
              (unsigned)limit, (unsigned)llm_cia1_trace_count);
    uint32_t start = (llm_cia1_trace_head + LLM_CIA_TRACE_CAP - limit) % LLM_CIA_TRACE_CAP;
    for (uint32_t i = 0; i < limit; i++) {
        uint32_t idx = (start + i) % LLM_CIA_TRACE_CAP;
        const llm_cia_trace_entry_t *e = &llm_cia1_trace[idx];
        llm_sendf(st,
            "%3u pc=$%04X %s $%04X data=$%02X irq=%u ta_ctr=$%04X ta_lat=$%04X ta_cr=$%02X icr=$%02X imr=$%02X\n",
            (unsigned)(i + 1), e->pc, e->rw ? "RD" : "WR", e->addr, e->data, (unsigned)e->irq_pin,
            e->ta_counter, e->ta_latch, e->ta_cr, e->intr_icr, e->intr_imr);
    }
    llm_sendf(st, "OK END\n");
}

static void llm_cia_dump_one(llm_state_t *st, const char *name, const m6526_t *cia) {
    llm_sendf(st,
        "OK %s TA counter=$%04X latch=$%04X cr=$%02X t_bit=%d t_out=%d pip=$%08X\n",
        name,
        (unsigned)cia->ta.counter, (unsigned)cia->ta.latch, cia->ta.cr,
        cia->ta.t_bit ? 1 : 0, cia->ta.t_out ? 1 : 0, (unsigned)cia->ta.pip);
    llm_sendf(st,
        "OK %s TB counter=$%04X latch=$%04X cr=$%02X t_bit=%d t_out=%d pip=$%08X\n",
        name,
        (unsigned)cia->tb.counter, (unsigned)cia->tb.latch, cia->tb.cr,
        cia->tb.t_bit ? 1 : 0, cia->tb.t_out ? 1 : 0, (unsigned)cia->tb.pip);
    llm_sendf(st,
        "OK %s INTR icr=$%02X imr=$%02X imr1=$%02X pip=$%08X flag=%d pins=$%016llX\n",
        name,
        cia->intr.icr, cia->intr.imr, cia->intr.imr1,
        (unsigned)cia->intr.pip, cia->intr.flag ? 1 : 0,
        (unsigned long long)cia->pins);
}

static void llm_cia_dump(c64_t *c64, llm_state_t *st, int which) {
    if (which == 1) {
        llm_cia_dump_one(st, "CIA1", &c64->cia_1);
    } else if (which == 2) {
        llm_cia_dump_one(st, "CIA2", &c64->cia_2);
    } else {
        llm_sendf(st, "ERR CIA requires 1 or 2\n");
    }
}

static void llm_step_instruction(c64_t *c64, llm_state_t *st, break_hit_t *hit) {
    memset(hit, 0, sizeof(*hit));

    /* Break if current PC already matches a PC breakpoint. */
    uint16_t pc = m6502_pc(&c64->cpu);
    uint16_t exec_pc = pc;
    int id = llm_find_bp_hit(st, BP_PC, pc);
    if (id) {
        hit->hit = 1;
        hit->id = id;
        hit->type = BP_PC;
        hit->addr = pc;
        return;
    }

    bool left_sync = (c64->pins & M6502_SYNC) ? false : true;

    for (int i = 0; i < 200000; i++) {
        uint64_t pins = _c64_tick(c64, c64->pins);
        c64->pins = pins;

        uint16_t addr = M6502_GET_ADDR(pins);
        if (pins & M6502_RW) {
            id = llm_find_bp_hit(st, BP_READ, addr);
            if (id) {
                hit->hit = 1;
                hit->id = id;
                hit->type = BP_READ;
                hit->addr = addr;
                return;
            }
        } else {
            id = llm_find_bp_hit(st, BP_WRITE, addr);
            if (id) {
                hit->hit = 1;
                hit->id = id;
                hit->type = BP_WRITE;
                hit->addr = addr;
                return;
            }
        }

        if (!(pins & M6502_SYNC)) {
            left_sync = true;
        } else if (left_sync) {
            st->instr_counter++;
            if (st->profiling_enabled && st->profile_hits) {
                st->profile_hits[exec_pc]++;
            }
            uint16_t next_pc = m6502_pc(&c64->cpu);
            id = llm_find_bp_hit(st, BP_PC, next_pc);
            if (id) {
                hit->hit = 1;
                hit->id = id;
                hit->type = BP_PC;
                hit->addr = next_pc;
            }
            return;
        }
    }
}

/* Execute up to max_instr instructions in RUNNING mode and stop on breakpoint.
 * Used by both headless and SDL+LLM hybrid mode so breakpoint semantics match.
 */
static void llm_run_instruction_slice(c64_t *c64, llm_state_t *st, int max_instr) {
    for (int i = 0; i < max_instr && !st->stopped; i++) {
        break_hit_t hit;
        llm_step_instruction(c64, st, &hit);
        if (hit.hit) {
            st->stopped = 1;
            llm_sendf(st,
                "STOPPED BREAKPOINT id=%d type=%s addr=$%04X pc=$%04X\n",
                hit.id, bp_type_name(hit.type), hit.addr, m6502_pc(&c64->cpu));
            break;
        }
    }
}

static int llm_find_color_index(uint32_t rgb, uint32_t *colors, int count) {
    for (int i = 0; i < count; i++) {
        if (colors[i] == rgb) return i;
    }
    return -1;
}

static int llm_path_has_ext_ci(const char *path, const char *ext) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);
    if (path_len < ext_len) return 0;
    const char *tail = path + (path_len - ext_len);
    for (size_t i = 0; i < ext_len; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i])) {
            return 0;
        }
    }
    return 1;
}

static void llm_write_u32_be(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)((v >> 24) & 0xFF);
    dst[1] = (uint8_t)((v >> 16) & 0xFF);
    dst[2] = (uint8_t)((v >> 8) & 0xFF);
    dst[3] = (uint8_t)(v & 0xFF);
}

static uint32_t llm_crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)-(int)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint32_t llm_adler32(const uint8_t *buf, size_t len) {
    const uint32_t MOD_ADLER = 65521u;
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + buf[i]) % MOD_ADLER;
        b = (b + a) % MOD_ADLER;
    }
    return (b << 16) | a;
}

static int llm_write_png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    uint8_t be_len[4];
    uint8_t be_crc[4];
    llm_write_u32_be(be_len, len);
    if (fwrite(be_len, 1, 4, f) != 4) return 0;
    if (fwrite(type, 1, 4, f) != 4) return 0;
    if (len && fwrite(data, 1, len, f) != len) return 0;

    uint32_t crc = 0;
    crc = llm_crc32_update(crc, (const uint8_t *)type, 4);
    if (len) crc = llm_crc32_update(crc, data, len);
    llm_write_u32_be(be_crc, crc);
    if (fwrite(be_crc, 1, 4, f) != 4) return 0;
    return 1;
}

static int llm_write_screenshot_png(const char *path, const uint8_t *fb, int fb_width,
                                    int win_x, int win_y, int win_w, int win_h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    const size_t row_bytes = (size_t)win_w * 3 + 1;
    const size_t raw_size = row_bytes * (size_t)win_h;
    uint8_t *raw = malloc(raw_size);
    if (!raw) {
        fclose(f);
        return 0;
    }

    for (int y = 0; y < win_h; y++) {
        const int src_y = win_y + y;
        uint8_t *dst = raw + ((size_t)y * row_bytes);
        dst[0] = 0; /* PNG filter type 0 (none). */
        for (int x = 0; x < win_w; x++) {
            const int src_x = win_x + x;
            const uint8_t *p = fb + ((src_y * fb_width + src_x) * 3);
            dst[1 + x * 3 + 0] = p[0];
            dst[1 + x * 3 + 1] = p[1];
            dst[1 + x * 3 + 2] = p[2];
        }
    }

    const size_t block_size = 65535u;
    const size_t num_blocks = (raw_size + block_size - 1) / block_size;
    const size_t zlib_size = 2 + raw_size + num_blocks * 5 + 4;
    uint8_t *zbuf = malloc(zlib_size);
    if (!zbuf) {
        free(raw);
        fclose(f);
        return 0;
    }

    size_t zi = 0;
    zbuf[zi++] = 0x78; /* zlib header CMF */
    zbuf[zi++] = 0x01; /* zlib header FLG (fastest, valid FCHECK) */

    size_t remaining = raw_size;
    size_t roff = 0;
    while (remaining > 0) {
        uint16_t len = (uint16_t)((remaining > block_size) ? block_size : remaining);
        uint8_t bfinal = (remaining <= block_size) ? 1 : 0;
        zbuf[zi++] = bfinal; /* BTYPE=00 stored block */
        zbuf[zi++] = (uint8_t)(len & 0xFF);
        zbuf[zi++] = (uint8_t)((len >> 8) & 0xFF);
        uint16_t nlen = (uint16_t)~len;
        zbuf[zi++] = (uint8_t)(nlen & 0xFF);
        zbuf[zi++] = (uint8_t)((nlen >> 8) & 0xFF);
        memcpy(zbuf + zi, raw + roff, len);
        zi += len;
        roff += len;
        remaining -= len;
    }

    uint32_t adler = llm_adler32(raw, raw_size);
    llm_write_u32_be(zbuf + zi, adler);
    zi += 4;

    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t ihdr[13];
    llm_write_u32_be(ihdr + 0, (uint32_t)win_w);
    llm_write_u32_be(ihdr + 4, (uint32_t)win_h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* color type: truecolor RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */

    int ok = 1;
    if (fwrite(png_sig, 1, sizeof(png_sig), f) != sizeof(png_sig)) ok = 0;
    if (ok && !llm_write_png_chunk(f, "IHDR", ihdr, sizeof(ihdr))) ok = 0;
    if (ok && !llm_write_png_chunk(f, "IDAT", zbuf, (uint32_t)zi)) ok = 0;
    if (ok && !llm_write_png_chunk(f, "IEND", NULL, 0)) ok = 0;

    free(zbuf);
    free(raw);
    fclose(f);
    return ok;
}

static int llm_parse_window_args(int argc, char **argv, int arg_start,
                                 int fb_width, int fb_height,
                                 int *out_x, int *out_y,
                                 int *out_w, int *out_h) {
    if (argc == arg_start) {
        *out_x = 0;
        *out_y = 0;
        *out_w = fb_width;
        *out_h = fb_height;
        return 1;
    }
    if (argc != arg_start + 4) return 0;

    uint32_t x, y, w, h;
    if (!parse_u32(argv[arg_start + 0], &x) ||
        !parse_u32(argv[arg_start + 1], &y) ||
        !parse_u32(argv[arg_start + 2], &w) ||
        !parse_u32(argv[arg_start + 3], &h)) {
        return 0;
    }

    if (x >= (uint32_t)fb_width || y >= (uint32_t)fb_height ||
        w == 0 || h == 0 ||
        w > (uint32_t)fb_width || h > (uint32_t)fb_height ||
        x + w > (uint32_t)fb_width || y + h > (uint32_t)fb_height) {
        return 0;
    }

    *out_x = (int)x;
    *out_y = (int)y;
    *out_w = (int)w;
    *out_h = (int)h;
    return 1;
}

static char llm_screen_code_to_ascii(uint8_t c) {
    /* C64 uppercase/graphics screen code approximation for debug readability. */
    if (c == 0) return '@';
    if (c >= 1 && c <= 26) return (char)('A' + (c - 1));
    if (c == 27) return '[';
    if (c == 28) return ']';
    if (c == 29) return '^';
    if (c == 30) return '-';
    if (c == 31) return '~';
    if (c >= 32 && c <= 126) return (char)c;
    return '.';
}

static int llm_parse_text_window_args(int argc, char **argv, int arg_start,
                                      int *out_x, int *out_y, int *out_w, int *out_h) {
    const int cols = 40;
    const int rows = 25;

    if (argc == arg_start) {
        *out_x = 0;
        *out_y = 0;
        *out_w = cols;
        *out_h = rows;
        return 1;
    }
    if (argc != arg_start + 4) return 0;

    uint32_t x, y, w, h;
    if (!parse_u32(argv[arg_start + 0], &x) ||
        !parse_u32(argv[arg_start + 1], &y) ||
        !parse_u32(argv[arg_start + 2], &w) ||
        !parse_u32(argv[arg_start + 3], &h)) {
        return 0;
    }
    if (x >= (uint32_t)cols || y >= (uint32_t)rows ||
        w == 0 || h == 0 ||
        x + w > (uint32_t)cols || y + h > (uint32_t)rows) {
        return 0;
    }

    *out_x = (int)x;
    *out_y = (int)y;
    *out_w = (int)w;
    *out_h = (int)h;
    return 1;
}

static void llm_dump_text(c64_t *c64, llm_state_t *st, int x, int y, int w, int h) {
    const uint8_t d011 = mem_rd(&c64->mem_cpu, 0xD011);
    const uint8_t d016 = mem_rd(&c64->mem_cpu, 0xD016);
    const uint8_t d018 = mem_rd(&c64->mem_cpu, 0xD018);
    const uint8_t dd00 = mem_rd(&c64->mem_cpu, 0xDD00);

    const int bitmap_mode = (d011 & 0x20) ? 1 : 0;
    const int multicolor_mode = (d016 & 0x10) ? 1 : 0;

    const uint16_t vic_bank_base = (uint16_t)((~dd00) & 0x03) * 0x4000;
    const uint16_t screen_base = (uint16_t)(vic_bank_base + (((d018 >> 4) & 0x0F) * 0x0400));
    const uint16_t charset_base = (uint16_t)(vic_bank_base + (((d018 >> 1) & 0x07) * 0x0800));

    llm_sendf(st,
        "OK TEXT mode=%s bitmap=%d multicolor=%d screen=$%04X charset=$%04X vicbank=$%04X window=%d,%d,%d,%d\n",
        bitmap_mode ? "bitmap" : "text",
        bitmap_mode,
        multicolor_mode,
        screen_base,
        charset_base,
        vic_bank_base,
        x, y, w, h);

    for (int row = y; row < (y + h); row++) {
        char line[64];
        int p = 0;
        for (int col = x; col < (x + w); col++) {
            uint16_t addr = (uint16_t)(screen_base + row * 40 + col);
            uint8_t code = mem_rd(&c64->mem_cpu, addr);
            line[p++] = llm_screen_code_to_ascii(code);
        }
        line[p] = '\0';
        llm_sendf(st, "T%02d %s\n", row, line);
    }
    llm_sendf(st, "OK END\n");
}

static int llm_write_screenshot_text(const char *path, const uint8_t *fb, int fb_width, int fb_height,
                                     int win_x, int win_y, int win_w, int win_h) {
    (void)fb_height;
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    const char symbols[17] = "0123456789ABCDEF";
    uint32_t palette[16];
    int palette_count = 0;
    int overflow_colors = 0;

    for (int y = 0; y < win_h; y++) {
        const int src_y = win_y + y;
        for (int x = 0; x < win_w; x++) {
            const int src_x = win_x + x;
            const uint8_t *p = fb + ((src_y * fb_width + src_x) * 3);
            uint32_t rgb = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            int idx = llm_find_color_index(rgb, palette, palette_count);
            if (idx < 0) {
                if (palette_count < 16) {
                    palette[palette_count++] = rgb;
                } else {
                    overflow_colors++;
                }
            }
        }
    }

    fprintf(f, "C64-LLM-SCREENSHOT V2\n");
    fprintf(f, "WIDTH %d\n", win_w);
    fprintf(f, "HEIGHT %d\n", win_h);
    fprintf(f, "ORIGIN_X %d\n", win_x);
    fprintf(f, "ORIGIN_Y %d\n", win_y);
    fprintf(f, "FORMAT C64_16COLOR_CHARMAP\n");
    fprintf(f, "SYMBOLS %s\n", symbols);
    fprintf(f, "PALETTE_SIZE %d\n", palette_count);
    for (int i = 0; i < palette_count; i++) {
        fprintf(f, "C%c %06X\n", symbols[i], palette[i] & 0xFFFFFF);
    }
    if (overflow_colors) {
        fprintf(f, "WARN OVERFLOW_COLORS %d\n", overflow_colors);
    }

    for (int y = 0; y < win_h; y++) {
        const int src_y = win_y + y;
        fprintf(f, "Y%03d ", y);
        for (int x = 0; x < win_w; x++) {
            const int src_x = win_x + x;
            const uint8_t *p = fb + ((src_y * fb_width + src_x) * 3);
            uint32_t rgb = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            int idx = llm_find_color_index(rgb, palette, palette_count);
            fputc((idx >= 0 && idx < 16) ? symbols[idx] : '?', f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return 1;
}

static void llm_show_help(llm_state_t *st) {
    llm_sendf(st,
        "OK COMMANDS\n"
        "RUN [prg_path]\n"
        "RUNSYS <prg_path|addr>\n"
        "JUMP <addr>\n"
        "JOY1 <mask>\n"
        "JOY2 <mask>\n"
        "KEYDOWN <key>\n"
        "KEYUP <key>\n"
        "KEY <key>\n"
        "TYPE <text>\n"
        "DUMP\n"
        "CPUINT\n"
        "VIC\n"
        "CIA <1|2>\n"
        "CIATRACE [N]\n"
        "DISASM [addr] [count]\n"
        "TEXT [x y w h]\n"
        "MEMGET <addr> <len>\n"
        "MEMSET <addr> <b0> [b1 ...]\n"
        "RESET\n"
        "STOP\n"
        "NEXT\n"
        "FRAME\n"
        "BREAKPOINT LIST\n"
        "BREAKPOINT DEL <id>\n"
        "BREAKPOINT PC <addr>\n"
        "BREAKPOINT READ <addr>\n"
        "BREAKPOINT WRITE <addr>\n"
        "PSTART\n"
        "PSTOP\n"
        "PRESET\n"
        "PTOP [N]\n"
        "AUDIORESET\n"
        "AUDIOSTAT [window_samples]\n"
        "SCREENSHOT <path> [x y w h]  (.png=image, other=text dump)\n"
        "QUIT\n"
        "OK END\n");
}

static int llm_handle_command(c64_t *c64, llm_state_t *st,
                              uint8_t *fb, int width, int height, char *line) {
    char original_line[2048];
    snprintf(original_line, sizeof(original_line), "%s", line);

    char *argv[256];
    int argc = 0;

    for (char *tok = strtok(line, " \t"); tok && argc < 256; tok = strtok(NULL, " \t")) {
        argv[argc++] = tok;
    }
    if (argc == 0) return 0;

    if (!strcasecmp(argv[0], "HELP")) {
        llm_show_help(st);
        return 0;
    }

    if (!strcasecmp(argv[0], "QUIT")) {
        llm_sendf(st, "OK BYE\n");
        return 1;
    }

    if (!strcasecmp(argv[0], "KEYDOWN") || !strcasecmp(argv[0], "KEYUP") ||
        !strcasecmp(argv[0], "KEY")) {
        if (argc < 2) {
            llm_sendf(st, "ERR %s requires key token\n", argv[0]);
            return 0;
        }
        int key;
        if (!parse_key_token(argv[1], &key)) {
            llm_sendf(st, "ERR invalid key token: %s\n", argv[1]);
            return 0;
        }

        if (!strcasecmp(argv[0], "KEYDOWN")) {
            c64_key_down(c64, key);
            llm_sendf(st, "OK KEYDOWN key=$%02X\n", key & 0xFF);
        } else if (!strcasecmp(argv[0], "KEYUP")) {
            c64_key_up(c64, key);
            llm_sendf(st, "OK KEYUP key=$%02X\n", key & 0xFF);
        } else {
            c64_key_down(c64, key);
            c64_key_up(c64, key);
            llm_sendf(st, "OK KEY key=$%02X\n", key & 0xFF);
        }
        return 0;
    }

    if (!strcasecmp(argv[0], "TYPE")) {
        const char *p = original_line + 4;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0') {
            llm_sendf(st, "ERR TYPE requires text\n");
            return 0;
        }

        int sent = 0;
        while (*p) {
            int key = normalize_ascii_to_c64((unsigned char)*p);
            if (key) {
                c64_key_down(c64, key);
                c64_key_up(c64, key);
                sent++;
            }
            p++;
        }
        llm_sendf(st, "OK TYPE sent=%d\n", sent);
        return 0;
    }

    if (!strcasecmp(argv[0], "DUMP")) {
        llm_cpu_dump(c64, st);
        return 0;
    }

    if (!strcasecmp(argv[0], "CPUINT")) {
        llm_cpuint_dump(c64, st);
        return 0;
    }

    if (!strcasecmp(argv[0], "VIC")) {
        llm_vic_dump(c64, st);
        return 0;
    }

    if (!strcasecmp(argv[0], "CIA")) {
        if (argc != 2) {
            llm_sendf(st, "ERR CIA usage: CIA <1|2>\n");
            return 0;
        }
        uint32_t which = 0;
        if (!parse_u32(argv[1], &which) || (which != 1 && which != 2)) {
            llm_sendf(st, "ERR CIA requires 1 or 2\n");
            return 0;
        }
        llm_cia_dump(c64, st, (int)which);
        return 0;
    }

    if (!strcasecmp(argv[0], "CIATRACE")) {
        uint32_t limit = 32;
        if (argc >= 2) {
            if (!parse_u32(argv[1], &limit) || limit == 0 || limit > LLM_CIA_TRACE_CAP) {
                llm_sendf(st, "ERR CIATRACE invalid N (1..%d)\n", LLM_CIA_TRACE_CAP);
                return 0;
            }
        }
        if (argc > 2) {
            llm_sendf(st, "ERR CIATRACE usage: CIATRACE [N]\n");
            return 0;
        }
        llm_cia_trace_dump(st, limit);
        return 0;
    }

    if (!strcasecmp(argv[0], "DISASM")) {
        uint16_t addr = m6502_pc(&c64->cpu);
        uint32_t raw = 0;
        uint32_t count = 1;
        if (argc >= 2) {
            if (!parse_u32(argv[1], &raw) || raw > 0xFFFF) {
                llm_sendf(st, "ERR DISASM invalid addr\n");
                return 0;
            }
            addr = (uint16_t)raw;
        }
        if (argc >= 3) {
            if (!parse_u32(argv[2], &count) || count == 0 || count > 64) {
                llm_sendf(st, "ERR DISASM invalid count (1..64)\n");
                return 0;
            }
        }
        if (argc > 3) {
            llm_sendf(st, "ERR DISASM usage: DISASM [addr] [count]\n");
            return 0;
        }
        for (uint32_t i = 0; i < count; i++) {
            llm_send_disasm_line(c64, st, "INSN", addr);
            llm_insn_t insn = llm_decode_opcode(mem_rd(&c64->mem_cpu, addr));
            addr = (uint16_t)(addr + (insn.len ? insn.len : 1));
        }
        llm_sendf(st, "OK END\n");
        return 0;
    }

    if (!strcasecmp(argv[0], "JOY1") || !strcasecmp(argv[0], "JOY2")) {
        if (argc < 2) {
            llm_sendf(st, "ERR %s requires mask (0..31)\n", argv[0]);
            return 0;
        }
        uint32_t m;
        if (!parse_u32(argv[1], &m) || m > 0x1F) {
            llm_sendf(st, "ERR invalid joystick mask (0..31)\n");
            return 0;
        }
        if (!strcasecmp(argv[0], "JOY1")) st->joy1_mask = (uint8_t)m;
        else st->joy2_mask = (uint8_t)m;
        c64_joystick(c64, st->joy1_mask, st->joy2_mask);
        llm_sendf(st, "OK %s mask=$%02X joy1=$%02X joy2=$%02X\n",
                  argv[0], (unsigned)m, st->joy1_mask, st->joy2_mask);
        return 0;
    }

    if (!strcasecmp(argv[0], "TEXT")) {
        int x, y, w, h;
        if (!llm_parse_text_window_args(argc, argv, 1, &x, &y, &w, &h)) {
            llm_sendf(st, "ERR TEXT usage: TEXT [x y w h]\n");
            return 0;
        }
        llm_dump_text(c64, st, x, y, w, h);
        return 0;
    }

    if (!strcasecmp(argv[0], "STOP")) {
        st->stopped = 1;
        llm_sendf(st, "OK STOPPED\n");
        return 0;
    }

    if (!strcasecmp(argv[0], "PSTART")) {
        st->profiling_enabled = 1;
        llm_sendf(st, "OK PSTART enabled=1\n");
        return 0;
    }

    if (!strcasecmp(argv[0], "PSTOP")) {
        st->profiling_enabled = 0;
        llm_sendf(st, "OK PSTOP enabled=0\n");
        return 0;
    }

    if (!strcasecmp(argv[0], "PRESET")) {
        llm_profile_reset(st);
        llm_sendf(st, "OK PRESET cleared=%d\n", LLM_PROFILE_ADDR_COUNT);
        return 0;
    }

    if (!strcasecmp(argv[0], "PTOP")) {
        uint32_t limit = 16;
        if (argc >= 2) {
            if (!parse_u32(argv[1], &limit) || limit == 0 || limit > 1024) {
                llm_sendf(st, "ERR PTOP invalid N (1..1024)\n");
                return 0;
            }
        }
        if (argc > 2) {
            llm_sendf(st, "ERR PTOP usage: PTOP [N]\n");
            return 0;
        }

        if (!st->profile_hits) {
            llm_sendf(st, "ERR PTOP profiler not initialized\n");
            return 0;
        }

        uint32_t nonzero = 0;
        uint64_t total_hits = 0;
        for (uint32_t i = 0; i < LLM_PROFILE_ADDR_COUNT; i++) {
            uint64_t hits = st->profile_hits[i];
            if (hits) {
                nonzero++;
                total_hits += hits;
            }
        }
        if (nonzero == 0) {
            llm_sendf(st, "OK PTOP count=0 nonzero=0 total=0\n");
            llm_sendf(st, "OK END\n");
            return 0;
        }

        profile_entry_t *entries = malloc((size_t)nonzero * sizeof(profile_entry_t));
        if (!entries) {
            llm_sendf(st, "ERR PTOP out of memory\n");
            return 0;
        }

        uint32_t idx = 0;
        for (uint32_t i = 0; i < LLM_PROFILE_ADDR_COUNT; i++) {
            uint64_t hits = st->profile_hits[i];
            if (!hits) continue;
            entries[idx].addr = (uint16_t)i;
            entries[idx].hits = hits;
            idx++;
        }
        qsort(entries, nonzero, sizeof(profile_entry_t), llm_profile_entry_cmp_desc);
        if (limit > nonzero) limit = nonzero;

        llm_sendf(st,
            "OK PTOP count=%u nonzero=%u total=%llu profiling=%d\n",
            (unsigned)limit, (unsigned)nonzero, (unsigned long long)total_hits,
            st->profiling_enabled);
        for (uint32_t i = 0; i < limit; i++) {
            llm_sendf(st, "%2u addr=$%04X hits=%llu\n",
                      (unsigned)(i + 1), entries[i].addr,
                      (unsigned long long)entries[i].hits);
            llm_send_disasm_line(c64, st, "PTOP", entries[i].addr);
        }
        llm_sendf(st, "OK END\n");
        free(entries);
        return 0;
    }

    if (!strcasecmp(argv[0], "AUDIORESET")) {
#ifdef USE_AUDIO
        llm_audio_reset_metrics();
        llm_sendf(st, "OK AUDIORESET\n");
#else
        llm_sendf(st, "ERR AUDIORESET unavailable (audio not compiled)\n");
#endif
        return 0;
    }

    if (!strcasecmp(argv[0], "AUDIOSTAT")) {
#ifdef USE_AUDIO
        uint32_t window = 4096;
        if (argc >= 2) {
            uint32_t w = 0;
            if (!parse_u32(argv[1], &w) || w == 0) {
                llm_sendf(st, "ERR AUDIOSTAT invalid window\n");
                return 0;
            }
            window = w;
        }

        uint32_t available = llm_audio_ring_count;
        if (available == 0) {
            llm_sendf(st,
                      "OK AUDIOSTAT window=0 available=0 max_abs=0.000000 rms=0.000000 mean_abs=0.000000 total=%llu\n",
                      (unsigned long long)llm_audio_total_samples);
            return 0;
        }
        if (window > available) window = available;

        uint32_t start = (llm_audio_ring_pos + LLM_AUDIO_RING_SAMPLES - window) % LLM_AUDIO_RING_SAMPLES;
        float max_abs = 0.0f;
        double sum_sq = 0.0;
        double sum_abs = 0.0;
        for (uint32_t i = 0; i < window; i++) {
            uint32_t idx = (start + i) % LLM_AUDIO_RING_SAMPLES;
            float v = llm_audio_ring[idx];
            float a = fabsf(v);
            if (a > max_abs) max_abs = a;
            sum_sq += (double)v * (double)v;
            sum_abs += (double)a;
        }
        double rms = sqrt(sum_sq / (double)window);
        double mean_abs = sum_abs / (double)window;
        llm_sendf(st,
                  "OK AUDIOSTAT window=%u available=%u max_abs=%.6f rms=%.6f mean_abs=%.6f total=%llu\n",
                  window, available, (double)max_abs, rms, mean_abs,
                  (unsigned long long)llm_audio_total_samples);
#else
        llm_sendf(st, "ERR AUDIOSTAT unavailable (audio not compiled)\n");
#endif
        return 0;
    }

    if (!strcasecmp(argv[0], "RUN")) {
        if (argc >= 2) {
            uint16_t start_addr = 0;
            uint16_t sys_addr = 0;
            if (!llm_prepare_program_boot(c64, st, 600)) {
                llm_sendf(st, "ERR RUN boot did not reach BASIC READY\n");
                return 0;
            }
            if (!load_prg_file(c64, argv[1], &start_addr)) {
                llm_sendf(st, "ERR RUN failed to load %s\n", argv[1]);
                return 0;
            }
            if (llm_parse_basic_sys_start(c64, start_addr, &sys_addr)) {
                start_addr = sys_addr;
            }
            llm_start_machine_code(c64, start_addr);
            st->stopped = 0;
            llm_sendf(st,
                      "OK RUN loaded=%s start=$%04X running=1 boot=READY mode=MACHINE\n",
                      argv[1], start_addr);
            return 0;
        }
        st->stopped = 0;
        llm_sendf(st, "OK RUNNING\n");
        return 0;
    }

    if (!strcasecmp(argv[0], "RUNSYS")) {
        if (argc < 2) {
            llm_sendf(st, "ERR RUNSYS requires prg path or addr\n");
            return 0;
        }
        uint16_t start_addr = 0;
        uint16_t sys_addr = 0;
        uint32_t raw = 0;
        if (parse_u32(argv[1], &raw) && raw <= 0xFFFF) {
            start_addr = (uint16_t)raw;
        } else {
            if (!llm_prepare_program_boot(c64, st, 600)) {
                llm_sendf(st, "ERR RUNSYS boot did not reach BASIC READY\n");
                return 0;
            }
            if (!load_prg_file(c64, argv[1], &start_addr)) {
                llm_sendf(st, "ERR RUNSYS failed to load %s\n", argv[1]);
                return 0;
            }
            if (llm_parse_basic_sys_start(c64, start_addr, &sys_addr)) {
                start_addr = sys_addr;
            }
            llm_start_machine_code(c64, start_addr);
            st->stopped = 0;
            llm_sendf(st,
                "OK RUNSYS loaded=%s sys=$%04X running=1 via=MACHINE\n",
                argv[1], start_addr);
            return 0;
        }
        llm_start_machine_code(c64, start_addr);
        st->stopped = 0;
        llm_sendf(st, "OK RUNSYS start=$%04X running=1\n", start_addr);
        return 0;
    }

    if (!strcasecmp(argv[0], "RESET")) {
        if (!llm_prepare_program_boot(c64, st, 600)) {
            llm_sendf(st, "ERR RESET boot did not reach BASIC READY\n");
            return 0;
        }
        llm_sendf(st, "OK RESET ready=1 pc=$%04X\n", m6502_pc(&c64->cpu));
        return 0;
    }

    if (!strcasecmp(argv[0], "NEXT")) {
        st->stopped = 1;
        break_hit_t hit;
        llm_step_instruction(c64, st, &hit);
        if (hit.hit) {
            llm_sendf(st,
                "STOPPED BREAKPOINT id=%d type=%s addr=$%04X pc=$%04X\n",
                hit.id, bp_type_name(hit.type), hit.addr, m6502_pc(&c64->cpu));
        }
        llm_cpu_dump(c64, st);
        return 0;
    }

    if (!strcasecmp(argv[0], "FRAME")) {
        st->stopped = 1;
        c64_exec(c64, FRAME_USEC);
        st->frame_counter++;
        llm_cpu_dump(c64, st);
        return 0;
    }

    if (!strcasecmp(argv[0], "JUMP")) {
        if (argc < 2) {
            llm_sendf(st, "ERR JUMP requires address\n");
            return 0;
        }
        uint32_t a;
        if (!parse_u32(argv[1], &a) || a > 0xFFFF) {
            llm_sendf(st, "ERR invalid address\n");
            return 0;
        }
        uint16_t addr = (uint16_t)a;
        m6502_set_pc(&c64->cpu, addr);
        c64->pins = M6502_SYNC;
        M6502_SET_ADDR(c64->pins, addr);
        M6502_SET_DATA(c64->pins, mem_rd(&c64->mem_cpu, addr));
        llm_sendf(st, "OK JUMP pc=$%04X\n", addr);
        return 0;
    }

    if (!strcasecmp(argv[0], "MEMGET")) {
        if (argc < 3) {
            llm_sendf(st, "ERR MEMGET requires addr len\n");
            return 0;
        }
        uint32_t a, n;
        if (!parse_u32(argv[1], &a) || !parse_u32(argv[2], &n) || a > 0xFFFF) {
            llm_sendf(st, "ERR MEMGET invalid args\n");
            return 0;
        }
        if (n == 0) {
            llm_sendf(st, "OK MEMGET empty\n");
            return 0;
        }
        if (n > 4096) n = 4096;

        llm_sendf(st, "OK MEMGET addr=$%04X len=%u\n", (unsigned)a, (unsigned)n);
        for (uint32_t i = 0; i < n; i += 16) {
            uint16_t addr = (uint16_t)((a + i) & 0xFFFF);
            llm_sendf(st, "%04X:", addr);
            for (uint32_t j = 0; j < 16 && (i + j) < n; j++) {
                uint16_t cur = (uint16_t)((a + i + j) & 0xFFFF);
                llm_sendf(st, " %02X", mem_rd(&c64->mem_cpu, cur));
            }
            llm_sendf(st, "\n");
        }
        llm_sendf(st, "OK END\n");
        return 0;
    }

    if (!strcasecmp(argv[0], "MEMSET")) {
        if (argc < 3) {
            llm_sendf(st, "ERR MEMSET requires addr and at least one byte\n");
            return 0;
        }
        uint32_t a;
        if (!parse_u32(argv[1], &a) || a > 0xFFFF) {
            llm_sendf(st, "ERR MEMSET invalid address\n");
            return 0;
        }

        uint16_t addr = (uint16_t)a;
        int written = 0;
        for (int i = 2; i < argc; i++) {
            uint32_t b;
            if (!parse_u32(argv[i], &b) || b > 0xFF) {
                llm_sendf(st, "ERR MEMSET invalid byte: %s\n", argv[i]);
                return 0;
            }
            mem_wr(&c64->mem_cpu, addr, (uint8_t)b);
            addr = (uint16_t)(addr + 1);
            written++;
        }
        llm_sendf(st, "OK MEMSET wrote=%d\n", written);
        return 0;
    }

    if (!strcasecmp(argv[0], "BREAKPOINT")) {
        if (argc < 2) {
            llm_sendf(st, "ERR BREAKPOINT requires subcommand\n");
            return 0;
        }

        if (!strcasecmp(argv[1], "LIST")) {
            llm_sendf(st, "OK BREAKPOINT LIST count=%d\n", st->bp_count);
            for (int i = 0; i < st->bp_count; i++) {
                breakpoint_t *bp = &st->bps[i];
                llm_sendf(st, "id=%d type=%s addr=$%04X enabled=%d\n",
                          bp->id, bp_type_name(bp->type), bp->addr, bp->enabled);
            }
            llm_sendf(st, "OK END\n");
            return 0;
        }

        if (!strcasecmp(argv[1], "DEL")) {
            if (argc < 3) {
                llm_sendf(st, "ERR BREAKPOINT DEL requires id\n");
                return 0;
            }
            uint32_t id;
            if (!parse_u32(argv[2], &id)) {
                llm_sendf(st, "ERR invalid breakpoint id\n");
                return 0;
            }
            if (!llm_delete_breakpoint(st, (int)id)) {
                llm_sendf(st, "ERR breakpoint not found\n");
                return 0;
            }
            llm_sendf(st, "OK BREAKPOINT DEL id=%u\n", (unsigned)id);
            return 0;
        }

        if (argc < 3) {
            llm_sendf(st, "ERR BREAKPOINT %s requires addr\n", argv[1]);
            return 0;
        }

        uint32_t a;
        if (!parse_u32(argv[2], &a) || a > 0xFFFF) {
            llm_sendf(st, "ERR invalid address\n");
            return 0;
        }

        bp_type_t type;
        if (!strcasecmp(argv[1], "PC")) type = BP_PC;
        else if (!strcasecmp(argv[1], "READ")) type = BP_READ;
        else if (!strcasecmp(argv[1], "WRITE")) type = BP_WRITE;
        else {
            llm_sendf(st, "ERR unknown BREAKPOINT type\n");
            return 0;
        }

        int id = llm_add_breakpoint(st, type, (uint16_t)a);
        if (id < 0) {
            llm_sendf(st, "ERR breakpoint table full\n");
            return 0;
        }
        llm_sendf(st, "OK BREAKPOINT id=%d type=%s addr=$%04X\n",
                  id, bp_type_name(type), (unsigned)a);
        return 0;
    }

    if (!strcasecmp(argv[0], "SCREENSHOT")) {
        if (argc < 2) {
            llm_sendf(st, "ERR SCREENSHOT requires path\n");
            return 0;
        }

        int win_x, win_y, win_w, win_h;
        if (!llm_parse_window_args(argc, argv, 2, width, height, &win_x, &win_y, &win_w, &win_h)) {
            llm_sendf(st, "ERR SCREENSHOT usage: SCREENSHOT <path> [x y w h]\n");
            return 0;
        }
        int write_ok = 0;
        const char *fmt = "C64_16COLOR_CHARMAP";
        if (llm_path_has_ext_ci(argv[1], ".png")) {
            fmt = "PNG_RGB24";
            write_ok = llm_write_screenshot_png(argv[1], fb, width, win_x, win_y, win_w, win_h);
        } else {
            write_ok = llm_write_screenshot_text(argv[1], fb, width, height, win_x, win_y, win_w, win_h);
        }

        if (!write_ok) {
            llm_sendf(st, "ERR SCREENSHOT failed path=%s\n", argv[1]);
            return 0;
        }
        llm_sendf(st,
            "OK SCREENSHOT path=%s format=%s window=%d,%d,%d,%d\n",
            argv[1], fmt, win_x, win_y, win_w, win_h);
        return 0;
    }

    llm_sendf(st, "ERR unknown command\n");
    return 0;
}

static int llm_poll_client(c64_t *c64, llm_state_t *st,
                           uint8_t *fb, int width, int height) {
    if (st->client_fd < 0) return 0;

    ssize_t n = read(st->client_fd, st->rxbuf + st->rxlen,
                     sizeof(st->rxbuf) - st->rxlen - 1);

    if (n == 0) {
        llm_close_client(st);
        return 0;
    }

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        llm_close_client(st);
        return 0;
    }

    st->rxlen += (size_t)n;
    st->rxbuf[st->rxlen] = '\0';

    char *line_start = st->rxbuf;
    while (1) {
        char *nl = strchr(line_start, '\n');
        if (!nl) break;
        *nl = '\0';

        while (*line_start == ' ' || *line_start == '\t' || *line_start == '\r')
            line_start++;

        size_t len = strlen(line_start);
        while (len && (line_start[len - 1] == '\r' || line_start[len - 1] == ' ' || line_start[len - 1] == '\t')) {
            line_start[--len] = '\0';
        }

        if (len > 0) {
            char cmdline[2048];
            snprintf(cmdline, sizeof(cmdline), "%s", line_start);
            if (llm_handle_command(c64, st, fb, width, height, cmdline))
                return 1;
        }

        line_start = nl + 1;
    }

    size_t consumed = (size_t)(line_start - st->rxbuf);
    if (consumed > 0 && consumed <= st->rxlen) {
        memmove(st->rxbuf, st->rxbuf + consumed, st->rxlen - consumed);
        st->rxlen -= consumed;
    }

    return 0;
}

static int run_llm_server(c64_t *c64, uint8_t *fb, int width, int height) {
    llm_state_t st;
    memset(&st, 0, sizeof(st));
    st.listen_fd = -1;
    st.client_fd = -1;
    st.stopped = !EmuConfig.bas_mode;
    st.next_bp_id = 1;
    st.joy1_mask = 0;
    st.joy2_mask = 0;
    if (!llm_profile_init(&st)) {
        fprintf(stderr, "Failed to allocate LLM profiler counters\n");
        return 1;
    }

    if (!llm_setup_socket(&st, EmuConfig.llm_socket_path)) {
        fprintf(stderr, "Failed to create LLM socket at %s: %s\n",
                EmuConfig.llm_socket_path, strerror(errno));
        return 1;
    }

    printf("C64-LLM socket listening at %s\n", EmuConfig.llm_socket_path);
    printf("Default state: %s. Connect with: nc -U %s\n",
           st.stopped ? "STOPPED" : "RUNNING", EmuConfig.llm_socket_path);

    if (EmuConfig.prg_filename && !EmuConfig.bas_mode) {
        uint16_t start_addr = 0;
        if (load_prg_file(c64, EmuConfig.prg_filename, &start_addr)) {
            printf("Quickloaded %s at $%04X\n", EmuConfig.prg_filename, start_addr);
        } else {
            printf("Warning: failed to quickload %s\n", EmuConfig.prg_filename);
        }
    }

    int quit = 0;
    while (!quit) {
        llm_accept_client(&st);
        quit = llm_poll_client(c64, &st, fb, width, height);
        if (quit) break;

        if (!st.stopped) {
            llm_run_instruction_slice(c64, &st, 2000);
        } else {
            usleep(1000);
        }
    }

    llm_close_client(&st);
    if (st.listen_fd >= 0) close(st.listen_fd);
    unlink(st.sock_path);
    llm_profile_shutdown(&st);
    return 0;
}

/* ============================ main ============================ */

int main(int argc, char **argv) {
    c64_t c64;
    c64_desc_t c64_desc = {0};

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;

    parse_config(argc, argv);
    signal(SIGPIPE, SIG_IGN);

#ifdef USE_AUDIO
    void *audio_user_data = audio_init();
    if (audio_user_data == NULL) {
        fprintf(stderr, "Audio initialization failed\n");
        return 1;
    }
    llm_audio_reset_metrics();
    chips_audio_callback_t audio_cb;
    audio_cb.func = llm_audio_tap_from_emulator;
    audio_cb.user_data = audio_user_data;
    c64_desc.audio.callback = audio_cb;
    c64_desc.audio.sample_rate = 44100;
    /* Lower callback chunk to reduce audible event latency. */
    c64_desc.audio.num_samples = 256;
#endif

    const int width = _C64_SCREEN_WIDTH;
    const int height = _C64_SCREEN_HEIGHT;
    uint8_t *fb = malloc((size_t)width * (size_t)height * 3);
    if (!fb) {
        fprintf(stderr, "Unable to allocate framebuffer\n");
#ifdef USE_AUDIO
        audio_cleanup(audio_user_data);
#endif
        return 1;
    }
    memset(fb, 0, (size_t)width * (size_t)height * 3);

    c64_desc.roms.chars.ptr = dump_c64_char_bin;
    c64_desc.roms.chars.size = sizeof(dump_c64_char_bin);
    c64_desc.roms.basic.ptr = dump_c64_basic_bin;
    c64_desc.roms.basic.size = sizeof(dump_c64_basic_bin);
    c64_desc.roms.kernal.ptr = dump_c64_kernalv3_bin;
    c64_desc.roms.kernal.size = sizeof(dump_c64_kernalv3_bin);
    c64_desc.crt_set_pixel = crt_set_pixel;
    c64_desc.crt_set_pixel_fb = fb;

    c64_init(&c64, &c64_desc);

    chips_display_info_t di = c64_display_info(&c64);
    printf("FB total size %dx%d\n", di.frame.dim.width, di.frame.dim.height);
    printf("FB screen %dx%d at %dx%d\n", di.screen.width, di.screen.height, di.screen.x, di.screen.y);

    int rc = 0;
    llm_state_t llm = {0};
    int llm_active = 0;

    if (EmuConfig.bas_mode) {
        if (!llm_wait_basic_ready(&c64, NULL, 600)) {
            fprintf(stderr, "BASIC boot did not reach READY\n");
            rc = 1;
            goto cleanup;
        }
        if (!load_basic_file(&c64, EmuConfig.prg_filename)) {
            rc = 1;
            goto cleanup;
        }
        c64_basic_run(&c64);
        printf("BASIC loaded: %s, injected RUN\n", EmuConfig.prg_filename);
    }

    if (EmuConfig.llm_mode && !EmuConfig.sdl_mode) {
        rc = run_llm_server(&c64, fb, width, height);
        goto cleanup;
    }

    if (EmuConfig.sdl_mode) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            rc = 1;
            goto cleanup;
        }

        int window_w = (int)(width * EmuConfig.zoom);
        int window_h = (int)(height * EmuConfig.zoom);
        if (window_w < 1) window_w = 1;
        if (window_h < 1) window_h = 1;

        window = SDL_CreateWindow("c64-llm (SDL)",
                                  SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED,
                                  window_w, window_h,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window) {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            rc = 1;
            goto cleanup;
        }

        renderer = SDL_CreateRenderer(window, -1,
                                      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (!renderer) {
                fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
                rc = 1;
                goto cleanup;
            }
        }

        texture = SDL_CreateTexture(renderer,
                                    SDL_PIXELFORMAT_RGB24,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    width, height);
        if (!texture) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            rc = 1;
            goto cleanup;
        }

        /* SDL doesn't always activate terminal-launched windows on macOS, so
         * explicitly raise the window and optionally ask AppKit to foreground
         * the process in the macOS build.
         */
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
#if defined(__APPLE__) && defined(USE_MACOS_APPKIT_ACTIVATE)
        c64_llm_macos_activate_app();
#endif
        SDL_RenderSetLogicalSize(renderer, width, height);
        printf("C64 emulator started in SDL mode. Press ESC to quit.\n");
        if (EmuConfig.llm_mode) {
            memset(&llm, 0, sizeof(llm));
            llm.listen_fd = -1;
            llm.client_fd = -1;
            llm.stopped = 0; /* Hybrid starts running for manual reproduction. */
            llm.next_bp_id = 1;
            llm.joy1_mask = 0;
            llm.joy2_mask = 0;
            if (!llm_profile_init(&llm)) {
                fprintf(stderr, "Failed to allocate LLM profiler counters\n");
                rc = 1;
                goto cleanup;
            }

            if (!llm_setup_socket(&llm, EmuConfig.llm_socket_path)) {
                fprintf(stderr, "Failed to create LLM socket at %s: %s\n",
                        EmuConfig.llm_socket_path, strerror(errno));
                llm_profile_shutdown(&llm);
                rc = 1;
                goto cleanup;
            }
            llm_active = 1;
            printf("C64-LLM socket listening at %s\n", EmuConfig.llm_socket_path);
            printf("Hybrid SDL+LLM mode: RUNNING by default\n");
            printf("Manual debug stop hotkey: Ctrl+Shift\n");
            printf("Attach with: nc -U %s\n", EmuConfig.llm_socket_path);
        }

        int quit_requested = 0;
        int frame = 0;
        int prg_loaded = EmuConfig.bas_mode;

        if (EmuConfig.prg_filename && !prg_loaded) {
            printf("PRG queued: %s (waiting for BASIC READY)\n", EmuConfig.prg_filename);
        }
        if (EmuConfig.auto_run && !prg_loaded) {
            if (!EmuConfig.prg_filename) {
                printf("Warning: --run provided without PRG, ignoring auto-run.\n");
            } else {
                int boot_frames = 0;
                while (!quit_requested &&
                       !c64_is_basic_ready_pc(m6502_pc(&c64.cpu)) &&
                       boot_frames < 2000) {
                    c64_exec(&c64, FRAME_USEC);
                    boot_frames++;

                    if (llm_active) {
                        llm.frame_counter++;
                        llm_accept_client(&llm);
                        quit_requested = llm_poll_client(&c64, &llm, fb, width, height);
                        if (quit_requested) break;
                    }

                    int manual_stop = 0;
                    quit_requested = process_sdl_events(&c64, &manual_stop);
                    if (manual_stop && llm_active) {
                        llm.stopped = 1;
                        llm_sendf(&llm, "STOPPED MANUAL key=CTRL+SHIFT pc=$%04X\n",
                                  m6502_pc(&c64.cpu));
                        printf("STOPPED MANUAL key=CTRL+SHIFT pc=$%04X\n",
                               m6502_pc(&c64.cpu));
                    }
                }

                uint16_t start_addr = 0;
                if (!quit_requested && load_prg_file(&c64, EmuConfig.prg_filename, &start_addr)) {
                    c64_basic_run(&c64);
                    prg_loaded = 1;
                    printf("Auto-run: booted in %d frames, loaded %s at $%04X, injected RUN\n",
                           boot_frames, EmuConfig.prg_filename, start_addr);
                } else if (!quit_requested) {
                    printf("Auto-run failed to load %s\n", EmuConfig.prg_filename);
                }
            }
        }

        uint64_t total_us_emulated = 0;
        uint64_t total_us_start = time_us();

        while (!quit_requested) {
            if (llm_active) {
                llm_accept_client(&llm);
                quit_requested = llm_poll_client(&c64, &llm, fb, width, height);
                if (quit_requested) break;
            }

            int manual_stop = 0;
            quit_requested = process_sdl_events(&c64, &manual_stop);
            if (quit_requested) break;
            if (manual_stop && llm_active) {
                llm.stopped = 1;
                llm_sendf(&llm, "STOPPED MANUAL key=CTRL+SHIFT pc=$%04X\n",
                          m6502_pc(&c64.cpu));
                printf("STOPPED MANUAL key=CTRL+SHIFT pc=$%04X\n",
                       m6502_pc(&c64.cpu));
            }

            int emulated_frame = 0;
            if (!llm_active || !llm.stopped) {
                if (llm_active && (llm.bp_count > 0 || llm.profiling_enabled)) {
                    /* Hybrid debug path: when breakpoints/profiler are active,
                     * run instruction stepping so PC/read/write breakpoints can
                     * stop execution while RUNNING.
                     */
                    llm_run_instruction_slice(&c64, &llm,
                                             LLM_HYBRID_DEBUG_SLICE_INSTR);
                } else {
                    /* Hybrid normal path: keep full-speed audio/video pacing. */
                    c64_exec(&c64, FRAME_USEC);
                }
                total_us_emulated += FRAME_USEC;
                frame++;
                emulated_frame = 1;
                if (llm_active) llm.frame_counter++;
            }

            SDL_UpdateTexture(texture, NULL, fb, width * 3);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);

            if (!prg_loaded && EmuConfig.prg_filename) {
                uint16_t pc = m6502_pc(&c64.cpu);
                int ready = c64_is_basic_ready_pc(pc);
                int timeout = frame >= 250; /* ~5 seconds at 50Hz */
                if (ready || timeout) {
                    uint16_t start_addr = 0;
                    if (load_prg_file(&c64, EmuConfig.prg_filename, &start_addr)) {
                        printf("Loaded %s at $%04X%s\n",
                               EmuConfig.prg_filename,
                               start_addr,
                               ready ? " (BASIC READY)" : " (timeout)");
                    } else {
                        printf("Failed to load %s\n", EmuConfig.prg_filename);
                    }
                    prg_loaded = 1;
                }
            }

            if (emulated_frame) {
                uint64_t total_us_real = time_us() - total_us_start;
                int64_t delta_us = (int64_t)total_us_emulated - (int64_t)total_us_real;
                int64_t to_sleep_us = FRAME_USEC + delta_us;
                if (to_sleep_us > 0) usleep((useconds_t)to_sleep_us);
            } else {
                usleep(1000);
            }
        }
    }

cleanup:
#ifdef USE_AUDIO
    audio_cleanup(audio_user_data);
#endif

    if (llm_active) {
        llm_close_client(&llm);
        if (llm.listen_fd >= 0) close(llm.listen_fd);
        unlink(llm.sock_path);
        llm_profile_shutdown(&llm);
    }
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (EmuConfig.sdl_mode) SDL_Quit();

    free(fb);
    printf("C64 emulator terminated.\n");
    return rc;
}
