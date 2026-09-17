# gbemu — a teaching Game Boy (DMG) emulator

A single-threaded Game Boy emulator written in C99, built to be *read*, not to
be fast. Every non-obvious piece of code carries a comment explaining the
hardware behavior it implements. The goal is that someone who knows C but has
never written an emulator can follow the whole thing from `main()` down to a
pixel appearing on screen.

It runs Tetris (and other ROM-only / MBC1 games) correctly.

> **Note:** this is a learning project. A generated reference implementation
> was studied before writing this code.

## Building

Requires SDL2 and a C99 compiler on Linux:

```
sudo apt install libsdl2-dev   # Debian/Ubuntu
make
```

The default build is a debug build (`-g -fsanitize=address`). Use
`make BUILD=release` for an optimized binary.

## Running

```
./gbemu path/to/tetris.gb
```

No ROMs are included — bring your own legally obtained cartridge dump.
No boot ROM is needed; the emulator initializes the CPU and I/O registers to
their known post-boot values and starts execution at `0x0100`, exactly where
the real boot ROM hands off.

## Controls

| Key        | Game Boy button |
|------------|-----------------|
| Arrow keys | D-pad           |
| Z          | A               |
| X          | B               |
| Enter      | Start           |
| Right Shift| Select          |
| Escape     | Quit            |

## What's implemented

- Full SM83 instruction set, including CB-prefixed opcodes, with per-opcode
  T-cycle counts (cycle-accurate at instruction granularity)
- Scanline-based PPU: background, window, sprites with DMG priority rules,
  modes 0–3 with correct timing
- All five interrupts, including the HALT bug
- DIV/TIMA timers with falling-edge increment behavior
- MBC1 banking (Tetris itself is ROM-only, but real games need it)
- Joypad with its inverted-bit convention

## What's deliberately missing

Audio, a debugger, save states, serial link, and sub-instruction cycle
accuracy. See [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit
together and the bugs you're most likely to write if you build one of these
yourself.
