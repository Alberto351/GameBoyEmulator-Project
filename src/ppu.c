/* ppu.c — the pixel-processing unit, rendered a scanline at a time.
 *
 * The real PPU is a pipeline that pushes one pixel per dot; emulating that
 * exactly is only needed for a handful of effects no Tetris-era game uses.
 * Instead we track the PPU's TIMING exactly (modes, LY, interrupts — that's
 * what games observe and race against) but render each scanline in one shot
 * when it enters HBlank.
 *
 * TIMING. The PPU and CPU share the 4194304 Hz clock; one PPU "dot" is one
 * T-cycle. Every scanline is exactly 456 dots, and a frame is 154 lines
 * (144 visible + 10 VBlank) = 70224 dots ≈ 59.7 frames/sec. Within a
 * visible line the PPU walks through its modes like this:
 *
 *   dot   0-79    mode 2  OAM scan     (which sprites hit this line?)
 *   dot  80-251   mode 3  drawing      (pixels pushed to the LCD; length
 *                                       actually varies 172-289 dots with
 *                                       scrolling and sprites — we use the
 *                                       minimum 172, see below)
 *   dot 252-455   mode 0  HBlank       (padding until 456)
 *
 * Lines 144-153 are mode 1, VBlank: 4560 dots in which VRAM is all yours.
 *
 * Why the modes matter to the CPU: during mode 3 the PPU owns VRAM, and
 * during modes 2-3 it owns OAM; CPU accesses then read 0xFF/get dropped on
 * real hardware. Well-behaved games therefore poll STAT's mode bits or wait
 * for interrupts before touching video memory. We do NOT emulate the access
 * blocking (accesses always succeed here): correct games can't tell the
 * difference, and buggy homebrew shows glitches instead of mysteries —
 * friendlier for learning. The fixed 172-dot mode 3 is the same tradeoff:
 * games only race the mode boundary for mid-line raster tricks, which
 * DMG-era titles (and certainly Tetris) don't do.
 */
#include "ppu.h"

#include <string.h>

#include "mmu.h"

/* LCDC (0xFF40) bits — the master control register. */
#define LCDC_ENABLE      0x80 /* LCD on/off */
#define LCDC_WIN_MAP     0x40 /* window tilemap: 0=0x9800, 1=0x9C00 */
#define LCDC_WIN_ENABLE  0x20
#define LCDC_TILE_DATA   0x10 /* BG/window tile data addressing, see below */
#define LCDC_BG_MAP      0x08 /* background tilemap: 0=0x9800, 1=0x9C00 */
#define LCDC_OBJ_SIZE    0x04 /* sprites: 0=8x8, 1=8x16 */
#define LCDC_OBJ_ENABLE  0x02
#define LCDC_BG_ENABLE   0x01 /* 0 = background AND window blank (DMG) */

/* STAT (0xFF41) bits. */
#define STAT_LYC_INT     0x40 /* interrupt sources the game can enable...  */
#define STAT_MODE2_INT   0x20
#define STAT_MODE1_INT   0x10
#define STAT_MODE0_INT   0x08
#define STAT_LYC_EQUAL   0x04 /* ...and read-only status: LY==LYC,        */
                              /* plus the current mode in bits 1-0.       */

#define MODE_HBLANK 0
#define MODE_VBLANK 1
#define MODE_OAM    2
#define MODE_DRAW   3

static uint8_t vram[0x2000];
static uint8_t oam[0xA0];

static uint8_t lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx;

static int  dot;         /* position within the current scanline, 0..455 */
static int  window_line; /* the window's own line counter, see render     */
static bool frame_done;

uint32_t ppu_framebuffer[LCD_WIDTH * LCD_HEIGHT];

/* The classic DMG green-grey shades, index 0 (lightest) to 3 (darkest). */
static const uint32_t shades[4] = {
    0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820,
};

void ppu_init(void)
{
    memset(vram, 0, sizeof vram);
    memset(oam, 0, sizeof oam);
    /* Post-boot register values: LCD on, BG on, tile data at 0x8000. */
    lcdc = 0x91;
    stat = 0x85;
    bgp  = 0xFC;
    obp0 = obp1 = 0xFF; /* actually uninitialized on hardware; FF is common */
    scy = scx = ly = lyc = wy = wx = 0;
    dot = 0;
    window_line = 0;
}

