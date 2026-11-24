SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

ifeq ($(shell uname -s),Darwin)
MACOS_AUDIO_TEST := tests/audio-macos
endif

.PHONY: all noaudio macos linux-pulseaudio linux-alsa dram-test-crt test clean

all:
	@echo "Available targets:"
	@echo "  noaudio         - Build without audio support"
	@echo "  sid-keys        - Build SDL SID keyboard sketch tool"
	@echo "  macos           - Build with macOS audio support"
	@echo "  linux-alsa      - Build with Linux ALSA audio support"
	@echo "  linux-pulseaudio - Build with Linux PulseAudio support"
	@echo "  dram-test-crt   - Build the C64 DRAM-test cartridge image"
	@echo "  test            - Run BASIC, timing, SID and platform audio regressions"
	@echo "  clean           - Remove build artifacts"

noaudio: c64-llm
c64-llm: c64-llm.c $(wildcard *.h)
	gcc -O2 -Wall -W $(SDL_CFLAGS) c64-llm.c -o c64-llm -g -ggdb $(SDL_LIBS) -lm
sid-keys: sid-keys.c m6581.h
	gcc -O2 -Wall -W $(SDL_CFLAGS) sid-keys.c -o sid-keys -g -ggdb $(SDL_LIBS) -lm
macos: c64-llm.c audio_macos.c platform_macos_focus.m
	gcc -D USE_AUDIO -D USE_MACOS_APPKIT_ACTIVATE -O2 -Wall -W $(SDL_CFLAGS) c64-llm.c audio_macos.c platform_macos_focus.m -o c64-llm -g -ggdb $(SDL_LIBS) -lm -framework AudioToolbox -framework CoreFoundation -framework AppKit
linux-pulseaudio: c64-llm.c audio_linux_pulse.c
	gcc -D USE_AUDIO -O2 -Wall -W $(SDL_CFLAGS) c64-llm.c audio_linux_pulse.c -o c64-llm -g -ggdb $(SDL_LIBS) -lm -lpulse -lpulse-simple
linux-alsa: c64-llm.c audio_linux_alsa.c
	gcc -D USE_AUDIO -O2 -Wall -W $(SDL_CFLAGS) c64-llm.c audio_linux_alsa.c -o c64-llm -g -ggdb $(SDL_LIBS) -lm -lasound -lpthread
dram-test-crt: tools/build_dram_test_crt.py
	python3 tools/build_dram_test_crt.py
test: tests/basic tests/cpu-timing tests/vic-timing tests/sid $(MACOS_AUDIO_TEST)
	./tests/basic
	./tests/cpu-timing
	./tests/vic-timing
	./tests/sid
	$(if $(MACOS_AUDIO_TEST),./$(MACOS_AUDIO_TEST))
tests/basic: tests/basic.c c64-llm.c $(wildcard *.h)
	$(CC) -O2 -Wall -W $(SDL_CFLAGS) tests/basic.c -o tests/basic $(SDL_LIBS) -lm
tests/cpu-timing: tests/cpu_timing.c m6502.h
	$(CC) -O2 -Wall -W tests/cpu_timing.c -o tests/cpu-timing
tests/vic-timing: tests/vic_timing.c m6569.h chips_common.h
	$(CC) -O2 -Wall -W tests/vic_timing.c -o tests/vic-timing
tests/audio-macos: tests/audio_macos.c audio_macos.c
	$(CC) -O2 -Wall -W tests/audio_macos.c -o tests/audio-macos -framework AudioToolbox
tests/sid: tests/sid.c m6581.h
	$(CC) -O2 -Wall -W tests/sid.c -o tests/sid -lm
clean:
	rm -f c64-llm sid-keys tests/basic tests/cpu-timing tests/vic-timing tests/audio-macos tests/sid
