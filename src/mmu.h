#ifndef MMU_H
#define MMU_H

#include <stdint.h>

/* Interrupt bits, shared vocabulary between all modules. These are the bit
 * positions in both IF (0xFF0F, "requested") and IE (0xFFFF, "enabled").
 * The order also fixes each interrupt's priority and its jump vector:
 * bit N vectors to 0x40 + N*8. */
#define INT_VBLANK 0x01 /* vector 0x40 */
#define INT_STAT   0x02 /* vector 0x48 */
#define INT_TIMER  0x04 /* vector 0x50 */
#define INT_SERIAL 0x08 /* vector 0x58 */
#define INT_JOYPAD 0x10 /* vector 0x60 */

void mmu_init(void);

/* Every CPU memory access goes through these two functions. They implement
 * the DMG memory map by dispatching each address range to the module that
 * owns it (cartridge, PPU, timer, or the MMU's own RAM). */
uint8_t mmu_read(uint16_t addr);
void    mmu_write(uint16_t addr, uint8_t value);

/* Called by the PPU, timer, and joypad to set a bit in IF and thereby
 * request an interrupt. The CPU decides later whether to actually take it
 * (that depends on IME and IE). */
void mmu_request_interrupt(uint8_t bit);

/* Called by main.c whenever SDL key state changes. Bits are ACTIVE-HIGH here
 * (1 = pressed); the inversion to the Game Boy's active-low convention
 * happens when the game reads 0xFF00. dpad: bit0=Right,1=Left,2=Up,3=Down.
 * buttons: bit0=A,1=B,2=Select,3=Start. */
void mmu_set_input(uint8_t dpad, uint8_t buttons);

#endif
