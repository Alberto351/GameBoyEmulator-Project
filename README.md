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

### Linux (primary target)

Requires SDL2 and a C99 compiler:

```
sudo apt install libsdl2-dev   # Debian/Ubuntu
make
```

The default build is a debug build (`-g -fsanitize=address`). Use
`make BUILD=release` for an optimized binary.

### Windows (MSYS2)

Install SDL2 into the MSYS2 toolchain, then build directly with its gcc.
(Don't use `make` here if Git for Windows is installed: Git ships its own
broken `sdl2-config` that shadows the real one on PATH.)

```
pacman -S mingw-w64-x86_64-SDL2
/c/msys64/mingw64/bin/gcc -std=c99 -Wall -Wextra -O2 src/*.c -o gbemu.exe $(/c/msys64/mingw64/bin/sdl2-config --cflags --libs)
```

## Day-to-day workflow (copy-paste)

From a terminal in the repo root (VS Code terminal works fine):

**Run a game:**

```
./gbemu.exe roms/adjustris.gb
```

**Rebuild after changing the source, then run** (Windows/MSYS2 — this is
the same command the VS Code build task uses):

```
C:/msys64/mingw64/bin/gcc.exe -std=c99 -Wall -Wextra -O2 src/*.c -o gbemu.exe -IC:/msys64/mingw64/include -LC:/msys64/mingw64/lib -lmingw32 -lSDL2main -lSDL2 -Dmain=SDL_main && ./gbemu.exe roms/adjustris.gb
```

**Rerun the CPU test suite after a CPU change** (results print to the
terminal via the emulated serial port — wait ~40 seconds for
"Passed all tests"):

```
./gbemu.exe roms/cpu_instrs.gb
```

On Linux the equivalents are just `make` and `./gbemu roms/<game>.gb`.

In VS Code, `.vscode/tasks.json` wires this up: **Ctrl+Shift+B** rebuilds,
and *Terminal → Run Task → run adjustris* rebuilds and launches the game.

## Running

```
./gbemu path/to/game.gb        # ./gbemu.exe on Windows
```

Put ROMs in `roms/` — the folder is gitignored so they never get committed.
Good first things to run, all freely distributed:

- [Blargg's `cpu_instrs.gb`](https://github.com/retrio/gb-test-roms) —
  CPU test suite; this emulator passes all 11, and the results also print
  to stderr through the emulated serial port
- [`dmg-acid2.gb`](https://github.com/mattcurrie/dmg-acid2) — PPU rendering
  test; compare against its reference image
- [Adjustris](https://github.com/tbsp/Adjustris) — open-source homebrew
  falling-block game (MBC3)

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
