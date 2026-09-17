/* cpu.c — the Sharp SM83 core.
 *
 * The Game Boy's CPU is commonly called "a Z80" but it isn't: it's Sharp's
 * SM83, an 8080-family core with the Z80's CB-prefixed bit operations bolted
 * on, no IX/IY, no shadow registers, and a few of its own instructions
 * (LDH, LD (HL+)/(HL-), SWAP, STOP).
 *
 * Everything here revolves around cpu_step(): fetch one opcode at PC, decode
 * it with a big switch, execute it, and return how many T-cycles it took.
 * T-cycles are ticks of the 4194304 Hz master clock; every instruction takes
 * a multiple of 4 of them (one memory access = 4 T-cycles = 1 "M-cycle").
 * The returned count is what drives the PPU and timer, so getting these
 * numbers right IS the emulator's notion of time.
 *
 * FLAGS. F holds four flags in its top nibble (the low nibble always reads
 * 0 — even POP AF can't set those bits, hardware masks them):
 *   Z (0x80) result was zero
 *   N (0x40) last op was a subtraction  — exists only to make DAA work
 *   H (0x20) half-carry: carry out of BIT 3 (or bit 11 for 16-bit adds)
 *   C (0x10) full carry: carry out of bit 7 (or 15)
 * HALF_CARRY is the most commonly botched flag, so each ALU helper below
 * spells out exactly which bits it tests and why.
 */
#include "cpu.h"

#include "mmu.h"

#define FLAG_Z 0x80
#define FLAG_N 0x40
#define FLAG_H 0x20
#define FLAG_C 0x10

static struct {
    uint8_t a, f;
    uint8_t b, c;
    uint8_t d, e;
    uint8_t h, l;
    uint16_t sp, pc;
} R;

/* True while the CPU is stopped by HALT, waiting for an interrupt. */
static bool halted;

/* IME is the master interrupt switch. It is NOT a memory-mapped register —
 * only DI, EI, RETI and the interrupt-dispatch sequence itself can touch it. */
static bool ime;

/* EI doesn't enable interrupts immediately: the effect is delayed until
 * after the *next* instruction. This exists so the idiom "EI ; RET" at the
 * end of an interrupt handler can return before another interrupt fires,
 * preventing unbounded stack growth. We model it with this one-shot flag. */
static bool ime_pending;

/* Set when the HALT bug triggers; makes the next fetch NOT advance PC. */
static bool halt_bug;

void cpu_init(void)
{
    /* Documented DMG register state at the moment the boot ROM jumps to
     * 0x0100. A=0x01 identifies the model (DMG) to the game; F=0xB0 means
     * Z,H,C set (the boot ROM's final checksum compare leaves them so). */
    R.a = 0x01; R.f = 0xB0;
    R.b = 0x00; R.c = 0x13;
    R.d = 0x00; R.e = 0xD8;
    R.h = 0x01; R.l = 0x4D;
    R.sp = 0xFFFE;
    R.pc = 0x0100;
    halted = false;
    ime = false;       /* boot ROM hands off with interrupts disabled */
    ime_pending = false;
    halt_bug = false;
}

/* ---- register-pair helpers -------------------------------------------- */

static uint16_t get_bc(void) { return (uint16_t)(R.b << 8) | R.c; }
static uint16_t get_de(void) { return (uint16_t)(R.d << 8) | R.e; }
static uint16_t get_hl(void) { return (uint16_t)(R.h << 8) | R.l; }
static void set_bc(uint16_t v) { R.b = v >> 8; R.c = v & 0xFF; }
static void set_de(uint16_t v) { R.d = v >> 8; R.e = v & 0xFF; }
static void set_hl(uint16_t v) { R.h = v >> 8; R.l = v & 0xFF; }

/* ---- fetch ------------------------------------------------------------ */

static uint8_t fetch8(void)
{
    uint8_t v = mmu_read(R.pc);
    /* The HALT bug: when HALT executes with IME=0 while an interrupt is
     * already pending, the CPU fails to increment PC on the next fetch, so
     * the byte after HALT executes twice. Real hardware, real games depend
     * on emulating it (or at least not crashing on it). */
    if (halt_bug)
        halt_bug = false;
    else
        R.pc++;
    return v;
}

