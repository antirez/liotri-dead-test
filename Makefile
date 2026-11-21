TASS ?= 64tass
PYTHON ?= python3

all: liotri.crt

liotri.crt: src/liotri.asm src/cpu.asm src/font.asm src/harness.asm src/sprites.asm
	$(TASS) -a -B -b -o $@ src/liotri.asm

test: liotri.crt
	$(PYTHON) tests/liotri.py

.PHONY: all test
