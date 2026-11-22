# Origin

This repository was extracted from the `c64-llm/` subdirectory of the Jetpac
workspace at:

- `/Users/antirez/hack/2026/jetpac.c`

Extraction method:

- `git subtree split --prefix=c64-llm`
- new standalone repository created at:
  - `/Users/antirez/hack/2026/c64-llm`

Standalone additions after extraction:

- bundled `describe_image.sh` helper used by the screenshot workflow in
  `README.md`
- refreshed `README.md` so the documented LLM protocol matches the current
  debugger commands (`CPUINT`, `CIA 1|2`, `CIATRACE`)

The standalone repository is intended to continue emulator and tooling work
independently from the Jetpac game repository.