static uint16_t fetch16(void)
{
    uint8_t lo = fetch8(); /* the SM83 is little-endian: low byte first */
    uint8_t hi = fetch8();
    return (uint16_t)(hi << 8) | lo;
}

/* ---- stack ------------------------------------------------------------ */

static void push16(uint16_t v)
{
    /* Stack grows downward; high byte is pushed first so the value sits in
     * memory little-endian like everything else. */
    mmu_write(--R.sp, v >> 8);
    mmu_write(--R.sp, v & 0xFF);
}

static uint16_t pop16(void)
{
    uint8_t lo = mmu_read(R.sp++);
    uint8_t hi = mmu_read(R.sp++);
    return (uint16_t)(hi << 8) | lo;
}

/* ---- 8-bit ALU --------------------------------------------------------
 * Flag behavior per group is commented on each helper. The recurring
 * pattern for HALF_CARRY: mask both operands to their low nibble, do the
 * operation on just those 4 bits, and see if it overflowed past 0x0F —
 * that's literally what the carry chain out of bit 3 does in silicon. */

static void alu_add(uint8_t v, int carry_in)
{
    /* ADD/ADC: Z from result, N=0, H from bit 3, C from bit 7. */
    int c = carry_in ? 1 : 0;
    int result = R.a + v + c;
    int half = (R.a & 0x0F) + (v & 0x0F) + c; /* carry INTO bit 4? */
    R.f = 0;
    if ((result & 0xFF) == 0) R.f |= FLAG_Z;
    if (half > 0x0F)          R.f |= FLAG_H;
    if (result > 0xFF)        R.f |= FLAG_C;
    R.a = (uint8_t)result;
}

static void alu_sub(uint8_t v, int carry_in, bool store)
{
    /* SUB/SBC/CP: Z from result, N=1 (it's a subtraction — DAA needs to
     * know), H = borrow FROM bit 4, C = borrow from bit 8. CP is exactly
     * SUB with the result thrown away, hence the 'store' parameter. */
    int c = carry_in ? 1 : 0;
    int result = R.a - v - c;
    int half = (R.a & 0x0F) - (v & 0x0F) - c;
    R.f = FLAG_N;
    if ((result & 0xFF) == 0) R.f |= FLAG_Z;
    if (half < 0)             R.f |= FLAG_H;
    if (result < 0)           R.f |= FLAG_C;
    if (store)
        R.a = (uint8_t)result;
}

static void alu_and(uint8_t v)
{
    /* AND: Z from result, N=0, C=0, and H=1 ALWAYS — not because any carry
     * happens, but because the 8080/SM83 ALU physically routes AND through
     * circuitry that asserts the half-carry line. Easy to forget; some
     * games' DAA-adjacent code notices if you do. */
    R.a &= v;
    R.f = (R.a == 0 ? FLAG_Z : 0) | FLAG_H;
}

static void alu_xor(uint8_t v)
{
    /* XOR/OR: only Z can be set; N,H,C all cleared. */
    R.a ^= v;
    R.f = (R.a == 0) ? FLAG_Z : 0;
}

static void alu_or(uint8_t v)
{
    R.a |= v;
    R.f = (R.a == 0) ? FLAG_Z : 0;
}

static uint8_t alu_inc8(uint8_t v)
{
    /* INC r: Z, N=0, H from bit 3 — but C is UNTOUCHED. The classic use is
     * loop counters inside multi-byte arithmetic, where clobbering carry
     * would destroy the value being carried between bytes. */
    uint8_t result = v + 1;
    R.f &= FLAG_C; /* preserve C, clear Z/N/H */
    if (result == 0)          R.f |= FLAG_Z;
    if ((v & 0x0F) == 0x0F)   R.f |= FLAG_H; /* 0x?F + 1 carries out of bit 3 */
    return result;
}

static uint8_t alu_dec8(uint8_t v)
{
    /* DEC r: Z, N=1, H = borrow from bit 4; C preserved, same reason. */
    uint8_t result = v - 1;
    R.f = (R.f & FLAG_C) | FLAG_N;
    if (result == 0)        R.f |= FLAG_Z;
    if ((v & 0x0F) == 0x00) R.f |= FLAG_H; /* 0x?0 - 1 borrows from bit 4 */
    return result;
}