bool ppu_frame_ready(void)
{
    bool ready = frame_done;
    frame_done = false;
    return ready;
}

/* ---- scanline rendering ------------------------------------------------ */

/* Decode one pixel from a tile row. Tiles are 2 BITPLANES per row: byte 0
 * holds the low bit of all 8 pixels, byte 1 the high bit. This layout is
 * the single most confusing thing about GB graphics — a pixel's 2-bit color
 * index is split across two bytes. Bit 7 is the LEFTMOST pixel. */
static int tile_pixel(uint16_t tile_row_addr, int x)
{
    uint8_t lo = vram[tile_row_addr];
    uint8_t hi = vram[tile_row_addr + 1];
    int bit = 7 - x;
    return ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
}

/* Look up a BG/window tile's pixel-data address from its tilemap index.
 * Two addressing modes (LCDC bit 4):
 *   1: base 0x8000, tile number is UNSIGNED 0-255
 *   0: base 0x9000, tile number is SIGNED -128..127
 * Mode 0 exists so the BG can share tiles 128-255 with sprites while
 * keeping its own 128 at 0x9000-0x97FF. Forgetting the signed cast here is
 * a rite of passage: the screen shows garbage for exactly half the tiles. */
static uint16_t tile_data_addr(uint8_t tile_num, int row)
{
    if (lcdc & LCDC_TILE_DATA)
        return (uint16_t)(tile_num * 16 + row * 2);
    return (uint16_t)(0x1000 + (int8_t)tile_num * 16 + row * 2);
}

/* Apply a palette register to a 2-bit color index. Palettes exist so games
 * can remap/flash colors without rewriting tiles: BGP/OBP hold four 2-bit
 * shades packed into one byte, index N in bits 2N+1..2N. */
static int palette_lookup(uint8_t palette, int index)
{
    return (palette >> (index * 2)) & 3;
}

