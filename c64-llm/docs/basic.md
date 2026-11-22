# BASIC source loading

`--bas file.bas` converts a numbered text listing into the usual linked BASIC
lines at `$0801`, then uses the existing PRG loader and keyboard-buffer `RUN`.
The program executes in the emulator's BASIC V2 ROM. The same startup path
serves SDL, SDL with socket control, and headless socket control.

The tokenizer follows the ROM's keyword order and handles adjacent keywords,
`?`, `PRINT#`, `INPUT#`, `GO TO`, shifted-letter abbreviations, and the literal
text inside strings, `DATA`, and `REM`. The ROM's
[tokenizer disassembly](https://skoolkid.github.io/sk6502/c64rom/asm/A57C.html)
was used as a reference. PETSCII escapes are decoded before tokenization; for
example, `G{$cf}` supplies the shifted O in the C64 abbreviation for `GOTO`.

`make test` compares all 76 keywords and representative combinations against
the actual ROM tokenizer, running that routine on an isolated 6502. It also
checks linked lines, sorting, replacement/deletion, line endings, rejected
input, and an executable program exercising arrays, loops, `DEF FN`, string
and arithmetic functions, `DATA`/`READ`/`RESTORE`, computed `GOSUB`, and `POKE`.
Invalid files are rejected before loading any of their contents into C64 RAM.

Three online programs were downloaded unchanged and tested through the actual
headless `--bas` startup path:

| Program | Source lines | Execution checked |
| --- | ---: | --- |
| [AMAZING](https://github.com/coding-horror/basic-computer-games/blob/main/02_Amazing/amazing.bas) | 143 | Entered 5,5; generated a maze and returned to READY. |
| [Hammurabi](https://github.com/coding-horror/basic-computer-games/blob/main/43_Hammurabi/hammurabi.bas) | 119 | Entered buy/sell/feed/plant decisions, advanced to year 2, and exited normally. |
| [Snake](https://github.com/greggelong/basicSnake/blob/main/snakec.bas) | 55 | Initialized the screen and arrays, then moved the snake with keyboard input. |

Every source line in these programs also matched the ROM's tokenization.
The loader tests pass under AddressSanitizer and UndefinedBehaviorSanitizer,
as do `make test` and the macOS build. SDL startup was checked with its dummy
video driver, including `--bas` combined with `--run` and `--llm`.
The test binary accepts local listings to repeat this comparison:

```sh
make tests/basic
./tests/basic /path/to/amazing.bas /path/to/hammurabi.bas /path/to/snakec.bas
```

No network access, display window, or audio device is needed for `make test`.