static void add_hl(uint16_t v)
{
    /* ADD HL,rr: N=0, H = carry out of bit 11, C = carry out of bit 15,
     * and Z is NOT affected (unlike the 8-bit adds). Bit 11 because the
     * 16-bit add runs through the same 4-bit ALU twice: H reflects the
     * half-carry of the HIGH byte's addition. */
    uint16_t hl = get_hl();
    uint32_t result = (uint32_t)hl + v;
    R.f &= FLAG_Z;
    if (((hl & 0x0FFF) + (v & 0x0FFF)) > 0x0FFF) R.f |= FLAG_H;
    if (result > 0xFFFF)                          R.f |= FLAG_C;
    set_hl((uint16_t)result);
}

static uint16_t sp_plus_offset(void)
{
    /* Shared by ADD SP,e8 and LD HL,SP+e8. The offset is SIGNED, but H and
     * C come from UNSIGNED byte arithmetic on the low byte (bits 3 and 7,
     * not 11 and 15!) because the hardware just adds the raw byte to the
     * low half of SP and fixes the high half up afterward. Z and N are
     * always 0. This is the single weirdest flag case in the CPU. */
    int8_t off = (int8_t)fetch8();
    uint16_t sp = R.sp;
    R.f = 0;
    if (((sp & 0x0F) + (off & 0x0F)) > 0x0F)   R.f |= FLAG_H;
    if (((sp & 0xFF) + (off & 0xFF)) > 0xFF)   R.f |= FLAG_C;
    return (uint16_t)(sp + off);
}

/* ---- rotates and shifts ------------------------------------------------
 * All of these set Z from the result, N=0, H=0, C = the bit rotated/shifted
 * out. Exception: the four A-only forms (RLCA/RRCA/RLA/RRA) force Z=0 even
 * when A becomes zero — a genuine hardware quirk that Blargg's tests (and
 * some games) catch. The CB-prefixed forms of the same operations DO set Z
 * normally. The 'set_z' parameter carries that distinction. */

static uint8_t rot_rlc(uint8_t v, bool set_z)
{
    uint8_t carry = v >> 7;
    v = (uint8_t)((v << 1) | carry);
    R.f = (carry ? FLAG_C : 0) | ((set_z && v == 0) ? FLAG_Z : 0);
    return v;
}

static uint8_t rot_rrc(uint8_t v, bool set_z)
{
    uint8_t carry = v & 1;
    v = (uint8_t)((v >> 1) | (carry << 7));
    R.f = (carry ? FLAG_C : 0) | ((set_z && v == 0) ? FLAG_Z : 0);
    return v;
}

static uint8_t rot_rl(uint8_t v, bool set_z)
{
    /* Rotate THROUGH carry: 9-bit rotate with C as the ninth bit. */
    uint8_t old_c = (R.f & FLAG_C) ? 1 : 0;
    uint8_t carry = v >> 7;
    v = (uint8_t)((v << 1) | old_c);
    R.f = (carry ? FLAG_C : 0) | ((set_z && v == 0) ? FLAG_Z : 0);
    return v;
}

static uint8_t rot_rr(uint8_t v, bool set_z)
{
    uint8_t old_c = (R.f & FLAG_C) ? 1 : 0;
    uint8_t carry = v & 1;
    v = (uint8_t)((v >> 1) | (old_c << 7));
    R.f = (carry ? FLAG_C : 0) | ((set_z && v == 0) ? FLAG_Z : 0);
    return v;
}

static uint8_t shift_sla(uint8_t v)
{
    uint8_t carry = v >> 7;
    v <<= 1;
    R.f = (carry ? FLAG_C : 0) | (v == 0 ? FLAG_Z : 0);
    return v;
}

static uint8_t shift_sra(uint8_t v)
{
    /* Arithmetic shift right: bit 7 is DUPLICATED, preserving the sign. */
    uint8_t carry = v & 1;
    v = (uint8_t)((v >> 1) | (v & 0x80));
    R.f = (carry ? FLAG_C : 0) | (v == 0 ? FLAG_Z : 0);
    return v;
}

static uint8_t shift_srl(uint8_t v)
{
    /* Logical shift right: bit 7 becomes 0. */
    uint8_t carry = v & 1;
    v >>= 1;
    R.f = (carry ? FLAG_C : 0) | (v == 0 ? FLAG_Z : 0);
    return v;
}

