#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include <stdint.h>

/* Sets registers to their documented post-boot-ROM values and PC to 0x0100,
 * so we can run cartridges without a boot ROM image. */
void cpu_init(void);

/* Executes one instruction (or services one interrupt, or idles in HALT) and
 * returns how many T-cycles it took. The caller advances the PPU and timer
 * by the same amount — that return value is the clock that keeps the whole
 * machine in sync. */
int cpu_step(void);

#endif
