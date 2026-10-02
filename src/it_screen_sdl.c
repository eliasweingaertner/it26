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
static it_key_t      KeyQueue[64];
/* feature 014: physical half of the current event, captured on
 * SDL_KEYDOWN and still valid for the SDL_TEXTINPUT that follows. */
static uint8_t       CurScan, CurFlags;
static int           KeyHead, KeyTail;
static int           WantQuit;
static int           SkipText;       /* TEXTINPUT duplicating a pushed key */
static int           MousePX, MousePY;      /* logical pixels 0..639/0..399 */
static int           MouseB;

/* SDL reports USB HID usage ids; the ported ASM tables are all indexed
 * by PC set-1 scancodes (research R6), so translate. Only the keys the
 * tables reference need to be here; anything else yields 0 = "position
 * unknown", which no consumer requires. */
static uint8_t SDLScanToSet1(SDL_Scancode s)
{
    switch (s) {
    case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_B: return 0x30;
    case SDL_SCANCODE_C: return 0x2E; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_E: return 0x12; case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_J: return 0x24;
    case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_O: return 0x18; case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_T: return 0x14;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_V: return 0x2F;
    case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_X: return 0x2D;
    case SDL_SCANCODE_Y: return 0x15; case SDL_SCANCODE_Z: return 0x2C;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0A; case SDL_SCANCODE_0: return 0x0B;
    case SDL_SCANCODE_MINUS:        return 0x0C;
    case SDL_SCANCODE_EQUALS:       return 0x0D;
    case SDL_SCANCODE_LEFTBRACKET:  return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_BACKSLASH:    return 0x2B;
    case SDL_SCANCODE_SEMICOLON:    return 0x27;
    case SDL_SCANCODE_APOSTROPHE:   return 0x28;
    case SDL_SCANCODE_GRAVE:        return 0x29;
    case SDL_SCANCODE_COMMA:        return 0x33;
    case SDL_SCANCODE_PERIOD:       return 0x34;
    case SDL_SCANCODE_SLASH:        return 0x35;
    case SDL_SCANCODE_SPACE:        return 0x39;
    case SDL_SCANCODE_RETURN:       return 0x1C;
    case SDL_SCANCODE_ESCAPE:       return 0x01;
    case SDL_SCANCODE_BACKSPACE:    return 0x0E;
    case SDL_SCANCODE_TAB:          return 0x0F;
    case SDL_SCANCODE_CAPSLOCK:     return 0x3A;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
    /* E0-extended: the original encodes these as +80h (IT_K.ASM:1156) */
    case SDL_SCANCODE_KP_DIVIDE:    return 0xB5;
    case SDL_SCANCODE_LCTRL:        return 0x1D;
    case SDL_SCANCODE_LSHIFT:       return 0x2A;
    case SDL_SCANCODE_LALT:         return 0x38;
    case SDL_SCANCODE_RCTRL:        return 0x9D;
    case SDL_SCANCODE_RSHIFT:       return 0x36;
    case SDL_SCANCODE_RALT:         return 0xB8;
    default: return 0;
    }
}

/* macOS: Right Option alone, held in the pattern editor, is the note
 * preview key -- IT's held Caps Lock (PE_PatternCursorPreview), since on
 * a Mac Caps Lock is a toggle and macOS pops up its indicator for it
 * (issue #20). Left Option stays Alt for all of IT's shortcuts. */
#if defined(__APPLE__) && !defined(ITED_RIGHTOPT_PREVIEW)
#define ITED_RIGHTOPT_PREVIEW 1
#endif
#ifdef ITED_RIGHTOPT_PREVIEW
static int RightOptPreview(SDL_Keymod m)
{
    return Screen_RightOptPreview && (m & KMOD_RALT) && !(m & KMOD_LALT) &&
           !(m & (KMOD_CTRL | KMOD_GUI));
}
#endif

/* the original's CH (IT_K.ASM:1216) from SDL's live modifier state */
static uint8_t ModFlags(void)
{
    SDL_Keymod m = SDL_GetModState();
    uint8_t f = ITKF_PRESSED;
    if (m & KMOD_LSHIFT) f |= ITKF_LSHIFT;
    if (m & KMOD_RSHIFT) f |= ITKF_RSHIFT;
    if (m & KMOD_LCTRL)  f |= ITKF_LCTRL;
    if (m & KMOD_RCTRL)  f |= ITKF_RCTRL;
#ifdef ITED_RIGHTOPT_PREVIEW
    if (RightOptPreview(m))             /* preview held, not Alt */
        return (uint8_t)(f | ITKF_CAPSDOWN);
#endif
    if (m & KMOD_LALT)   f |= ITKF_LALT;
    if (m & KMOD_RALT)   f |= ITKF_RALT;
    if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_CAPSLOCK])
        f |= ITKF_CAPSDOWN;
    return f;
}