static void render_scanline(void)
{
    /* bg_index keeps the RAW color index (before palette) of the BG/window
     * pixel, because sprite priority is decided against the index: a sprite
     * with the "behind background" flag still shows over BG index 0. */
    int bg_index[LCD_WIDTH] = {0};

    /* -- background + window -- */
    if (lcdc & LCDC_BG_ENABLE) {
        /* Does the window start somewhere on this line? WX is offset by 7
         * (WX=7 puts the window at the left edge) — a hardware oddity that
         * lets WX=0..6 partially hide it off-screen. */
        bool window_here = (lcdc & LCDC_WIN_ENABLE) && ly >= wy && wx <= 166;

        for (int x = 0; x < LCD_WIDTH; x++) {
            bool in_window = window_here && x >= wx - 7;
            uint16_t map_base;
            int px, py; /* pixel position within the 256x256 tilemap plane */

            if (in_window) {
                /* The window has NO scrolling; it renders its map from its
                 * own top-left. Crucially it uses its own line counter, not
                 * LY: if the window is disabled for some lines and re-
                 * enabled, it resumes where it left off. */
                map_base = (lcdc & LCDC_WIN_MAP) ? 0x1C00 : 0x1800;
                px = x - (wx - 7);
                py = window_line;
            } else {
                /* The BG scrolls: SCX/SCY pick the top-left of the visible
                 * 160x144 view into the 256x256 plane, wrapping around. */
                map_base = (lcdc & LCDC_BG_MAP) ? 0x1C00 : 0x1800;
                px = (x + scx) & 0xFF;
                py = (ly + scy) & 0xFF;
            }

            /* The tilemap is 32x32 bytes, one tile number per 8x8 tile. */
            uint8_t tile_num = vram[map_base + (py / 8) * 32 + (px / 8)];
            bg_index[x] = tile_pixel(tile_data_addr(tile_num, py % 8), px % 8);
        }

        if (window_here && wx - 7 < LCD_WIDTH)
            window_line++; /* only lines the window actually rendered count */
    }
    /* else: LCDC bit 0 clear blanks BG and window to white on DMG.
     * bg_index stays 0, which palette index 0 of BGP maps below. */

    /* Palette-translate the BG into the framebuffer before sprites. */
    uint32_t *line = &ppu_framebuffer[ly * LCD_WIDTH];
    for (int x = 0; x < LCD_WIDTH; x++)
        line[x] = shades[palette_lookup(bgp, bg_index[x])];

    /* -- sprites -- */
    if (!(lcdc & LCDC_OBJ_ENABLE))
        return;

    int height = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

    /* Mode 2's OAM scan: walk OAM in order and keep the first 10 sprites
     * that overlap this line. The 10-sprite limit is real hardware (the
     * sprite FIFO has 10 slots) and games exploit it to hide things. */
    int selected[10], count = 0;
    for (int i = 0; i < 40 && count < 10; i++) {
        int sy = oam[i * 4] - 16; /* OAM Y is stored +16 so sprites can hang
                                   * off the top edge of the screen */
        if (ly >= sy && ly < sy + height)
            selected[count++] = i;
    }

    /* DMG priority between overlapping sprites: SMALLER X WINS, ties broken
     * by lower OAM index. (CGB drops the X rule; that's a common source of
     * confusion in docs.) Sorting by X descending and drawing in that order
     * makes the highest-priority sprite paint last, which is the simplest
     * way to express the rule. The insertion sort is stable-in-reverse so
     * equal X keeps higher OAM index earlier (= painted first = loses). */
    for (int i = 1; i < count; i++) {
        int s = selected[i], j = i - 1;
        while (j >= 0 && oam[selected[j] * 4 + 1] < oam[s * 4 + 1]) {
            selected[j + 1] = selected[j];
            j--;
        }
        selected[j + 1] = s;
    }

    for (int i = 0; i < count; i++) {
        const uint8_t *e = &oam[selected[i] * 4];
        int sy      = e[0] - 16;
        int sx      = e[1] - 8; /* X is stored +8, same edge-clipping trick */
        uint8_t tile  = e[2];
        uint8_t flags = e[3];
        bool behind_bg = flags & 0x80; /* "priority": behind BG colors 1-3 */
        bool flip_y    = flags & 0x40;
        bool flip_x    = flags & 0x20;
        uint8_t palette = (flags & 0x10) ? obp1 : obp0;

        int row = ly - sy;
        if (flip_y)
            row = height - 1 - row;
        /* In 8x16 mode the tile number's low bit is ignored: the sprite
         * uses tiles N&~1 (top) and N|1 (bottom). */
        if (height == 16)
            tile = (row < 8) ? (tile & 0xFE) : (tile | 0x01);
        /* Sprites always use the 0x8000 unsigned addressing mode. */
        uint16_t row_addr = (uint16_t)(tile * 16 + (row % 8) * 2);

        for (int px = 0; px < 8; px++) {
            int x = sx + px;
            if (x < 0 || x >= LCD_WIDTH)
                continue;
            int index = tile_pixel(row_addr, flip_x ? 7 - px : px);
            /* Sprite color index 0 is ALWAYS transparent (that's why OBP
             * palettes only meaningfully hold 3 colors). */
            if (index == 0)
                continue;
            /* The behind-BG flag hides the sprite behind BG indices 1-3
             * but NOT behind index 0 — that's how sprites walk "behind"
             * scenery while still showing against the sky. */
            if (behind_bg && bg_index[x] != 0)
                continue;
            line[x] = shades[palette_lookup(palette, index)];
        }
    }
}

/* ---- mode state machine ------------------------------------------------ */

static void set_mode(int mode)
{
    stat = (uint8_t)((stat & ~0x03) | mode);
    /* Each mode entry can raise a STAT interrupt if the game enabled that
     * source. (Real hardware ORs all sources into one line and interrupts
     * only on its rising edge — "STAT blocking". We fire per-event, which
     * is right unless a game deliberately overlaps sources; DMG-era games
     * don't.) */
    static const uint8_t mode_int[3] = { STAT_MODE0_INT, STAT_MODE1_INT,
                                         STAT_MODE2_INT };
    if (mode <= MODE_OAM && (stat & mode_int[mode]))
        mmu_request_interrupt(INT_STAT);
}

static void check_lyc(void)
{
    if (ly == lyc) {
        stat |= STAT_LYC_EQUAL;
        if (stat & STAT_LYC_INT)
            mmu_request_interrupt(INT_STAT);
    } else {
        stat &= (uint8_t)~STAT_LYC_EQUAL;
    }
}

