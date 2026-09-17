#ifndef CARTRIDGE_H
#define CARTRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* Loads the ROM file, reads the cartridge header, and allocates external RAM
 * if the header asks for it. Returns false on any failure (file missing,
 * unsupported mapper). */
bool cart_load(const char *path);
void cart_free(void);

/* CPU accesses to 0x0000-0x7FFF (ROM) and 0xA000-0xBFFF (external RAM) are
 * routed here by the MMU, because what those addresses mean depends on the
 * mapper chip (MBC) inside the cartridge, not on the Game Boy itself. */
uint8_t cart_read_rom(uint16_t addr);
void    cart_write_rom(uint16_t addr, uint8_t value); /* MBC register writes */
uint8_t cart_read_ram(uint16_t addr);
void    cart_write_ram(uint16_t addr, uint8_t value);

#endif
