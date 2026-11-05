# Audio tests in PCSX2

Self-checking EE programs that run headless in PCSX2. Each prints `<name>: PASS` or `<name>: FAIL`
through `kputs`; `run.sh` builds it with ps2build, runs it and reports the result. It is the same
runner as sdk-core's `tests/run.sh`, so `--llvm`, `PS2BUILD`, `PCSX2` and `TIMEOUT` work the same way.

```
TIMEOUT=90 tests/pcsx2/run.sh            # GCC
TIMEOUT=90 tests/pcsx2/run.sh --llvm     # LLVM
```

The runner is a bash script, so run it on Linux (or any system with bash); the Windows install has no bash.

| Test | Covers |
|---|---|
| `adpcm` | Loading APCM (`.adp`) sounds, playing them on mixer channels, pan, pitch, master volume, play length against the clip length, queued commands, freeing and shutdown. The clips are generated silence, so no files are needed |
