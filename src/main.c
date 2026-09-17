/* main.c — SDL window, input, and the loop that ties the machine together.
 *
 * The synchronization scheme, which is the heart of any emulator: the CPU
 * is the only component that "runs"; everything else is dragged along. Each
 * cpu_step() returns how many T-cycles the instruction took, and we hand
 * exactly that many cycles to the PPU and timer. No threads, no clocks —
 * simulated time only advances when the CPU executes. Real time enters the
 * picture once per frame, when we sleep to hold ~59.7 fps.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <SDL2/SDL.h>

#include "cartridge.h"
#include "cpu.h"
#include "mmu.h"
#include "ppu.h"
#include "timer.h"

#define WINDOW_SCALE 4 /* 160x144 is tiny on a modern monitor */

/* One frame is exactly 70224 T-cycles (154 lines x 456 dots), so at the
 * 4194304 Hz master clock a frame lasts 70224/4194304 ≈ 16.742 ms. That's
 * 59.73 fps — close to, but NOT, 60; syncing to a 60 Hz display would run
 * the game ~0.5% fast. */
static const double FRAME_MS = 70224.0 / 4194304.0 * 1000.0;

/* Poll SDL's keyboard state into the two active-high nibbles the joypad
 * register wants (mmu.c does the active-low inversion the hardware uses). */
static void update_input(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint8_t dpad = 0, buttons = 0;
    if (k[SDL_SCANCODE_RIGHT])  dpad |= 0x01;
    if (k[SDL_SCANCODE_LEFT])   dpad |= 0x02;
    if (k[SDL_SCANCODE_UP])     dpad |= 0x04;
    if (k[SDL_SCANCODE_DOWN])   dpad |= 0x08;
    if (k[SDL_SCANCODE_A])      buttons |= 0x01; /* A */
    if (k[SDL_SCANCODE_D])      buttons |= 0x02; /* B */
    if (k[SDL_SCANCODE_RSHIFT]) buttons |= 0x04; /* Select */
    if (k[SDL_SCANCODE_RETURN]) buttons |= 0x08; /* Start */
    mmu_set_input(dpad, buttons);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s rom.gb\n", argv[0]);
        return 1;
    }
    if (!cart_load(argv[1]))
        return 1;

    mmu_init();
    timer_init();
    ppu_init();
    cpu_init(); /* registers get post-boot values; PC starts at 0x0100 */

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("gbemu",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LCD_WIDTH * WINDOW_SCALE, LCD_HEIGHT * WINDOW_SCALE, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC);
    /* One streaming texture the size of the LCD; the renderer scales it up.
     * ARGB8888 matches the pixel format ppu.c writes. */
    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        LCD_WIDTH, LCD_HEIGHT);

    bool running = true;
    double next_frame = SDL_GetTicks64();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                running = false;
        }
        update_input();

        /* Run the machine until the PPU finishes a frame. The fetch-
         * execute-advance triad below IS the emulator; everything else in
         * this file is presentation. */
        while (!ppu_frame_ready()) {
            int cycles = cpu_step();
            ppu_step(cycles);
            timer_step(cycles);
        }

        SDL_UpdateTexture(texture, NULL, ppu_framebuffer,
                          LCD_WIDTH * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Pace to the DMG's real frame rate. The accumulator ("next_frame
         * += FRAME_MS" rather than "now + FRAME_MS") keeps long-run speed
         * exact even though each individual SDL_Delay is only ms-precise. */
        next_frame += FRAME_MS;
        double now = (double)SDL_GetTicks64();
        if (next_frame > now)
            SDL_Delay((Uint32)(next_frame - now));
        else if (now - next_frame > 100.0)
            next_frame = now; /* fell badly behind (e.g. window drag): don't
                               * fast-forward to catch up, just resync */
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    cart_free();
    return 0;
}
