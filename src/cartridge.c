/* cartridge.c — ROM loading and MBC1 banking.
 *
 * A Game Boy cartridge is not just a ROM chip. The address bus only exposes
 * 32KB of ROM space (0x0000-0x7FFF), so larger games ship a Memory Bank
 * Controller (MBC) chip that swaps which 16KB "bank" of the ROM is visible in
 * the upper half of that window. The CPU controls the MBC by *writing to ROM
 * addresses* — writes to a read-only chip would otherwise be meaningless, so
 * the MBC snoops them off the bus and treats them as register writes. That is
 * why cart_write_rom() exists at all.
 *
 * Tetris is "ROM ONLY" (header type 0x00): 32KB, no MBC, no RAM. We also
 * implement MBC1 (types 0x01-0x03) so real banked games work later.
 */
#include "cartridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *rom;          /* whole ROM file */
static size_t   rom_size;
static uint8_t *ext_ram;      /* external (cartridge) RAM, if any */
static size_t   ext_ram_size;

static enum { MBC_NONE, MBC1, MBC3 } mbc;

/* MBC1 register state. These live in the mapper chip, not in any RAM. */
static uint8_t  ram_enabled;   /* 0x0A written to 0x0000-0x1FFF enables RAM */
static uint8_t  rom_bank_lo;   /* 5-bit ROM bank number (writes to 0x2000)  */
static uint8_t  bank_hi;       /* 2-bit: upper ROM bits OR RAM bank number  */
static uint8_t  banking_mode;  /* 0 = "ROM" mode, 1 = "RAM" mode            */

bool cart_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open ROM: %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0x8000) { /* smaller than the fixed 32KB window = not a valid GB ROM */
        fprintf(stderr, "ROM too small (%ld bytes)\n", len);
        fclose(f);
        return false;
    }
    rom_size = (size_t)len;
    rom = malloc(rom_size);
    if (!rom || fread(rom, 1, rom_size, f) != rom_size) {
        fprintf(stderr, "failed to read ROM\n");
        fclose(f);
        return false;
    }
    fclose(f);

    /* Header byte 0x0147 identifies the mapper chip. */
    uint8_t type = rom[0x0147];
    switch (type) {
    case 0x00:                       mbc = MBC_NONE; break; /* ROM only (Tetris) */
    case 0x01: case 0x02: case 0x03: mbc = MBC1;     break; /* MBC1 variants */
    /* MBC3 without the real-time clock is a SIMPLER chip than MBC1: one
     * 7-bit ROM bank register (so no bank_hi splitting) and a plain RAM
     * bank register. RTC registers (types 0x0F/0x10) are not implemented;
     * plain MBC3 carts (0x11-0x13) never touch them. */
    case 0x11: case 0x12: case 0x13: mbc = MBC3;     break;
    default:
        fprintf(stderr, "unsupported cartridge type 0x%02X\n", type);
        return false;
    }

    /* Header byte 0x0149 encodes external RAM size. 0x02 = one 8KB bank,
     * 0x03 = four 8KB banks (the most MBC1 supports). */
    switch (rom[0x0149]) {
    case 0x02: ext_ram_size = 0x2000;  break;
    case 0x03: ext_ram_size = 0x8000;  break;
    default:   ext_ram_size = 0;       break;
    }
    if (ext_ram_size) {
        ext_ram = calloc(1, ext_ram_size);
        if (!ext_ram) return false;
    }

    /* The title lives at 0x0134 in the header; print it as a sanity check. */
    char title[17] = {0};
    memcpy(title, &rom[0x0134], 16);
    printf("loaded \"%s\" (%zuKB, type 0x%02X)\n", title, rom_size / 1024, type);

    rom_bank_lo = 1; /* the MBC1 powers up with bank 1 selected */
    return true;
}

void cart_free(void)
{
    free(rom);
    free(ext_ram);
}

/* Which 16KB ROM bank is currently mapped at 0x4000-0x7FFF? */
static uint32_t switchable_bank(void)
{
    /* Neither MBC's bank register can select 0 (the fixed bank): writing 0
     * selects bank 1 — the hardware ORs in a 1 when the register is zero.
     * On MBC1 the check covers only the low 5 bits, which is why banks
     * 0x20/0x40/0x60 are unreachable on real MBC1 carts. */
    uint32_t bank = rom_bank_lo ? rom_bank_lo : 1;
    /* MBC1 mode 0: the 2-bit register supplies ROM bank bits 5-6, letting
     * MBC1 address up to 2MB. In mode 1 those bits go to RAM banking
     * instead. (MBC3 has a full 7-bit register; nothing to splice.) */
    if (mbc == MBC1 && banking_mode == 0)
        bank |= (uint32_t)bank_hi << 5;
    return bank;
}

uint8_t cart_read_rom(uint16_t addr)
{
    uint32_t offset;
    if (addr < 0x4000) {
        /* Fixed bank 0 region. (True MBC1 mode-1 large-cart remapping of this
         * region is omitted: it only matters for >512KB carts in mode 1.) */
        offset = addr;
    } else {
        offset = switchable_bank() * 0x4000u + (addr - 0x4000);
    }
    /* Mask to ROM size: real carts mirror because unused high address lines
     * simply aren't connected to the ROM chip. */
    return rom[offset % rom_size];
}

void cart_write_rom(uint16_t addr, uint8_t value)
{
    if (mbc == MBC_NONE)
        return; /* no mapper chip on the bus; the write hits nothing */

    switch (addr & 0x6000) {
    case 0x0000:
        /* RAM enable. The magic value is "low nibble == 0xA"; anything else
         * disables. Games disable RAM when done writing saves to protect
         * them from corruption at power-off. */
        ram_enabled = (value & 0x0F) == 0x0A;
        break;
    case 0x2000:
        /* ROM bank number: MBC1 wires up 5 bits, MBC3 wires 7. */
        rom_bank_lo = value & (mbc == MBC3 ? 0x7F : 0x1F);
        break;
    case 0x4000:
        /* MBC1: 2-bit dual-purpose register. MBC3: RAM bank number (values
         * 0x08-0x0C would select RTC registers, which we don't have). */
        bank_hi = value & 0x03;
        break;
    case 0x6000:
        /* MBC1: banking mode. MBC3: RTC latch (unimplemented). */
        banking_mode = value & 0x01;
        break;
    }
}

uint8_t cart_read_ram(uint16_t addr)
{
    /* Reads from disabled or absent RAM float the bus; 0xFF is what real
     * hardware effectively returns. */
    if (!ext_ram || !ram_enabled)
        return 0xFF;
    /* MBC3's RAM bank register always applies; MBC1's only in mode 1. */
    uint32_t bank = (mbc == MBC3 || banking_mode == 1) ? bank_hi : 0;
    return ext_ram[(bank * 0x2000u + (addr - 0xA000)) % ext_ram_size];
}

void cart_write_ram(uint16_t addr, uint8_t value)
{
    if (!ext_ram || !ram_enabled)
        return;
    uint32_t bank = (mbc == MBC3 || banking_mode == 1) ? bank_hi : 0;
    ext_ram[(bank * 0x2000u + (addr - 0xA000)) % ext_ram_size] = value;
}
