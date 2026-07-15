/*
 * it_screen_sdl.c
 * ---------------
 * Pixel-accurate SDL2 presentation backend for it_screen.c -- the POSIX
 * counterpart of it_screen_win32.c. It shows the 80x50 cell buffer
 * rendered at 640x400 with the real VGA 8x8 glyph bitmaps and the IT
 * Camouflage palette (it_vgadata.c), integer-scaled into a window, using
 * the shared, backend-neutral Screen_Rasterize. This gives Linux/macOS
 * the authentic IT VGA look the Win32 backend already gives Windows.
 *
 * Compiled only when the build defines HAVE_SDL (CMake option ITED_SDL +
 * a found SDL2). Behaviour mirrors the Win32 backend exactly: same ITK_*
 * key codes, ITK_QUIT on window close, and cell + logical-pixel mouse
 * reporting.
 *
 * Single-threaded by design: the editor's main loop calls Key_Get and
 * Screen_Update regularly, which pump the SDL event queue. Only
 * SDL_INIT_VIDEO is requested -- audio stays on miniaudio's own thread.
 */

#ifdef HAVE_SDL

#include <SDL.h>
#include "it_screen.h"

#define PIX_W 640
#define PIX_H 400
#define SCALE 2

static SDL_Window   *Wnd;
static SDL_Renderer *Ren;
static SDL_Texture  *Tex;
static uint32_t      Pixels[PIX_W * PIX_H];
static int           KeyQueue[64];
static int           KeyHead, KeyTail;
static int           WantQuit;
static int           MousePX, MousePY;      /* logical pixels 0..639/0..399 */
static int           MouseB;

static void PushKey(int k)
{
    int next = (KeyTail + 1) % 64;
    if (next != KeyHead) {
        KeyQueue[KeyTail] = k;
        KeyTail = next;
    }
}

/* Non-text keys -> ITK_*. Printable characters arrive via SDL_TEXTINPUT
 * (the equivalent of Win32 WM_CHAR), so they are not mapped here. */
static int MapSDLKey(SDL_Keycode kc)
{
    switch (kc) {
    case SDLK_UP:        return ITK_UP;
    case SDLK_DOWN:      return ITK_DOWN;
    case SDLK_LEFT:      return ITK_LEFT;
    case SDLK_RIGHT:     return ITK_RIGHT;
    case SDLK_PAGEUP:    return ITK_PGUP;
    case SDLK_PAGEDOWN:  return ITK_PGDN;
    case SDLK_HOME:      return ITK_HOME;
    case SDLK_END:       return ITK_END;
    case SDLK_INSERT:    return ITK_INS;
    case SDLK_DELETE:    return ITK_DEL;
    case SDLK_ESCAPE:    return ITK_ESC;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return ITK_ENTER;
    case SDLK_BACKSPACE: return ITK_BACKSPACE;
    case SDLK_F1: case SDLK_F2: case SDLK_F3:  case SDLK_F4:
    case SDLK_F5: case SDLK_F6: case SDLK_F7:  case SDLK_F8:
    case SDLK_F9: case SDLK_F10: case SDLK_F11: case SDLK_F12:
        return ITK_F1 + (int)(kc - SDLK_F1);
    default:             return ITK_NONE;
    }
}

/* Convert window-space coords to logical 640x400 pixels. Uses the SDL
 * helper where available (2.0.18+, correct under HiDPI); otherwise scales
 * by the renderer's logical-size ratio. */
static void WinToLogical(int wx, int wy, int *lx, int *ly)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    float fx, fy;
    SDL_RenderWindowToLogical(Ren, wx, wy, &fx, &fy);
    *lx = (int)fx;
    *ly = (int)fy;
#else
    int ww = 1, wh = 1;
    SDL_GetWindowSize(Wnd, &ww, &wh);
    *lx = (ww > 0) ? wx * PIX_W / ww : 0;
    *ly = (wh > 0) ? wy * PIX_H / wh : 0;
#endif
    if (*lx < 0) *lx = 0; else if (*lx >= PIX_W) *lx = PIX_W - 1;
    if (*ly < 0) *ly = 0; else if (*ly >= PIX_H) *ly = PIX_H - 1;
}

