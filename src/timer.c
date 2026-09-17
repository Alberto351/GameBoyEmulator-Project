/* timer.c — DIV and TIMA.
 *
 * The key insight that makes all the timer quirks fall out naturally: there
 * is only ONE counter in the hardware, a free-running 16-bit counter that
 * increments every T-cycle. Everything else is derived from it:
 *
 *   - DIV (0xFF04) is literally the UPPER 8 BITS of that counter. That's
 *     why you can't write a value to DIV: any write just resets the whole
 *     counter to 0.
 *
 *   - TIMA (0xFF05) increments on the FALLING EDGE of one selected bit of
 *     the counter. TAC's low 2 bits pick which bit, giving the four rates:
 *       TAC=00 -> bit 9  -> 4096 Hz
 *       TAC=01 -> bit 3  -> 262144 Hz
 *       TAC=10 -> bit 5  -> 65536 Hz
 *       TAC=11 -> bit 7  -> 16384 Hz
 *     TAC bit 2 gates the edge detector (timer enable).
 *
 * Modeling the falling edge (instead of just "add cycles/N") buys us the
 * famous quirk for free: writing to DIV zeroes the counter, and if the
 * selected bit happened to be 1 at that moment, that zeroing IS a falling
 * edge — so resetting DIV can tick TIMA. Games (and test ROMs) rely on it.
 */
#include "timer.h"

#include <stdbool.h>

#include "mmu.h"

static uint16_t counter; /* the one true counter; DIV = counter >> 8 */
static uint8_t  tima;    /* 0xFF05: the game-visible timer            */
static uint8_t  tma;     /* 0xFF06: value reloaded into TIMA on overflow */
static uint8_t  tac;     /* 0xFF07: enable + rate select              */

void timer_init(void)
{
    /* DIV has been running since power-on; by the time the boot ROM hands
     * off it reads 0xAB. Games shouldn't depend on it, but matching real
     * hardware costs nothing. */
    counter = 0xAB00;
    tima = 0;
    tma = 0;
    tac = 0xF8;
}

/* Which bit of the counter does the current TAC rate watch? */
static uint16_t selected_bit(void)
{
    switch (tac & 0x03) {
    case 0:  return 1 << 9;
    case 1:  return 1 << 3;
    case 2:  return 1 << 5;
    default: return 1 << 7;
    }
}

/* The edge detector: called with the counter value before and after any
 * change (normal ticking OR a DIV-write reset — both go through here, which
 * is what makes the DIV-write quirk work). */
static void detect_falling_edge(uint16_t before, uint16_t after)
{
    if (!(tac & 0x04))
        return; /* timer disabled: the edge detector input is gated to 0 */
    uint16_t bit = selected_bit();
    if ((before & bit) && !(after & bit)) {
        tima++;
        if (tima == 0) {
            /* Overflow: reload from TMA and request the interrupt. (Real
             * hardware delays the reload by 4 T-cycles, during which TIMA
             * reads 0x00 and a write can cancel the reload — a subtlety no
             * commercial game depends on, so we reload immediately.) */
            tima = tma;
            mmu_request_interrupt(INT_TIMER);
        }
    }
}

void timer_step(int cycles)
{
    /* Tick one T-cycle at a time so no falling edge can be skipped. At
     * most ~24 iterations per instruction; clarity beats speed here. */
    for (int i = 0; i < cycles; i++) {
        uint16_t before = counter;
        counter++;
        detect_falling_edge(before, counter);
    }
}

uint8_t timer_read(uint16_t addr)
{
    switch (addr) {
    case 0xFF04: return (uint8_t)(counter >> 8);
    case 0xFF05: return tima;
    case 0xFF06: return tma;
    default:     return tac | 0xF8; /* upper 5 bits unwired, read as 1 */
    }
}

void timer_write(uint16_t addr, uint8_t value)
{
    switch (addr) {
    case 0xFF04: {
        /* ANY write to DIV resets the whole internal counter — the value
         * written is ignored. Run the reset through the edge detector: if
         * the selected bit was high, this counts as a falling edge and
         * TIMA ticks (see file comment). */
        uint16_t before = counter;
        counter = 0;
        detect_falling_edge(before, 0);
        break;
    }
    case 0xFF05: tima = value; break;
    case 0xFF06: tma = value; break;
    default:     tac = value & 0x07; break;
    }
}