static uint8_t op_swap(uint8_t v)
{
    /* SWAP nibbles — an SM83 addition, not on the Z80. Clears N/H/C. */
    v = (uint8_t)((v << 4) | (v >> 4));
    R.f = (v == 0) ? FLAG_Z : 0;
    return v;
}

static void op_daa(void)
{
    /* DAA fixes up A after BCD arithmetic. After ADD, each nibble that
     * overflowed past 9 (detected via H/C or by inspection) needs +6 to
     * skip the 6 unused values of a decimal digit; after SUB (N set) the
     * correction is subtracted, and ONLY the flags decide — you can't
     * inspect nibbles after a subtraction because the borrow already
     * wrapped them. This is the entire reason the N and H flags exist. */
    int a = R.a;
    bool carry = R.f & FLAG_C; /* C is sticky: DAA sets it but never clears
                                * it, so multi-byte BCD sums can chain. */
    if (!(R.f & FLAG_N)) {
        if (carry || a > 0x99)                   { a += 0x60; carry = true; }
        if ((R.f & FLAG_H) || (a & 0x0F) > 0x09)   a += 0x06;
    } else {
        /* After a subtraction only the flags can be trusted (the nibbles
         * already wrapped), and no new carry can be generated. */
        if (carry)        a -= 0x60;
        if (R.f & FLAG_H) a -= 0x06;
    }
    /* Z from result, H always cleared, N preserved. */
    R.f &= ~(FLAG_Z | FLAG_H | FLAG_C);
    if (carry)        R.f |= FLAG_C;
    R.a = (uint8_t)a;
    if (R.a == 0)     R.f |= FLAG_Z;
}

/* ---- decoded register access ------------------------------------------
 * In opcodes 0x40-0xBF and all CB opcodes, a 3-bit field selects the
 * operand in a fixed order: B,C,D,E,H,L,(HL),A. Index 6 is not a register
 * at all but the byte at address HL — that's why every instruction touching
 * it costs 4 extra T-cycles (one more memory access). */

static uint8_t read_r(int idx)
{
    switch (idx) {
    case 0: return R.b;
    case 1: return R.c;
    case 2: return R.d;
    case 3: return R.e;
    case 4: return R.h;
    case 5: return R.l;
    case 6: return mmu_read(get_hl());
    default: return R.a;
    }
}

static void write_r(int idx, uint8_t v)
{
    switch (idx) {
    case 0: R.b = v; break;
    case 1: R.c = v; break;
    case 2: R.d = v; break;
    case 3: R.e = v; break;
    case 4: R.h = v; break;
    case 5: R.l = v; break;
    case 6: mmu_write(get_hl(), v); break;
    default: R.a = v; break;
    }
}

/* Condition codes for JR/JP/CALL/RET cc, in opcode order NZ, Z, NC, C. */
static bool condition(int idx)
{
    switch (idx) {
    case 0: return !(R.f & FLAG_Z);
    case 1: return  (R.f & FLAG_Z);
    case 2: return !(R.f & FLAG_C);
    default: return (R.f & FLAG_C);
    }
}

/* ---- CB-prefixed opcodes ----------------------------------------------
 * The CB page is perfectly regular, which is why it's decoded arithmetically
 * instead of with a 256-case switch:
 *   bits 7-6: 00 = rotate/shift (which one: bits 5-3)
 *             01 = BIT b,r      10 = RES b,r      11 = SET b,r
 *   bits 5-3: bit number (or rotate/shift selector)
 *   bits 2-0: operand register (the B,C,D,E,H,L,(HL),A order above)
 */
