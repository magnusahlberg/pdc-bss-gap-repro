# pdc reserves too little memory when .bss is more than 4-byte aligned

Minimal repro for a Playdate C SDK bug (SDK 3.1.2, also seen with 2.7.2;
firmware 3.1.1; macOS, `arm-none-eabi-gcc` 9-2019-q4-major): when the start
of a game's `.bss` needs more than 4-byte alignment, which happens with a
single `double` or `int64_t` global, the last bytes of `.bss` end up outside
the memory the OS reserves for the game. The last word of `.bss` is always
`pdrealloc` from the SDK's `setup.c`, so that pointer is always among the
bytes affected.

## Cause

- The SDK linker script (`C_API/buildsupport/link_map.ld`) ends `.data` with
  `. = ALIGN(4)`.
- `.bss` starts at its own alignment: 8 with a `double` or `int64_t` global,
  16 with a 16-byte-aligned type such as a cglm `mat4`. Depending on where
  `.data` happens to end, that leaves a 4, 8 or 12-byte gap between the end of
  `.data` and the start of `.bss`.
- pdc writes the game's memory size into the `pdex.bin` header as *file size
  + `.bss` size*. That leaves the gap out, so the memory size is short by the
  size of the gap.

## Reproduce

```bash
make layout                     # 16-byte-aligned global
```

```
.data ends        0x00005214
.bss starts       0x00005220 (size 0x00000344)
.bss ends         0x00005564
pdex.bin header:  file size 0x00005214, memory size 0x00005558
=> 12 bytes of .bss outside the reserved memory
```

```bash
make clean
make layout UDEFS=-DUSE_DOUBLE  # plain double globals instead
```

```
.data ends        0x00005224
.bss starts       0x00005228 (size 0x00000344)
.bss ends         0x0000556c
pdex.bin header:  file size 0x00005224, memory size 0x00005568
=> 4 bytes of .bss outside the reserved memory
```

Expected: a memory size that reaches the end of `.bss` (`0x5564` and
`0x556c`).

`make layout` only uses the ELF (via `nm` and `objdump`) and the header pdc
wrote. The header layout (`0x20` file size, `0x24` memory size, `0x28` entry
point, little-endian) was inferred by matching values against the ELF; the
on-device check below agrees with it.

## On a device

`BssGap.pdx` (the default, 16-byte-aligned build) reads its own `pdex.bin`
header and shows the same numbers computed from its load address. On a
Playdate with firmware 3.1.1 it reports **12 bytes of .bss outside the
reserved memory**, matching `make layout`.

It also places canary words in the affected bytes and checks them after some
heap use (loading a system font, 32 `malloc`s). In this small program they
were not overwritten in a short run: whether the bytes get overwritten depends
on how the game uses memory. These checks rely on internals (the header
format, `pdrealloc` being the last `.bss` word) and are only diagnostics.

## How it was found

A 3D engine using cglm crashed on the device on its title screen, with about
800 KB on the heap. A menu index, the second-to-last `.bss` variable and inside
the gap, read garbage, and the game used it to index an array of strings. The
crash came back as globals were added or removed, never happened in the
Simulator, and the same code ran clean under AddressSanitizer on the Mac.
Padding `.data` (below) made it go away. That fix also shifts the globals by
the size of the gap, so this alone doesn't rule out another cause, but the
corrupted variable was exactly in the bytes the OS doesn't reserve.

## Control

The same program built with `link_map_fixed.ld`, which only pads `.data` to
16 bytes (`. = ALIGN(16)` before `__data_end__`), has no gap in either
variant:

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

- have pdc compute the memory size from the end of `.bss` (its address +
  size) rather than file size + `.bss` size, or
- pad `.data` in `link_map.ld` to the largest `.bss` alignment, as
  `link_map_fixed.ld` does.

## Workaround for games

Use a copy of the linker script with `.data` padded to 16, set after the
include in the game's Makefile:

```make
include $(SDK)/C_API/buildsupport/common.mk
LDSCRIPT := $(CURDIR)/link_map_fixed.ld
```

## Options

- `UDEFS=-DUSE_DOUBLE`: 8-byte-aligned `double` globals instead of the
  16-byte-aligned object.
- `UDEFS=-DDATA_WORDS=n`: number of initialized words in `.data` (default 1),
  to move where `.data` ends.