static void PumpEvents(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            WantQuit = 1;
            PushKey(ITK_QUIT);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                WantQuit = 1;
                PushKey(ITK_QUIT);
            }
            break;
        case SDL_KEYDOWN: {
            SDL_Keycode kc = e.key.keysym.sym;
            SDL_Keymod  mod = SDL_GetModState();
            if (kc == SDLK_LSHIFT || kc == SDLK_RSHIFT) {
                if (!e.key.repeat)
                    PushKey(ITK_SHIFT_PRESS);
                break;
            }
            if (kc == SDLK_TAB) {
                PushKey((mod & KMOD_SHIFT) ? ITK_SHIFT_TAB : ITK_TAB);
                break;
            }
            if (kc == SDLK_F9 && (mod & KMOD_SHIFT)) {
                PushKey(ITK_SHIFT_F9);
                break;
            }
            if (kc == SDLK_SCROLLLOCK) {
                PushKey(ITK_SCROLL_LOCK);
                break;
            }
            if (mod & KMOD_ALT) {           /* Alt combos (parity with
                                             * the Win32 backend) */
                if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) {
                    /* host concern, not an IT key: toggle fullscreen
                     * (logical size keeps the letterbox + mouse map) */
                    if (!e.key.repeat) {
                        Uint32 fs = SDL_GetWindowFlags(Wnd)
                                    & SDL_WINDOW_FULLSCREEN_DESKTOP;
                        SDL_SetWindowFullscreen(Wnd,
                            fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                    break;
                }
                if (kc >= SDLK_a && kc <= SDLK_z) {
                    PushKey(ITK_ALT_A + (int)(kc - SDLK_a));
                    break;
                }
                if (kc >= SDLK_0 && kc <= SDLK_9) {
                    PushKey(ITK_ALT_0 + (int)(kc - SDLK_0));
                    break;
                }
                switch (kc) {
                case SDLK_INSERT: PushKey(ITK_ALT_INS);   break;
                case SDLK_DELETE: PushKey(ITK_ALT_DEL);   break;
                case SDLK_UP:     PushKey(ITK_ALT_UP);    break;
                case SDLK_DOWN:   PushKey(ITK_ALT_DOWN);  break;
                case SDLK_F9:     PushKey(ITK_ALT_F9);    break;
                case SDLK_F10:    PushKey(ITK_ALT_F10);   break;
                case SDLK_F12:    PushKey(ITK_ALT_F12);   break;
                case SDLK_BACKSLASH:
                    PushKey(ITK_ALT_BACKSLASH); break;
                case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS:
                    PushKey(ITK_ALT_PLUS);  break;
                case SDLK_MINUS: case SDLK_KP_MINUS:
                    PushKey(ITK_ALT_MINUS); break;
                default: break;
                }
                break;
            }
            if (mod & KMOD_CTRL) {
                int shifted = (mod & KMOD_SHIFT) != 0;
                if (kc == SDLK_q) {
                    PushKey(0x11);          /* Ctrl-Q, as Win32 WM_CHAR */
                    break;
                }
                if (kc == SDLK_PLUS || kc == SDLK_EQUALS ||
                    kc == SDLK_KP_PLUS) {
                    PushKey(ITK_CTRL_PLUS);
                    break;
                }
                if (kc == SDLK_MINUS || kc == SDLK_KP_MINUS) {
                    PushKey(ITK_CTRL_MINUS);
                    break;
                }
                if (shifted && kc >= SDLK_1 && kc <= SDLK_4) {
                    PushKey(ITK_CTRL_SHIFT_1 + (int)(kc - SDLK_1));
                    break;
                }
                if (!shifted && kc >= SDLK_0 && kc <= SDLK_5) {
                    PushKey(ITK_CTRL_0 + (int)(kc - SDLK_0));
                    break;
                }
                switch (kc) {
                case SDLK_UP:       PushKey(ITK_CTRL_UP);        break;
                case SDLK_DOWN:     PushKey(ITK_CTRL_DOWN);      break;
                case SDLK_LEFT:     PushKey(ITK_CTRL_LEFT);      break;
                case SDLK_RIGHT:    PushKey(ITK_CTRL_RIGHT);     break;
                case SDLK_HOME:     PushKey(ITK_CTRL_HOME);      break;
                case SDLK_END:      PushKey(ITK_CTRL_END);       break;
                case SDLK_PAGEUP:   PushKey(ITK_CTRL_PGUP);      break;
                case SDLK_PAGEDOWN: PushKey(ITK_CTRL_PGDN);      break;
                case SDLK_INSERT:   PushKey(ITK_CTRL_INS);       break;
                case SDLK_DELETE:   PushKey(ITK_CTRL_DEL);       break;
                case SDLK_BACKSPACE:PushKey(ITK_CTRL_BACKSPACE); break;
                case SDLK_F7:       PushKey(ITK_CTRL_F7);        break;
                case SDLK_F2:       PushKey(ITK_CTRL_F2);        break;
                default:
                    if (kc >= SDLK_a && kc <= SDLK_z)
                        PushKey((int)(kc - SDLK_a) + 1); /* Ctrl-A..Z */
                    break;
                }
                break;
            }
            if (mod & KMOD_SHIFT) {
                switch (kc) {
                case SDLK_UP:       PushKey(ITK_SHIFT_UP);    break;
                case SDLK_DOWN:     PushKey(ITK_SHIFT_DOWN);  break;
                case SDLK_LEFT:     PushKey(ITK_SHIFT_LEFT);  break;
                case SDLK_RIGHT:    PushKey(ITK_SHIFT_RIGHT); break;
                case SDLK_PAGEUP:   PushKey(ITK_SHIFT_PGUP);  break;
                case SDLK_PAGEDOWN: PushKey(ITK_SHIFT_PGDN);  break;
                case SDLK_HOME:     PushKey(ITK_SHIFT_HOME);  break;
                case SDLK_END:      PushKey(ITK_SHIFT_END);   break;
                default: {
                    int k = MapSDLKey(kc);
                    if (k != ITK_NONE)
                        PushKey(k);
                    break;              /* else SDL_TEXTINPUT */
                }
                }
                break;
            }
            if (kc == SDLK_KP_DIVIDE) { /* keypad '/', scan 135h */
                PushKey(ITK_KP_DIVIDE);
                break;
            }
            {
                int k = MapSDLKey(kc);
                if (k != ITK_NONE)
                    PushKey(k);
                /* else: printable -> delivered via SDL_TEXTINPUT */
            }
            break;
        }
        case SDL_KEYUP:
            if (e.key.keysym.sym == SDLK_LSHIFT ||
                e.key.keysym.sym == SDLK_RSHIFT)
                PushKey(ITK_SHIFT_RELEASE);
            break;
        case SDL_TEXTINPUT: {
            const char *p = e.text.text;
            for (; *p; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c >= 32 && c < 127)
                    PushKey((int)c);
            }
            break;
        }
        case SDL_MOUSEMOTION:
            WinToLogical(e.motion.x, e.motion.y, &MousePX, &MousePY);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                WinToLogical(e.button.x, e.button.y, &MousePX, &MousePY);
                MouseB = 1;
                PushKey(ITK_MOUSE);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT)
                MouseB = 0;
            break;
        }
    }
}