static int execute_cb(void)
{
    uint8_t op = fetch8();
    int idx = op & 0x07;
    int bit = (op >> 3) & 0x07;
    uint8_t v = read_r(idx);
    /* Base cost: 8 (fetch CB + fetch op). (HL) adds a read AND a write for
     * the read-modify-write groups: 16 total. BIT only reads, so its (HL)
     * form is 12 — a distinction cycle-counting tests check. */
    int cycles = (idx == 6) ? 16 : 8;

    switch (op >> 6) {
    case 0: /* rotates and shifts; all set Z normally (unlike RLCA etc.) */
        switch (bit) {
        case 0: v = rot_rlc(v, true); break;
        case 1: v = rot_rrc(v, true); break;
        case 2: v = rot_rl(v, true);  break;
        case 3: v = rot_rr(v, true);  break;
        case 4: v = shift_sla(v);     break;
        case 5: v = shift_sra(v);     break;
        case 6: v = op_swap(v);       break;
        default: v = shift_srl(v);    break;
        }
        write_r(idx, v);
        break;
    case 1: /* BIT b,r: Z = complement of the tested bit, N=0, H=1 (another
             * hard-wired H, like AND), C untouched. No write-back. */
        R.f = (R.f & FLAG_C) | FLAG_H | ((v & (1 << bit)) ? 0 : FLAG_Z);
        if (idx == 6)
            cycles = 12;
        break;
    case 2: /* RES b,r: clear the bit. No flags. */
        write_r(idx, v & ~(1 << bit));
        break;
    default: /* SET b,r: set the bit. No flags. */
        write_r(idx, v | (1 << bit));
        break;
    }
    return cycles;
}

/* ---- interrupt dispatch ------------------------------------------------ */

static int service_interrupts(void)
{
    /* An interrupt is "pending" when the hardware requested it (IF) AND the
     * game enabled it (IE). Pending interrupts wake the CPU from HALT even
     * with IME off — that's how "HALT until VBlank" works with DI held. */
    uint8_t pending = mmu_read(0xFF0F) & mmu_read(0xFFFF) & 0x1F;
    if (pending)
        halted = false;
    if (!ime || !pending)
        return 0;

    /* Lowest bit number wins: VBlank beats STAT beats Timer, etc. */
    for (int bit = 0; bit < 5; bit++) {
        if (pending & (1 << bit)) {
            /* The dispatch sequence, as hardware does it: clear IME (so the
             * handler isn't immediately re-entered), acknowledge by clearing
             * this one IF bit, push PC, jump to the fixed vector. Takes 5
             * M-cycles = 20 T-cycles. */
            ime = false;
            mmu_write(0xFF0F, mmu_read(0xFF0F) & ~(1 << bit));
            push16(R.pc);
            R.pc = (uint16_t)(0x40 + bit * 8);
            return 20;
        }
    }
    return 0;
}

/* ---- the main step ----------------------------------------------------- */

