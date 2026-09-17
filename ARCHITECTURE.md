# Architecture

How the pieces of this emulator fit together, and why they're shaped the way
they are. Read this before the source; each section points at the file that
implements it.

## The big picture

```
main.c                    the only loop in the program
  └─ cpu_step()           execute ONE instruction, return its T-cycle count
       └─ mmu_read/write  every bus access dispatches through here
            ├─ cartridge.c   ROM banks, external RAM (MBC1)
            ├─ ppu.c         VRAM, OAM, LCD registers
            ├─ timer.c       DIV/TIMA registers
            └─ mmu.c itself  WRAM, HRAM, IF/IE, joypad, OAM DMA
  └─ ppu_step(cycles)     advance the video state machine by that many dots
  └─ timer_step(cycles)   advance the timer counter by that many ticks
```

Single-threaded, no callbacks, no event queue. Each module is a `.c` file
with static state and a tiny header — the "objects" of this design are
translation units.

## The fetch-decode-execute loop

`cpu_step()` in [cpu.c](src/cpu.c) does one instruction per call:

1. **Interrupts first.** Before fetching, check whether an enabled interrupt
   is pending; if so, dispatch it *instead of* executing an instruction
   (see below). This ordering matters: hardware samples the interrupt lines
   between instructions, never in the middle of one.
2. **Fetch** the opcode byte at PC and increment PC.
3. **Decode** with a `switch`. Half the opcode space (LD r,r′ / ALU A,r and
   the whole CB page) is perfectly regular, so those are decoded
   arithmetically from the opcode's bit fields; the irregular quarters get
   explicit cases.
4. **Execute**, reading and writing memory through the MMU only.
5. **Return the T-cycle count.** This number is the whole basis of timing —
   see the next section.

There is no instruction table-of-function-pointers. A switch keeps every
opcode's behavior and cycle count visible in one place and lets the compiler
check exhaustiveness; function-pointer tables are an optimization this
project doesn't want.

## How the CPU and PPU stay in sync

They share one clock in hardware: the 4194304 Hz crystal. One PPU "dot" is
one CPU T-cycle. So synchronization is nothing more than:

```c
int cycles = cpu_step();
ppu_step(cycles);
timer_step(cycles);
```

The CPU is the *only* thing that runs; the PPU and timer are passive state
machines that get dragged forward by however long the last instruction took.
This is called "catch-up scheduling" at instruction granularity, and its
accuracy limit is exactly the length of one instruction (4–24 cycles): if a
game reads a PPU register, it sees state that may be up to 24 dots stale
relative to a real DMG mid-instruction. No commercial DMG game can tell.

What we are *not* doing: running the CPU for a whole frame and then
rendering ("frame at a time" — breaks any game that changes video registers
mid-frame, which is most of them), or interleaving at per-cycle granularity
(needed only for exotic raster effects, and triples the complexity of every
memory access).

The frame loop in [main.c](src/main.c) runs this triad until the PPU
reports a finished frame (70224 cycles later), blits, then sleeps to hold
59.73 fps. Simulated time and wall-clock time only meet at that one point.

## The memory map, and why dispatch lives in one function

The DMG's 16-bit address space is a patchwork of physical chips: cartridge
ROM, cartridge RAM, VRAM and OAM inside the PPU, WRAM, HRAM inside the CPU,
and memory-mapped I/O registers. Two facts shape the design:

- **What an address means can change at runtime.** 0x4000-0x7FFF is
  whichever ROM bank the MBC currently selects; 0xFF00 reads differently
  depending on which joypad row was selected. So memory cannot be one big
  array — reads and writes must be *dispatched*.
- **Ownership follows the hardware.** VRAM lives in [ppu.c](src/ppu.c), not
  in the MMU, because on real hardware the PPU sits between the CPU and
  that chip. Same for ROM and the cartridge. The MMU
  ([mmu.c](src/mmu.c)) is only the address decoder — the chip-select logic
  on the board — plus the few things that really do belong to it (WRAM,
  HRAM, IF/IE, joypad, OAM DMA).

The payoff of a single `mmu_read`/`mmu_write` pair: one breakpoint shows
every bus access a misbehaving game makes, and quirks like echo RAM
(0xE000 mirrors WRAM because address decoding is incomplete) are one
commented line instead of scattered special cases.

## Interrupt dispatch, step by step

Five sources, in priority order: VBlank, LCD STAT, Timer, Serial, Joypad.
Three registers interact:

- **IF** (0xFF0F): which interrupts hardware has *requested*. Peripherals
  set bits here via `mmu_request_interrupt()`.
- **IE** (0xFFFF): which interrupts the game has *enabled*.
- **IME**: the master enable — not memory-mapped, only DI/EI/RETI and the
  dispatch itself touch it.

At the top of every `cpu_step()`:

1. `pending = IF & IE & 0x1F`. If nonzero, the CPU leaves HALT — **even if
   IME is off**. (That's deliberate: `DI ; HALT` is a common "wait for
   VBlank without taking the handler" idiom.)
2. If IME is off or nothing is pending, execute normally.
3. Otherwise take the lowest set bit and dispatch, exactly as hardware
   does: clear IME (no nesting unless the handler re-enables), clear that
   one IF bit (acknowledge), push PC, jump to the fixed vector
   `0x40 + 8*bit`. Costs 20 T-cycles.

Two timing subtleties live in [cpu.c](src/cpu.c): EI takes effect one
instruction *late* (so `EI ; RET` can't be interrupted between the two),
and the HALT bug (HALT with IME off and an interrupt already pending makes
the next byte execute twice).

## The three bugs you will write first

Every first implementation with this design hits some subset of these.
Check them before anything else when the screen is garbage:

1. **HALF_CARRY computed wrong.** H is carry out of *bit 3* for 8-bit ops,
   out of *bit 11* for ADD HL,rr — and from *unsigned low-byte* arithmetic
   for ADD SP,e8. Get any variant wrong and everything looks fine until a
   game runs DAA on its score counter and the digits turn to letters:
   Tetris scores are the classic symptom. Related traps: AND and BIT force
   H=1, INC/DEC preserve C, POP AF must mask F's low nibble.

2. **Signed tile addressing forgotten.** When LCDC bit 4 is 0, BG tile
   numbers are *signed* offsets from 0x9000. Miss the `(int8_t)` cast and
   exactly half the tiles render as garbage while the other half look
   perfect — which sends you hunting through your CPU for a bug that isn't
   there. Menus often use one addressing mode and gameplay the other, so
   "title screen fine, game broken" is this bug's signature.

3. **Interrupt bookkeeping half right.** The frequent variants: forgetting
   to clear the IF bit on dispatch (the handler runs forever, the game
   appears to hang); dispatching even when IME is off (games break
   instantly); never waking from HALT when IME is off but an interrupt is
   pending (the game freezes at the first "wait for VBlank" — usually
   before showing a single frame). If your emulator boots to a white
   screen forever, start here, not in the PPU.

## What was consciously left out

Audio, serial link, sub-instruction memory timing, VRAM/OAM access blocking
during PPU modes, the STAT-blocking IRQ edge, the 4-cycle TIMA reload
window, and mode-1 large-ROM MBC1 remapping. Each omission is commented at
the spot where the full behavior would go, with a note on what depends on
it — so upgrading any of them later is a local change, not a redesign.