static int SDL_BInit(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return 0;                           /* no display -> caller falls back */

    /* SDL can fall back to a headless "offscreen"/"dummy" video driver when
     * no real display is reachable (e.g. bare SSH). That makes SDL_Init
     * succeed even though no window will ever be visible, which would wrongly
     * select this backend instead of the terminal. Treat those as "no
     * display" so the caller falls back to the terminal backend. */
    {
        const char *drv = SDL_GetCurrentVideoDriver();
        if (drv && (SDL_strcasecmp(drv, "offscreen") == 0 ||
                    SDL_strcasecmp(drv, "dummy") == 0)) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return 0;
        }
    }

    Wnd = SDL_CreateWindow("Impulse Tracker",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           PIX_W * SCALE, PIX_H * SCALE,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!Wnd)
        goto fail;

    Ren = SDL_CreateRenderer(Wnd, -1, SDL_RENDERER_ACCELERATED);
    if (!Ren)
        Ren = SDL_CreateRenderer(Wnd, -1, 0);   /* software fallback */
    if (!Ren)
        goto fail;

    /* Letterboxed integer scaling; also maps mouse coords to 640x400. */
    SDL_RenderSetLogicalSize(Ren, PIX_W, PIX_H);

    /* Rasterizer output is 0x00RRGGBB; ARGB8888 is a packed format so the
     * uint32 layout matches on any endianness, and the streaming texture's
     * default blend mode (NONE) ignores the zero alpha. No conversion. */
    Tex = SDL_CreateTexture(Ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, PIX_W, PIX_H);
    if (!Tex)
        goto fail;

    SDL_StartTextInput();
    return 1;

fail:
    if (Ren) { SDL_DestroyRenderer(Ren); Ren = NULL; }
    if (Wnd) { SDL_DestroyWindow(Wnd);   Wnd = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return 0;
}

static void SDL_BUnInit(void)
{
    if (Tex) { SDL_DestroyTexture(Tex);  Tex = NULL; }
    if (Ren) { SDL_DestroyRenderer(Ren); Ren = NULL; }
    if (Wnd) { SDL_DestroyWindow(Wnd);   Wnd = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void SDL_BPresent(const screen_cell_t *cells)
{
    (void)cells;                            /* rasterizer reads the shared buffer */

    Screen_Rasterize(Pixels);
    SDL_UpdateTexture(Tex, NULL, Pixels, PIX_W * (int)sizeof(uint32_t));
    SDL_RenderClear(Ren);
    SDL_RenderCopy(Ren, Tex, NULL, NULL);
    SDL_RenderPresent(Ren);
    PumpEvents();
}

static int SDL_BKey(void)
{
    PumpEvents();
    if (WantQuit)
        return ITK_QUIT;
    if (KeyHead == KeyTail)
        return ITK_NONE;
    {
        int k = KeyQueue[KeyHead];
        KeyHead = (KeyHead + 1) % 64;
        return k;
    }
}

static void SDL_BMouse(it_mouse_t *m)
{
    PumpEvents();
    m->px = MousePX;
    m->py = MousePY;
    m->x = m->px / 8;
    m->y = m->py / 8;
    m->b = MouseB;
}

const screen_backend_t Screen_BackendSDL = {
    SDL_BInit, SDL_BUnInit, SDL_BPresent, SDL_BKey, SDL_BMouse
};

#endif /* HAVE_SDL */
