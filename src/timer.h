#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(void);

/* Advance the timer by the T-cycles the last instruction took. */
void timer_step(int cycles);

/* MMU dispatch for 0xFF04 (DIV), 0xFF05 (TIMA), 0xFF06 (TMA), 0xFF07 (TAC). */
uint8_t timer_read(uint16_t addr);
void    timer_write(uint16_t addr, uint8_t value);

#endif