static void PushKeyCh(int k, uint16_t ch)
{
    int next = (KeyTail + 1) % 64;
    if (next != KeyHead) {
        KeyQueue[KeyTail].scan  = CurScan;
        KeyQueue[KeyTail].flags = CurFlags;
        KeyQueue[KeyTail].ch    = ch;
        KeyQueue[KeyTail].code  = k;
        KeyTail = next;
    }
}

static void PushKey(int k)
{
    PushKeyCh(k, 0);
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

/* Mouse event coordinates -> logical 640x400 pixels (issue #8).
 * With SDL_RenderSetLogicalSize in effect, SDL2's renderer already
 * rewrites every mouse event into logical coordinates -- Retina scale
 * and letterbox included -- before we see it. The event values are
 * used as they are, only clamped. (Until 2026-09 the port converted
 * them a second time, as if they were window points, which pulled
 * every click towards the top-left: F4 button clicks landed in the
 * instrument list, slider clicks missed.) */
static void EventToLogical(int ex, int ey, int *lx, int *ly)
{
    *lx = ex < 0 ? 0 : ex >= PIX_W ? PIX_W - 1 : ex;
    *ly = ey < 0 ? 0 : ey >= PIX_H ? PIX_H - 1 : ey;
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
            CurScan  = SDLScanToSet1(e.key.keysym.scancode);
            CurFlags = ModFlags();
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
            if ((kc == SDLK_F9 || kc == SDLK_F10) && (mod & KMOD_SHIFT) &&
                (mod & KMOD_CTRL) && !(mod & KMOD_ALT)) {
                /* feature 016: system dialogs; ahead of Shift-F9 */
                PushKey(kc == SDLK_F9 ? ITK_CTRL_SHIFT_F9 : ITK_CTRL_SHIFT_F10);
                break;
            }
            if (kc == SDLK_F9 && (mod & KMOD_SHIFT)) {
                PushKey(ITK_SHIFT_F9);
                break;
            }
            if (kc == SDLK_F6 && (mod & KMOD_SHIFT)) {
                PushKey(ITK_SHIFT_F6);      /* Glbl_Shift_F6 */
                break;
            }
            if (kc == SDLK_F5 && (mod & KMOD_SHIFT)) {
                PushKey(ITK_SHIFT_F5);      /* Glbl_DriverScreen */
                break;
            }
            if (kc == SDLK_SCROLLLOCK) {
                PushKey(ITK_SCROLL_LOCK);
                break;
            }
#ifdef ITED_RIGHTOPT_PREVIEW
            if (RightOptPreview(mod)) {
                /* the plain key, flagged "preview held" by ModFlags; the
                 * Option character (often outside CP437, e.g. oe) is
                 * dropped so the note key always arrives */
                if ((kc >= SDLK_a && kc <= SDLK_z) ||
                    (kc >= SDLK_0 && kc <= SDLK_9)) {
                    PushKeyCh((int)kc, 0);
                    SkipText = 1;
                    break;
                }
                mod = (SDL_Keymod)(mod & ~KMOD_RALT);   /* other keys:
                                                           as without it */
            }
#endif
            if (mod & KMOD_ALT) {           /* Alt combos (parity with
                                             * the Win32 backend) */
                if (kc == SDLK_RETURN || kc == SDLK_KP_ENTER) {
                    /* host concern -- toggle fullscreen (logical size
                     * keeps the letterbox + mouse map) -- except in the
                     * pattern editor, where it stores the pattern */
                    if (Screen_AltEnterIsKey)
                        PushKey(ITK_ALT_ENTER);
                    else if (!e.key.repeat) {
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
                if (kc >= SDLK_F1 && kc <= SDLK_F8) {   /* Glbl_Alt_F1.. */
                    PushKey(ITK_ALT_F1 + (int)(kc - SDLK_F1));
                    break;
                }
                switch (kc) {
                case SDLK_LEFT:   PushKey(ITK_ALT_LEFT);  break;
                case SDLK_RIGHT:  PushKey(ITK_ALT_RIGHT); break;
                case SDLK_HOME:   PushKey(ITK_ALT_HOME);  break;
                case SDLK_END:    PushKey(ITK_ALT_END);   break;
                case SDLK_BACKSPACE: PushKey(ITK_ALT_BACKSPACE); break;
                case SDLK_F11:    PushKey(ITK_ALT_F11);   break;
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
                case SDLK_F1:       PushKey(ITK_CTRL_F1);        break;
                case SDLK_F3:       PushKey(ITK_CTRL_F3);        break;
                case SDLK_F7:       PushKey(ITK_CTRL_F7);        break;
                case SDLK_F4:       PushKey(ITK_CTRL_F4);        break;
                case SDLK_F5:       PushKey(ITK_CTRL_F5);        break;
                case SDLK_F6:       PushKey(ITK_CTRL_F6);        break;
                case SDLK_RETURN: case SDLK_KP_ENTER:   /* 111Ch */
                    PushKey((mod & KMOD_RCTRL) ? ITK_RCTRL_ENTER
                                               : ITK_ENTER);
                    break;
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
                case SDLK_KP_PLUS:  /* grey +/-: Next/Last4Patterns */
                    PushKey(ITK_SHIFT_PLUS);  SkipText = 1; break;
                case SDLK_KP_MINUS:
                    PushKey(ITK_SHIFT_MINUS); SkipText = 1; break;
                default: {
                    int k = MapSDLKey(kc);
                    if (k != ITK_NONE)
                        PushKey(k);
                    break;              /* else SDL_TEXTINPUT */
                }
                }
                break;
            }
            if (kc == SDLK_KP_DIVIDE) { /* keypad '/', scan 1B5h */
                PushKey(ITK_KP_DIVIDE);
                SkipText = 1;           /* not also a '/' character */
                break;
            }
            if (kc == SDLK_KP_MULTIPLY) {   /* keypad '*', scan 137h */
                PushKey(ITK_KP_MULTIPLY);
                SkipText = 1;
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
            /* UTF-8 -> code point -> CP437. The legacy `code` keeps the
             * old ASCII-only filter so this phase changes no behaviour;
             * `ch` carries the national characters (feature 014). */
            const unsigned char *p = (const unsigned char *)e.text.text;
            if (SkipText) {             /* duplicate of a pushed key */
                SkipText = 0;
                break;
            }
            while (*p) {
                uint32_t u;
                int extra;
                if (*p < 0x80)            { u = *p;         extra = 0; }
                else if ((*p & 0xE0) == 0xC0) { u = *p & 0x1F; extra = 1; }
                else if ((*p & 0xF0) == 0xE0) { u = *p & 0x0F; extra = 2; }
                else if ((*p & 0xF8) == 0xF0) { u = *p & 0x07; extra = 3; }
                else                      { p++; continue; }
                p++;
                while (extra-- > 0 && (*p & 0xC0) == 0x80)
                    u = (u << 6) | (*p++ & 0x3F);
                {
                    uint16_t cp = Screen_UnicodeToCP437(u);
                    if (u >= 32 && u < 127)
                        PushKeyCh((int)u, cp);
                    else if (u >= 127 && cp >= 32)
                        PushKeyCh((int)cp, cp);   /* national chars */
                }
            }
            break;
        }
        case SDL_MOUSEMOTION:
            EventToLogical(e.motion.x, e.motion.y, &MousePX, &MousePY);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) {
                EventToLogical(e.button.x, e.button.y, &MousePX, &MousePY);
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

#ifdef __APPLE__
/* Issue #15: macOS answers a held letter key with the press-and-hold
 * accent popup instead of key repeat, so a held note key entered one
 * note. The switch is ApplePressAndHoldEnabled. Classic SDL2 registers
 * it NO itself, but SDL3 (>= 3.2) registers YES -- and Homebrew's "sdl2"
 * is sdl2-compat running on SDL3. SDL 3.4 takes the choice from the
 * SDL_MAC_PRESS_AND_HOLD hint (set before SDL_Init, below); for SDL 3.2
 * we register NO again after SDL_Init, since the last registration wins.
 * The registration domain lives in this process only -- nothing is
 * written to the user's preferences. Held keys then repeat at the
 * system rate and delay, as under DOS typematic repeat. */
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>

static void DisablePressAndHold(void)
{
    const void *k = CFSTR("ApplePressAndHoldEnabled");
    const void *v = kCFBooleanFalse;
    CFDictionaryRef d;
    id defs;
    Class ud = objc_getClass("NSUserDefaults");

    if (!ud)
        return;
    defs = ((id (*)(id, SEL))objc_msgSend)((id)ud,
                                           sel_registerName("standardUserDefaults"));
    if (!defs)
        return;
    d = CFDictionaryCreate(NULL, &k, &v, 1, &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    if (!d)
        return;
    ((void (*)(id, SEL, id))objc_msgSend)(defs, sel_registerName("registerDefaults:"),
                                          (id)d);   /* toll-free NSDictionary */
    CFRelease(d);
}
#endif

static int SDL_BInit(void)
{
#ifdef __APPLE__
    SDL_SetHint("SDL_MAC_PRESS_AND_HOLD", "0");     /* SDL 3.4+ (via compat) */
#endif
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return 0;                           /* no display -> caller falls back */
#ifdef __APPLE__
    DisablePressAndHold();                  /* after SDL's own registration */
#endif

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

    Wnd = SDL_CreateWindow("Impulse Tracker - 2026 AI port by Elias W. / original by Jeffrey Lim",
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

    /* Letterboxed scaling; SDL also rewrites mouse event coordinates
     * into this 640x400 space (see EventToLogical). */
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
        int k = KeyQueue[KeyHead].code;
        KeyHead = (KeyHead + 1) % 64;
        return k;
    }
}

/* feature 014: the same queue, with the physical half attached */
static int SDL_BKeyEvent(it_key_t *k)
{
    PumpEvents();
    if (WantQuit) {
        k->scan = 0;
        k->flags = ITKF_PRESSED;
        k->ch = 0;
        k->code = ITK_QUIT;
        return 1;
    }
    if (KeyHead == KeyTail)
        return 0;
    *k = KeyQueue[KeyHead];
    KeyHead = (KeyHead + 1) % 64;
    return 1;
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

/* ---- feature 016: system file dialogs (it_dialog_mac.c /
 * it_dialog_posix.c) ---- */
#ifdef __APPLE__
int Dialog_Mac(const it_dialog_req_t *req, it_dialog_res_t *res);
#else
int Dialog_Posix(const it_dialog_req_t *req, it_dialog_res_t *res,
                 void (*idle)(void));

/* while the helper process runs: keep the window responsive (the
 * window manager would flag it as hung) by pumping events and showing
 * the last frame again; input is dropped afterwards */
static void DialogIdle(void)
{
    SDL_PumpEvents();
    if (Ren && Tex) {
        SDL_RenderClear(Ren);
        SDL_RenderCopy(Ren, Tex, NULL, NULL);
        SDL_RenderPresent(Ren);
    }
}
#endif

static int SDL_BFileDialog(const it_dialog_req_t *req, it_dialog_res_t *res)
{
    int r;
#ifdef __APPLE__
    r = Dialog_Mac(req, res);
#else
    /* the helper is a separate top-level window a fullscreen window
     * would cover: leave fullscreen for the dialog, return afterwards */
    Uint32 fs = Wnd ? (SDL_GetWindowFlags(Wnd) & SDL_WINDOW_FULLSCREEN_DESKTOP)
                    : 0;
    if (fs)
        SDL_SetWindowFullscreen(Wnd, 0);
    r = Dialog_Posix(req, res, DialogIdle);
    if (fs)
        SDL_SetWindowFullscreen(Wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif
    /* nothing typed into the dialog reaches the tracker, no modifier
     * stays held (key-up events went to the dialog), and a Shift chord /
     * marking in the pattern editor is closed */
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT);
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_ResetKeyboard();
#endif
    SDL_SetModState(KMOD_NONE);
    KeyHead = KeyTail = 0;
    SkipText = 0;
    CurScan = 0;
    CurFlags = ITKF_PRESSED;
    PushKey(ITK_SHIFT_RELEASE);
    if (Wnd)
        SDL_RaiseWindow(Wnd);
    return r;
}

const screen_backend_t Screen_BackendSDL = {
    SDL_BInit, SDL_BUnInit, SDL_BPresent, SDL_BKey, SDL_BMouse,
    SDL_BKeyEvent, SDL_BFileDialog,
#ifdef __APPLE__
    "Right Option"      /* the held note preview (issue #20), not Caps Lock */
#else
    NULL
#endif
};

#endif /* HAVE_SDL */
