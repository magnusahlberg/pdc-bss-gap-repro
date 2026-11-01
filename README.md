# pdc reserves too little memory when .bss is 16-byte aligned

Minimal repro for a Playdate C SDK bug (seen with SDK 3.1.1, firmware 3.1.1):
when a game's `.bss` contains a 16-byte-aligned object, the last bytes of
`.bss` end up outside the memory the OS reserves for the game. The heap then
overwrites those globals. On the device this shows up as random corruption of
the game's last few global variables; the Simulator is unaffected.

## Cause

- The SDK linker script (`C_API/buildsupport/link_map.ld`) ends `.data` with
  `. = ALIGN(4)`.
- A 16-byte-aligned object in `.bss` (a cglm `mat4`, any
  `__attribute__((aligned(16)))` type) makes the linker start `.bss` on a
  16-byte boundary, so there can be a 4, 8 or 12-byte gap between the end of
  `.data` and the start of `.bss`.
- pdc writes the game's memory size into the `pdex.bin` header (offset
  `0x24`) as *file size + `.bss` size*. That leaves the gap out, so the
  reserved memory ends up to 12 bytes before the real end of `.bss`.
- The last `.bss` word is always `pdrealloc` from the SDK's `setup.c`, so
  that pointer is always among the bytes outside the reserved memory.

## Reproduce

```bash
make layout
```

prints the ELF layout next to the `pdex.bin` header:

```
.data ends        0x00005214
.bss starts       0x00005220 (size 0x00000344)
.bss ends         0x00005564
pdex.bin header:  file size 0x00005214, memory size 0x00005558
=> 12 bytes of .bss outside the reserved memory
```

Expected: memory size `0x5564`, the end of `.bss`.

On a device, install `BssGap.pdx` and launch it. It reads its own `pdex.bin`
header, shows the same numbers computed from its load address, and checks
whether two canary words and `pdrealloc` (the 12 bytes outside the reserved
memory) survive some ordinary heap use (loading a system font, 32 `malloc`s).

## Control

The same program built with `link_map_fixed.ld`, which only pads `.data` to
16 bytes (`. = ALIGN(16)` before `__data_end__`), has no gap:

```bash
make clean
make layout LDSCRIPT=$PWD/link_map_fixed.ld
```

```
pdex.bin header:  file size 0x00005220, memory size 0x00005564
=> 0 bytes of .bss outside the reserved memory
```

## Suggested fix

Either:

- have pdc compute the memory size as the end of `.bss` (its address + size)
  rather than file size + `.bss` size, or
- pad `.data` in `link_map.ld` to the largest `.bss` alignment, as
  `link_map_fixed.ld` does.

## Workaround for games

Use a copy of the linker script with `.data` padded to 16, set after the
include in the game's Makefile:

```make
include $(SDK)/C_API/buildsupport/common.mk
LDSCRIPT := $(CURDIR)/link_map_fixed.ld
```

## How it was found

A 3D engine using cglm crashed on the device in its title screen: a menu
index that was the second-to-last `.bss` variable read garbage, and the game
used it to index an array of strings. The crash moved around as globals were
added or removed, and never happened in the Simulator.

## Notes

- The header layout (`0x20` file size, `0x24` memory size, `0x28` entry point,
  all little-endian) was inferred by matching the values against the ELF, not
  from documentation.
- `DATA_WORDS` (default 1) sets how many initialized words are in `.data`;
  with SDK 3.1.1 the default gives the largest possible gap, 12 bytes.
