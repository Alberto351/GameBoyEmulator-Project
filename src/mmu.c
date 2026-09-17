/* mmu.c — the DMG memory map.
 *
 * The Game Boy has a single flat 16-bit address space, but almost none of it
 * is "just RAM". Each range is a different physical chip or hardware block:
 *
 *   0x0000-0x3FFF  cartridge ROM, bank 0 (fixed)
 *   0x4000-0x7FFF  cartridge ROM, switchable bank (MBC-controlled)
 *   0x8000-0x9FFF  VRAM (inside the PPU)
 *   0xA000-0xBFFF  external RAM (on the cartridge, battery-backed saves)
 *   0xC000-0xDFFF  WRAM (8KB work RAM)
 *   0xE000-0xFDFF  "echo RAM" — see below
 *   0xFE00-0xFE9F  OAM (sprite attribute table, inside the PPU)
 *   0xFEA0-0xFEFF  unusable — see below
 *   0xFF00-0xFF7F  I/O registers (joypad, serial, timer, audio, PPU)
 *   0xFF80-0xFFFE  HRAM (127 bytes of fast RAM inside the CPU chip)
 *   0xFFFF         IE, the interrupt-enable register
 *
 * This file owns WRAM, HRAM, IF/IE, the joypad register, and serial stubs,
 * and dispatches everything else to the owning module. Keeping dispatch in
 * one switch-like function is the whole point: when a game misbehaves, you
 * can put one breakpoint here and see every bus access it makes.
 */
#include "mmu.h"

#include <stdio.h>
#include <string.h>

#include "cartridge.h"
#include "ppu.h"
#include "timer.h"

static uint8_t wram[0x2000];
static uint8_t hram[0x7F];

static uint8_t if_reg; /* 0xFF0F: interrupts *requested* by hardware   */
static uint8_t ie_reg; /* 0xFFFF: interrupts the game has *enabled*    */

/* Joypad state. The hardware is a 2x4 key matrix: the game writes to bits
 * 4-5 of 0xFF00 to select a row (d-pad or buttons), then reads the four
 * column bits. We store the pressed keys active-high and invert on read. */
static uint8_t joyp_select;   /* last value written to bits 4-5 of 0xFF00 */
static uint8_t input_dpad;    /* active-high: Right/Left/Up/Down          */
static uint8_t input_buttons; /* active-high: A/B/Select/Start            */

static uint8_t serial_data;   /* 0xFF01 SB — stored but never transferred */

void mmu_init(void)
{
    memset(wram, 0, sizeof wram);
    memset(hram, 0, sizeof hram);
    /* Post-boot value of IF. The boot ROM leaves VBlank requested (bit 0)
     * plus the always-set unused upper bits. */
    if_reg = 0xE1;
    ie_reg = 0x00;
    joyp_select = 0x30; /* neither matrix row selected */
}

void mmu_request_interrupt(uint8_t bit)
{
    if_reg |= bit;
}

void mmu_set_input(uint8_t dpad, uint8_t buttons)
{
    /* A joypad interrupt fires on a high-to-low edge of any column line,
     * i.e. when a key goes from released to pressed. Few games use it
     * (most poll), but Tetris's demo timeout does. */
    if ((dpad & ~input_dpad) || (buttons & ~input_buttons))
        mmu_request_interrupt(INT_JOYPAD);
    input_dpad = dpad;
    input_buttons = buttons;
}

/* Build the value the game sees at 0xFF00. THE BITS ARE INVERTED: on this
 * hardware 0 means "selected" / "pressed", because the matrix lines are
 * pulled high by resistors and pressing a key shorts them to ground. This
 * trips up everyone: to check if A is pressed a game ANDs with 0x01 and
 * branches if the bit is CLEAR. */
static uint8_t joyp_read(void)
{
    uint8_t result = 0xC0 | joyp_select | 0x0F; /* unused bits read as 1 */
    if (!(joyp_select & 0x10))          /* bit 4 low selects the d-pad row */
        result &= ~(input_dpad & 0x0F);
    if (!(joyp_select & 0x20))          /* bit 5 low selects the button row */
        result &= ~(input_buttons & 0x0F);
    return result;
}