void ppu_step(int cycles)
{
    /* LCD off: the PPU is fully stopped — LY pinned to 0, mode 0, no
     * interrupts, screen blank. Games toggle this only during VBlank
     * (turning it off mid-frame damages real DMG screens). */
    if (!(lcdc & LCDC_ENABLE))
        return;

    /* Advance dot by dot. As with the timer, an instruction is at most ~24
     * dots, so the loop is cheap and no boundary can be stepped over. */
    for (int i = 0; i < cycles; i++) {
        dot++;

        if (ly < LCD_HEIGHT) {
            /* Visible line: 2 -> 3 -> 0 at the exact dot boundaries from
             * the file comment. */
            if (dot == 80) {
                set_mode(MODE_DRAW);
            } else if (dot == 80 + 172) {
                /* Entering HBlank: the line's pixels are now final on real
                 * hardware, so this is the one honest moment to render. */
                render_scanline();
                set_mode(MODE_HBLANK);
            }
        }

        if (dot == 456) {
            /* End of scanline. */
            dot = 0;
            ly++;
            if (ly == LCD_HEIGHT) {
                /* First VBlank line: THE frame heartbeat. Nearly every game
                 * syncs its whole main loop to this interrupt. */
                set_mode(MODE_VBLANK);
                mmu_request_interrupt(INT_VBLANK);
                frame_done = true;
                window_line = 0; /* window line counter resets per frame */
            } else if (ly > 153) {
                /* Wrap to the top: line 0 begins with OAM scan. */
                ly = 0;
                set_mode(MODE_OAM);
            } else if (ly < LCD_HEIGHT) {
                set_mode(MODE_OAM);
            }
            check_lyc(); /* LY changed, re-evaluate the LY==LYC flag */
        }
    }
}

/* ---- MMU-facing register/memory access -------------------------------- */

uint8_t ppu_read(uint16_t addr)
{
    if (addr >= 0x8000 && addr <= 0x9FFF)
        return vram[addr - 0x8000];
    if (addr >= 0xFE00 && addr <= 0xFE9F)
        return oam[addr - 0xFE00];
    switch (addr) {
    case 0xFF40: return lcdc;
    case 0xFF41: return stat | 0x80; /* bit 7 unwired, reads 1 */
    case 0xFF42: return scy;
    case 0xFF43: return scx;
    case 0xFF44: return ly;   /* read-only: games poll this constantly */
    case 0xFF45: return lyc;
    case 0xFF47: return bgp;
    case 0xFF48: return obp0;
    case 0xFF49: return obp1;
    case 0xFF4A: return wy;
    case 0xFF4B: return wx;
    default:     return 0xFF; /* includes 0xFF46: DMA is write-only here */
    }
}

void ppu_write(uint16_t addr, uint8_t value)
{
    if (addr >= 0x8000 && addr <= 0x9FFF) {
        vram[addr - 0x8000] = value;
        return;
    }
    if (addr >= 0xFE00 && addr <= 0xFE9F) {
        oam[addr - 0xFE00] = value;
        return;
    }
    switch (addr) {
    case 0xFF40: {
        bool was_on = lcdc & LCDC_ENABLE;
        lcdc = value;
        if (was_on && !(lcdc & LCDC_ENABLE)) {
            /* Turning the LCD off resets the beam position immediately. */
            ly = 0;
            dot = 0;
            stat &= (uint8_t)~0x03; /* mode 0 while off */
        }
        break;
    }
    case 0xFF41:
        /* Only the interrupt-enable bits are writable; mode and LYC-equal
         * are status outputs the game can't set. */
        stat = (uint8_t)((stat & 0x07) | (value & 0x78));
        break;
    case 0xFF42: scy = value; break;
    case 0xFF43: scx = value; break;
    case 0xFF44: break; /* LY is read-only */
    case 0xFF45: lyc = value; check_lyc(); break;
    case 0xFF47: bgp  = value; break;
    case 0xFF48: obp0 = value; break;
    case 0xFF49: obp1 = value; break;
    case 0xFF4A: wy = value; break;
    case 0xFF4B: wx = value; break;
    default: break;
    }
}
