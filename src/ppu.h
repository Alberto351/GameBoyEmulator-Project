#ifndef PPU_H
#define PPU_H

#include <stdbool.h>
#include <stdint.h>

#define LCD_WIDTH  160
#define LCD_HEIGHT 144

void ppu_init(void);

/* Advance the PPU by the T-cycles the last instruction took. The PPU runs
 * off the same 4 MHz clock as the CPU (1 dot = 1 T-cycle), which is why a
 * plain "advance by N" keeps them in sync. */
void ppu_step(int cycles);

/* MMU dispatch for VRAM (0x8000-0x9FFF), OAM (0xFE00-0xFE9F), and the LCD
 * registers (0xFF40-0xFF4B). */
uint8_t ppu_read(uint16_t addr);
void    ppu_write(uint16_t addr, uint8_t value);

/* One ARGB pixel per LCD dot, rewritten scanline by scanline. main.c copies
 * it to the SDL texture whenever ppu_frame_ready() reports a completed
 * frame (the flag clears on read). */
extern uint32_t ppu_framebuffer[LCD_WIDTH * LCD_HEIGHT];
bool ppu_frame_ready(void);

#endif