/* OAM DMA (write to 0xFF46): the game writes a page number XX and hardware
 * copies 0xXX00-0xXX9F into OAM. On real hardware this takes 160 µs during
 * which the CPU can only execute from HRAM — that is why every game copies a
 * tiny "wait loop" routine into HRAM and calls it there. We copy instantly;
 * games that follow the rules never observe the difference. */
static void oam_dma(uint8_t page)
{
    uint16_t src = (uint16_t)page << 8;
    for (int i = 0; i < 0xA0; i++)
        ppu_write(0xFE00 + i, mmu_read(src + i));
}

uint8_t mmu_read(uint16_t addr)
{
    if (addr < 0x8000)  return cart_read_rom(addr);
    if (addr < 0xA000)  return ppu_read(addr);          /* VRAM */
    if (addr < 0xC000)  return cart_read_ram(addr);
    if (addr < 0xE000)  return wram[addr - 0xC000];
    if (addr < 0xFE00)
        /* Echo RAM: 0xE000-0xFDFF mirrors WRAM because address bit 13 isn't
         * fully decoded — the WRAM chip can't tell the two ranges apart.
         * Nintendo said "don't use it", some games use it anyway, so we
         * implement the mirror instead of ignoring it. */
        return wram[addr - 0xE000];
    if (addr < 0xFEA0)  return ppu_read(addr);          /* OAM */
    if (addr < 0xFF00)
        /* The unusable region. No chip is mapped here; reads mostly return
         * 0x00 on DMG (with obscure OAM-related corruption we skip). Return
         * a constant rather than crashing so buggy games keep running. */
        return 0x00;
    if (addr < 0xFF80) {
        /* I/O registers */
        switch (addr) {
        case 0xFF00: return joyp_read();
        case 0xFF01: return serial_data;
        case 0xFF02: return 0x7E; /* serial control: no transfer in progress */
        case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
            return timer_read(addr);
        case 0xFF0F: return if_reg | 0xE0; /* upper 3 bits unwired, read as 1 */
        default:
            if (addr >= 0xFF40 && addr <= 0xFF4B)
                return ppu_read(addr);
            /* Audio (0xFF10-0xFF3F) is not implemented, and the rest of the
             * I/O space is unmapped: reads see the open bus as 0xFF. */
            return 0xFF;
        }
    }
    if (addr < 0xFFFF)  return hram[addr - 0xFF80];
    return ie_reg;
}

void mmu_write(uint16_t addr, uint8_t value)
{
    if (addr < 0x8000)  { cart_write_rom(addr, value); return; }
    if (addr < 0xA000)  { ppu_write(addr, value); return; }
    if (addr < 0xC000)  { cart_write_ram(addr, value); return; }
    if (addr < 0xE000)  { wram[addr - 0xC000] = value; return; }
    if (addr < 0xFE00)  { wram[addr - 0xE000] = value; return; } /* echo */
    if (addr < 0xFEA0)  { ppu_write(addr, value); return; }      /* OAM */
    if (addr < 0xFF00)  return; /* unusable region: writes go nowhere */
    if (addr < 0xFF80) {
        switch (addr) {
        case 0xFF00: joyp_select = value & 0x30; return; /* only the row-
                        select bits are writable; column bits are inputs */
        case 0xFF01: serial_data = value; return;
        case 0xFF02:
            /* No link cable is emulated, but writing 0x81 here means "send
             * SB now" — and Blargg's test ROMs report their results this
             * way. Echoing the byte to stderr turns the whole test suite
             * into a headless pass/fail without any debugger. */
            if (value == 0x81)
                fputc(serial_data, stderr);
            return;
        case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
            timer_write(addr, value); return;
        case 0xFF0F: if_reg = value & 0x1F; return;
        case 0xFF46: oam_dma(value); return;
        default:
            if (addr >= 0xFF40 && addr <= 0xFF4B)
                ppu_write(addr, value);
            /* audio + unmapped: dropped */
            return;
        }
    }
    if (addr < 0xFFFF)  { hram[addr - 0xFF80] = value; return; }
    ie_reg = value;
}