int cpu_step(void)
{
    /* EI's delayed effect: enable IME now, but only if the flag was set by
     * the PREVIOUS instruction (see ime_pending comment at the top). */
    bool enable_ime_after = ime_pending;

    int icycles = service_interrupts();
    if (icycles)
        return icycles;

    if (halted)
        return 4; /* clock keeps running while halted; burn one M-cycle */

    uint8_t op = fetch8();
    int cycles;

    /* The two regular quadrants first (they cover half the opcode space): */

    if (op >= 0x40 && op <= 0x7F && op != 0x76) {
        /* LD r,r': bits 5-3 = destination, bits 2-0 = source. */
        int dst = (op >> 3) & 7, src = op & 7;
        write_r(dst, read_r(src));
        cycles = (dst == 6 || src == 6) ? 8 : 4;
    } else if (op >= 0x80 && op <= 0xBF) {
        /* ALU A,r: bits 5-3 select the operation in the fixed order
         * ADD,ADC,SUB,SBC,AND,XOR,OR,CP. Flags documented on each helper. */
        uint8_t v = read_r(op & 7);
        switch ((op >> 3) & 7) {
        case 0: alu_add(v, 0); break;
        case 1: alu_add(v, R.f & FLAG_C); break;
        case 2: alu_sub(v, 0, true); break;
        case 3: alu_sub(v, R.f & FLAG_C, true); break;
        case 4: alu_and(v); break;
        case 5: alu_xor(v); break;
        case 6: alu_or(v); break;
        default: alu_sub(v, 0, false); break; /* CP: compare, discard */
        }
        cycles = ((op & 7) == 6) ? 8 : 4;
    } else switch (op) {

    /* -- 0x00-0x3F: loads, 16-bit ops, control -- */
    case 0x00: cycles = 4; break; /* NOP */
    case 0x10:
        /* STOP: halts CPU and LCD until a button press; games basically
         * never use it on DMG (it's for power saving / CGB speed switch).
         * It's a 2-byte instruction, so skip the padding byte and move on. */
        fetch8();
        cycles = 4;
        break;

    case 0x01: set_bc(fetch16()); cycles = 12; break; /* LD rr,d16 */
    case 0x11: set_de(fetch16()); cycles = 12; break;
    case 0x21: set_hl(fetch16()); cycles = 12; break;
    case 0x31: R.sp = fetch16();  cycles = 12; break;

    case 0x02: mmu_write(get_bc(), R.a); cycles = 8; break; /* LD (rr),A */
    case 0x12: mmu_write(get_de(), R.a); cycles = 8; break;
    case 0x0A: R.a = mmu_read(get_bc()); cycles = 8; break; /* LD A,(rr) */
    case 0x1A: R.a = mmu_read(get_de()); cycles = 8; break;

    /* LD (HL+)/(HL-): store/load through HL then increment/decrement it.
     * SM83-specific; exists because copying tile data is the Game Boy's
     * national sport. */
    case 0x22: mmu_write(get_hl(), R.a); set_hl(get_hl() + 1); cycles = 8; break;
    case 0x32: mmu_write(get_hl(), R.a); set_hl(get_hl() - 1); cycles = 8; break;
    case 0x2A: R.a = mmu_read(get_hl()); set_hl(get_hl() + 1); cycles = 8; break;
    case 0x3A: R.a = mmu_read(get_hl()); set_hl(get_hl() - 1); cycles = 8; break;

    /* INC/DEC rr: pure 16-bit counter ops, NO flags affected at all (they
     * run through the address-increment unit, not the ALU). */
    case 0x03: set_bc(get_bc() + 1); cycles = 8; break;
    case 0x13: set_de(get_de() + 1); cycles = 8; break;
    case 0x23: set_hl(get_hl() + 1); cycles = 8; break;
    case 0x33: R.sp++;               cycles = 8; break;
    case 0x0B: set_bc(get_bc() - 1); cycles = 8; break;
    case 0x1B: set_de(get_de() - 1); cycles = 8; break;
    case 0x2B: set_hl(get_hl() - 1); cycles = 8; break;
    case 0x3B: R.sp--;               cycles = 8; break;

    /* INC/DEC r (flags on the helpers: Z/N/H, C preserved). */
    case 0x04: R.b = alu_inc8(R.b); cycles = 4; break;
    case 0x0C: R.c = alu_inc8(R.c); cycles = 4; break;
    case 0x14: R.d = alu_inc8(R.d); cycles = 4; break;
    case 0x1C: R.e = alu_inc8(R.e); cycles = 4; break;
    case 0x24: R.h = alu_inc8(R.h); cycles = 4; break;
    case 0x2C: R.l = alu_inc8(R.l); cycles = 4; break;
    case 0x34: mmu_write(get_hl(), alu_inc8(mmu_read(get_hl()))); cycles = 12; break;
    case 0x3C: R.a = alu_inc8(R.a); cycles = 4; break;
    case 0x05: R.b = alu_dec8(R.b); cycles = 4; break;
    case 0x0D: R.c = alu_dec8(R.c); cycles = 4; break;
    case 0x15: R.d = alu_dec8(R.d); cycles = 4; break;
    case 0x1D: R.e = alu_dec8(R.e); cycles = 4; break;
    case 0x25: R.h = alu_dec8(R.h); cycles = 4; break;
    case 0x2D: R.l = alu_dec8(R.l); cycles = 4; break;
    case 0x35: mmu_write(get_hl(), alu_dec8(mmu_read(get_hl()))); cycles = 12; break;
    case 0x3D: R.a = alu_dec8(R.a); cycles = 4; break;

    /* LD r,d8 */
    case 0x06: R.b = fetch8(); cycles = 8; break;
    case 0x0E: R.c = fetch8(); cycles = 8; break;
    case 0x16: R.d = fetch8(); cycles = 8; break;
    case 0x1E: R.e = fetch8(); cycles = 8; break;
    case 0x26: R.h = fetch8(); cycles = 8; break;
    case 0x2E: R.l = fetch8(); cycles = 8; break;
    case 0x36: mmu_write(get_hl(), fetch8()); cycles = 12; break;
    case 0x3E: R.a = fetch8(); cycles = 8; break;

    /* A-register rotates: Z FORCED to 0 (see the rotate helpers' comment). */
    case 0x07: R.a = rot_rlc(R.a, false); cycles = 4; break; /* RLCA */
    case 0x0F: R.a = rot_rrc(R.a, false); cycles = 4; break; /* RRCA */
    case 0x17: R.a = rot_rl(R.a, false);  cycles = 4; break; /* RLA  */
    case 0x1F: R.a = rot_rr(R.a, false);  cycles = 4; break; /* RRA  */

    case 0x08: { /* LD (a16),SP — the only 16-bit store; 20 cycles (5 memory
                  * accesses: opcode, 2 address bytes, 2 data bytes). */
        uint16_t addr = fetch16();
        mmu_write(addr, R.sp & 0xFF);
        mmu_write(addr + 1, R.sp >> 8);
        cycles = 20;
        break;
    }

    case 0x09: add_hl(get_bc()); cycles = 8; break; /* ADD HL,rr */
    case 0x19: add_hl(get_de()); cycles = 8; break;
    case 0x29: add_hl(get_hl()); cycles = 8; break;
    case 0x39: add_hl(R.sp);     cycles = 8; break;

    case 0x18: { /* JR e8: unconditional relative jump, always 12 cycles. */
        int8_t off = (int8_t)fetch8();
        R.pc = (uint16_t)(R.pc + off);
        cycles = 12;
        break;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: { /* JR cc,e8 */
        int8_t off = (int8_t)fetch8();
        /* Conditional jumps cost extra ONLY when taken: the extra M-cycle
         * is the address recomputation the CPU skips on the fall-through
         * path. Same principle for JP/CALL/RET cc below. */
        if (condition((op >> 3) & 3)) {
            R.pc = (uint16_t)(R.pc + off);
            cycles = 12;
        } else {
            cycles = 8;
        }
        break;
    }

    case 0x27: op_daa(); cycles = 4; break;
    case 0x2F: /* CPL: A = ~A. Sets N and H (both, always), Z/C untouched. */
        R.a = ~R.a;
        R.f |= FLAG_N | FLAG_H;
        cycles = 4;
        break;
    case 0x37: /* SCF: C=1, N=0, H=0, Z untouched. */
        R.f = (R.f & FLAG_Z) | FLAG_C;
        cycles = 4;
        break;
    case 0x3F: /* CCF: C flipped (not set!), N=0, H=0, Z untouched. */
        R.f = (uint8_t)((R.f & (FLAG_Z | FLAG_C)) ^ FLAG_C);
        cycles = 4;
        break;

    case 0x76: /* HALT */
        if (!ime && (mmu_read(0xFF0F) & mmu_read(0xFFFF) & 0x1F)) {
            /* THE HALT BUG. HALT with IME=0 and an interrupt already
             * pending doesn't halt — and a hardware race corrupts the next
             * fetch so PC fails to increment, executing the following byte
             * twice. E.g. "HALT ; INC A" increments A twice. We set a flag
             * that fetch8() consumes. */
            halt_bug = true;
        } else {
            halted = true;
        }
        cycles = 4;
        break;

    /* -- 0xC0-0xFF: stack, jumps, calls, misc -- */

    case 0xC0: case 0xC8: case 0xD0: case 0xD8: /* RET cc */
        if (condition((op >> 3) & 3)) {
            R.pc = pop16();
            cycles = 20;
        } else {
            cycles = 8;
        }
        break;
    case 0xC9: R.pc = pop16(); cycles = 16; break; /* RET */
    case 0xD9: /* RETI = RET + enable interrupts IMMEDIATELY (no EI delay). */
        R.pc = pop16();
        ime = true;
        cycles = 16;
        break;

    case 0xC1: set_bc(pop16()); cycles = 12; break; /* POP rr */
    case 0xD1: set_de(pop16()); cycles = 12; break;
    case 0xE1: set_hl(pop16()); cycles = 12; break;
    case 0xF1: { /* POP AF: low nibble of F is hard-wired to 0 — flags that
                  * don't exist can't be popped into existence. */
        uint16_t v = pop16();
        R.a = v >> 8;
        R.f = v & 0xF0;
        cycles = 12;
        break;
    }

    case 0xC5: push16(get_bc()); cycles = 16; break; /* PUSH rr */
    case 0xD5: push16(get_de()); cycles = 16; break;
    case 0xE5: push16(get_hl()); cycles = 16; break;
    case 0xF5: push16((uint16_t)(R.a << 8) | R.f); cycles = 16; break;

    case 0xC3: R.pc = fetch16(); cycles = 16; break; /* JP a16 */
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {   /* JP cc,a16 */
        uint16_t target = fetch16(); /* operand is fetched either way */
        if (condition((op >> 3) & 3)) {
            R.pc = target;
            cycles = 16;
        } else {
            cycles = 12;
        }
        break;
    }
    case 0xE9: R.pc = get_hl(); cycles = 4; break; /* JP HL: no memory access
                  beyond the fetch, hence the unusually cheap 4 cycles */

    case 0xCD: { /* CALL a16 */
        uint16_t target = fetch16();
        push16(R.pc); /* PC already points past the operand = return addr */
        R.pc = target;
        cycles = 24;
        break;
    }
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: { /* CALL cc,a16 */
        uint16_t target = fetch16();
        if (condition((op >> 3) & 3)) {
            push16(R.pc);
            R.pc = target;
            cycles = 24;
        } else {
            cycles = 12;
        }
        break;
    }

    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        /* RST n: a 1-byte CALL to a fixed low address (0x00,0x08,...0x38).
         * The target is encoded in bits 5-3 of the opcode itself. */
        push16(R.pc);
        R.pc = (uint16_t)(op & 0x38);
        cycles = 16;
        break;

    /* ALU A,d8 — same flag rules as the register forms. */
    case 0xC6: alu_add(fetch8(), 0);              cycles = 8; break;
    case 0xCE: alu_add(fetch8(), R.f & FLAG_C);   cycles = 8; break;
    case 0xD6: alu_sub(fetch8(), 0, true);        cycles = 8; break;
    case 0xDE: alu_sub(fetch8(), R.f & FLAG_C, true); cycles = 8; break;
    case 0xE6: alu_and(fetch8());                 cycles = 8; break;
    case 0xEE: alu_xor(fetch8());                 cycles = 8; break;
    case 0xF6: alu_or(fetch8());                  cycles = 8; break;
    case 0xFE: alu_sub(fetch8(), 0, false);       cycles = 8; break; /* CP */

    /* LDH: load "high" — the operand is an offset into 0xFF00-0xFFFF, the
     * I/O + HRAM page. Exists because games hammer I/O registers and a
     * 1-byte offset is faster than a full 16-bit address. */
    case 0xE0: mmu_write(0xFF00 + fetch8(), R.a); cycles = 12; break;
    case 0xF0: R.a = mmu_read(0xFF00 + fetch8()); cycles = 12; break;
    case 0xE2: mmu_write(0xFF00 + R.c, R.a);      cycles = 8;  break;
    case 0xF2: R.a = mmu_read(0xFF00 + R.c);      cycles = 8;  break;

    case 0xEA: mmu_write(fetch16(), R.a); cycles = 16; break; /* LD (a16),A */
    case 0xFA: R.a = mmu_read(fetch16()); cycles = 16; break; /* LD A,(a16) */

    case 0xE8: R.sp = sp_plus_offset(); cycles = 16; break; /* ADD SP,e8 */
    case 0xF8: set_hl(sp_plus_offset()); cycles = 12; break; /* LD HL,SP+e8 */
    case 0xF9: R.sp = get_hl(); cycles = 8; break; /* LD SP,HL */

    case 0xF3: /* DI: takes effect immediately, and also cancels a pending
                * EI (an "EI ; DI" pair never enables anything). */
        ime = false;
        ime_pending = false;
        enable_ime_after = false;
        cycles = 4;
        break;
    case 0xFB: /* EI: delayed one instruction — see ime_pending above. */
        ime_pending = true;
        cycles = 4;
        break;

    case 0xCB: cycles = execute_cb(); break;

    default:
        /* 0xD3,0xDB,0xDD,0xE3,0xE4,0xEB,0xEC,0xED,0xF4,0xFC,0xFD are holes
         * in the opcode map. Real hardware LOCKS UP executing them (the
         * decoder wedges). Treating them as NOP keeps a derailed game
         * visibly running instead of freezing the emulator, which is more
         * useful when debugging your own CPU bugs. */
        cycles = 4;
        break;
    }

    if (enable_ime_after) {
        ime = true;
        ime_pending = false;
    }
    return cycles;
}
