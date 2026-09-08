// SKOKAN, playable on macOS and in the browser. Same src/game.cpp the board
// runs -- this file only supplies what the ESP32 normally would: a screen, a
// clock and a button.
//
//   make play     native macOS app
//   make web      WebAssembly build for GitHub Pages
//
//   SPACE / UP / W / click / tap .... the arcade button (tap, hold, double-tap)
//   1 / 2 / 3 ...................... hand over to the 4yo / 8yo / ace bot
//   0 .............................. take the controls back
//   F .............................. toggle 2x / 3x window (native only)
//   ESC / Q ........................ quit (native only)
//
// The panel is a 320x240 RGB565 framebuffer in host/panel.cpp; here it is
// uploaded to a texture every frame. Nothing in the game knows the difference.
#include <SDL.h>
#include <Adafruit_SPITFT.h>
#include <Arduino.h>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "../src/display.h"

void setup();
void loop();
void gameSetBot(int profile, bool drive);
const char *gameBotName(int profile);

// The desktop harness uses this to grab screenshots; nothing to do when a
// human is watching in real time.
void hostFrameDone() {}

static const int PANEL_W = 320, PANEL_H = 240;

static SDL_Window   *win = nullptr;
static SDL_Renderer *ren = nullptr;
static SDL_Texture  *tex = nullptr;
static bool   running = true, keyHeld = false;
static Uint32 t0      = 0;
static int    scale   = 3;

static void frame() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT: running = false; break;
      case SDL_KEYDOWN:
        if (e.key.repeat) break;
        switch (e.key.keysym.sym) {
#ifndef __EMSCRIPTEN__
          case SDLK_ESCAPE: case SDLK_q: running = false; break;
          case SDLK_f:
            scale = (scale == 3) ? 2 : 3;
            SDL_SetWindowSize(win, PANEL_W * scale, PANEL_H * scale);
            break;
#endif
          case SDLK_SPACE: case SDLK_UP: case SDLK_w: keyHeld = true; break;
          case SDLK_1: case SDLK_2: case SDLK_3: {
            const int p = e.key.keysym.sym - SDLK_1;
            gameSetBot(p, true);
            printf("[play] bot \"%s\" has the controls\n", gameBotName(p));
            break;
          }
          case SDLK_0: gameSetBot(1, false); printf("[play] you have the controls\n"); break;
          default: break;
        }
        break;
      case SDL_KEYUP:
        if (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_UP ||
            e.key.keysym.sym == SDLK_w) keyHeld = false;
        break;
      case SDL_MOUSEBUTTONDOWN: case SDL_FINGERDOWN: keyHeld = true;  break;
      case SDL_MOUSEBUTTONUP:   case SDL_FINGERUP:   keyHeld = false; break;
      default: break;
    }
  }

  // The arcade button is wired to a pull-up: pressed reads LOW.
  hostButtonLevel = keyHeld ? LOW : HIGH;

  // Real time, so the game's own dt-based motion runs at true speed.
  hostMillis = SDL_GetTicks() - t0;

  loop();

  SDL_UpdateTexture(tex, nullptr, tft->pixels(), PANEL_W * (int)sizeof(uint16_t));
  SDL_RenderClear(ren);
  SDL_RenderCopy(ren, tex, nullptr, nullptr);
  SDL_RenderPresent(ren);
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  // Nearest-neighbour: the art is 2x2 pixel blocks and must stay crisp.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  win = SDL_CreateWindow("SKOKAN", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         PANEL_W * scale, PANEL_H * scale, SDL_WINDOW_ALLOW_HIGHDPI);
  ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                          PANEL_W, PANEL_H);
  if (!win || !ren || !tex) {
    fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
    return 1;
  }

  printf("SKOKAN -- SPACE to jump (hold for height, press again in mid-air for\n"
         "a double jump).  1/2/3 hand over to a bot, 0 takes control back.\n");

  setup();
  t0 = SDL_GetTicks();

#ifdef __EMSCRIPTEN__
  // The browser owns the frame loop; returning from main keeps the runtime up.
  emscripten_set_main_loop(frame, 0, 1);
#else
  while (running) frame();
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
#endif
  return 0;
}
