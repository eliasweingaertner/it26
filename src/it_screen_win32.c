/*
 * it_screen_win32.c
 * -----------------
 * Pixel-accurate Win32 presentation backend for it_screen.c: a window
 * showing the 80x50 cell buffer rendered at 640x400 with the real VGA
 * 8x8 glyph bitmaps and the IT Camouflage palette (it_vgadata.c),
 * integer-scaled. This reproduces what IT's 80x50 text mode looked
 * like on a VGA monitor.
 *
 * Single-threaded by design: the editor's main loop calls Key_Get and
 * Screen_Update regularly, which pump the message queue.
 */

#ifdef _WIN32

#include <windows.h>
#include "it_screen.h"

#define PIX_W 640
#define PIX_H 400
#define SCALE 2
#define WIN_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

static HWND      Wnd;
static uint32_t  Pixels[PIX_W * PIX_H];
static BITMAPINFO Bmi;
static it_key_t  KeyQueue[64];
static int       KeyHead, KeyTail;
/* feature 014: the physical half of the event. Set on every
 * WM_KEYDOWN/WM_SYSKEYDOWN and still valid for the WM_CHAR that
 * TranslateMessage synthesizes immediately afterwards, which is how the
 * scancode and the character get paired. */
static uint8_t   CurScan, CurFlags;
static int       WantQuit;
static int       MousePX, MousePY;      /* logical pixels 0..639/0..399 */
static int       MouseB;
static int       SkipChar;              /* WM_CHAR that duplicates a key
                                           already pushed from WM_KEYDOWN */
static int       FullScr;               /* Alt-Enter borderless fullscreen */
static WINDOWPLACEMENT SavedPlacement;
static int       DstX, DstY;            /* letterboxed blit rect (for the */
static int       DstW = PIX_W * SCALE;  /* window -> logical mouse map)   */
static int       DstH = PIX_H * SCALE;
static HDC       BackDC;                /* double buffer: compose offscreen, */
static HBITMAP   BackBmp, BackBmpOld;   /* present with one BitBlt (direct   */
static int       BackW, BackH;          /* StretchDIBits to the screen shows
                                           mid-blit states = flicker)        */

/* the original's CH, rebuilt from the live key state (IT_K.ASM:1216) */
static uint8_t ModFlags(void)
{
    uint8_t f = ITKF_PRESSED;
    if (GetKeyState(VK_LSHIFT)   & 0x8000) f |= ITKF_LSHIFT;
    if (GetKeyState(VK_RSHIFT)   & 0x8000) f |= ITKF_RSHIFT;
    if (GetKeyState(VK_LCONTROL) & 0x8000) f |= ITKF_LCTRL;
    if (GetKeyState(VK_RCONTROL) & 0x8000) f |= ITKF_RCTRL;
    if (GetKeyState(VK_LMENU)    & 0x8000) f |= ITKF_LALT;
    if (GetKeyState(VK_RMENU)    & 0x8000) f |= ITKF_RALT;
    if (GetKeyState(VK_CAPITAL)  & 0x8000) f |= ITKF_CAPSDOWN;
    return f;
}

/* lParam bits 16-23 are the OEM set-1 scancode; bit 24 marks the
 * E0-extended variant, which the original encodes as +80h. */
static void CaptureScan(LPARAM lp)
{
    CurScan  = (uint8_t)((lp >> 16) & 0xFF);
    if (lp & (1L << 24))
        CurScan |= 0x80;
    CurFlags = ModFlags();
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

static int MapVKey(WPARAM vk)
{
    switch (vk) {
    case VK_UP:     return ITK_UP;
    case VK_DOWN:   return ITK_DOWN;
    case VK_LEFT:   return ITK_LEFT;
    case VK_RIGHT:  return ITK_RIGHT;
    case VK_PRIOR:  return ITK_PGUP;
    case VK_NEXT:   return ITK_PGDN;
    case VK_HOME:   return ITK_HOME;
    case VK_END:    return ITK_END;
    case VK_INSERT: return ITK_INS;
    case VK_DELETE: return ITK_DEL;
    case VK_ESCAPE: return ITK_ESC;
    case VK_RETURN: return ITK_ENTER;
    case VK_BACK:   return ITK_BACKSPACE;
    case VK_F1: case VK_F2: case VK_F3: case VK_F4:  case VK_F5:  case VK_F6:
    case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        return ITK_F1 + (int)(vk - VK_F1);
    default:        return ITK_NONE;
    }
}

static void FreeBackBuffer(void)
{
    if (BackDC) {
        SelectObject(BackDC, BackBmpOld);
        DeleteObject(BackBmp);
        DeleteDC(BackDC);
        BackDC = NULL;
        BackW = BackH = 0;
    }
}

static void PaintWindow(HDC dc)
{
    HDC out;
    RECT rc;
    int cw, ch;

    GetClientRect(Wnd, &rc);
    cw = rc.right;
    ch = rc.bottom;
    if (cw <= 0 || ch <= 0)
        return;

    /* largest 640:400 rect that fits, centred; black bars around it
     * (only differs from the full client rect in fullscreen) */
    if (cw * PIX_H >= ch * PIX_W) {
        DstH = ch;
        DstW = ch * PIX_W / PIX_H;
    } else {
        DstW = cw;
        DstH = cw * PIX_H / PIX_W;
    }
    DstX = (cw - DstW) / 2;
    DstY = (ch - DstH) / 2;

    /* compose into the client-sized back buffer, then present with a
     * single BitBlt.  Painting the visible surface directly flickers:
     * the screen samples the window mid-draw, showing the black fill
     * or a half-finished stretch (font shimmer at non-integer scale). */
    if (BackW != cw || BackH != ch) {
        FreeBackBuffer();
        BackDC = CreateCompatibleDC(dc);
        BackBmp = CreateCompatibleBitmap(dc, cw, ch);
        if (BackDC && BackBmp) {
            BackBmpOld = SelectObject(BackDC, BackBmp);
            BackW = cw;
            BackH = ch;
        } else {                        /* out of GDI resources: draw
                                           direct rather than nothing */
            if (BackBmp)
                DeleteObject(BackBmp);
            if (BackDC)
                DeleteDC(BackDC);
            BackDC = NULL;
        }
    }
    out = BackDC ? BackDC : dc;

    if (DstX || DstY)
        FillRect(out, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetStretchBltMode(out, COLORONCOLOR);
    StretchDIBits(out, DstX, DstY, DstW, DstH,
                  0, 0, PIX_W, PIX_H,
                  Pixels, &Bmi, DIB_RGB_COLORS, SRCCOPY);
    if (out == BackDC)
        BitBlt(dc, 0, 0, cw, ch, BackDC, 0, 0, SRCCOPY);
}

static void ToggleFullscreen(void)
{
    if (!FullScr) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        SavedPlacement.length = sizeof(SavedPlacement);
        GetWindowPlacement(Wnd, &SavedPlacement);
        /* entered by maximizing: leaving returns to the normal window */
        if (SavedPlacement.showCmd == SW_SHOWMAXIMIZED)
            SavedPlacement.showCmd = SW_SHOWNORMAL;
        GetMonitorInfoA(MonitorFromWindow(Wnd, MONITOR_DEFAULTTONEAREST),
                        &mi);
        SetWindowLongPtrA(Wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(Wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
        FullScr = 1;
    } else {
        SetWindowLongPtrA(Wnd, GWL_STYLE, WIN_STYLE | WS_VISIBLE);
        SetWindowPlacement(Wnd, &SavedPlacement);
        SetWindowPos(Wnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_FRAMECHANGED);
        FullScr = 0;
    }
    InvalidateRect(Wnd, NULL, FALSE);
}

static void MouseFromLParam(LPARAM lp)
{
    int wx = (int)(short)LOWORD(lp), wy = (int)(short)HIWORD(lp);
    MousePX = DstW > 0 ? (wx - DstX) * PIX_W / DstW : 0;
    MousePY = DstH > 0 ? (wy - DstY) * PIX_H / DstH : 0;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        WantQuit = 1;
        PushKey(ITK_QUIT);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintWindow(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        CaptureScan(lp);                /* feature 014: physical half */
        if (wp == VK_SHIFT) {           /* shift press event (2Ah/36h) */
            if (!(lp & (1u << 30)))     /* suppress autorepeat */
                PushKey(ITK_SHIFT_PRESS);
            return 0;
        }
        if (wp == VK_TAB) {
            PushKey((GetKeyState(VK_SHIFT) & 0x8000) ? ITK_SHIFT_TAB
                                                     : ITK_TAB);
            return 0;
        }
        if ((wp == VK_F9 || wp == VK_F10) &&
            (GetKeyState(VK_SHIFT) & 0x8000) &&
            (GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000)) {
            /* feature 016: system dialogs; ahead of Shift-F9 */
            PushKey(wp == VK_F9 ? ITK_CTRL_SHIFT_F9 : ITK_CTRL_SHIFT_F10);
            return 0;
        }
        if (wp == VK_F9 && (GetKeyState(VK_SHIFT) & 0x8000)) {
            PushKey(ITK_SHIFT_F9);      /* message editor */
            return 0;
        }
        if (wp == VK_F6 && (GetKeyState(VK_SHIFT) & 0x8000)) {
            PushKey(ITK_SHIFT_F6);      /* Glbl_Shift_F6 */
            return 0;
        }
        if (wp == VK_F5 && (GetKeyState(VK_SHIFT) & 0x8000)) {
            PushKey(ITK_SHIFT_F5);      /* Glbl_DriverScreen */
            return 0;
        }
        if (wp == VK_SCROLL) {
            PushKey(ITK_SCROLL_LOCK);
            return 0;
        }
        /* Alt combos (WM_SYSKEYDOWN with the menu key held) */
        if (GetKeyState(VK_MENU) & 0x8000) {
            if (wp == VK_RETURN) {      /* host concern, except in the
                                           pattern editor (store pattern) */
                if (Screen_AltEnterIsKey)
                    PushKey(ITK_ALT_ENTER);
                else if (!(lp & (1u << 30)))    /* suppress autorepeat */
                    ToggleFullscreen();
                return 0;
            }
            if (wp >= VK_F1 && wp <= VK_F8) {   /* Glbl_Alt_F1..F8 */
                PushKey(ITK_ALT_F1 + (int)(wp - VK_F1));
                return 0;
            }
            if (wp >= 'A' && wp <= 'Z') {
                PushKey(ITK_ALT_A + (int)(wp - 'A'));
                return 0;
            }
            if (wp >= '0' && wp <= '9') {
                PushKey(ITK_ALT_0 + (int)(wp - '0'));
                return 0;
            }
            switch (wp) {
            case VK_INSERT: PushKey(ITK_ALT_INS);  return 0;
            case VK_DELETE: PushKey(ITK_ALT_DEL);  return 0;
            case VK_UP:     PushKey(ITK_ALT_UP);   return 0;
            case VK_DOWN:   PushKey(ITK_ALT_DOWN); return 0;
            case VK_LEFT:   PushKey(ITK_ALT_LEFT);  return 0;
            case VK_RIGHT:  PushKey(ITK_ALT_RIGHT); return 0;
            case VK_HOME:   PushKey(ITK_ALT_HOME);  return 0;
            case VK_END:    PushKey(ITK_ALT_END);   return 0;
            case VK_BACK:   PushKey(ITK_ALT_BACKSPACE); return 0;
            case VK_F11:    PushKey(ITK_ALT_F11);  return 0;
            case VK_F9:     PushKey(ITK_ALT_F9);   return 0;
            case VK_F10:    PushKey(ITK_ALT_F10);  return 0;
            case VK_F12:    PushKey(ITK_ALT_F12);  return 0;
            case VK_OEM_5:  PushKey(ITK_ALT_BACKSLASH); return 0;
            case VK_OEM_PLUS: case VK_ADD:
                PushKey(ITK_ALT_PLUS);  return 0;
            case VK_OEM_MINUS: case VK_SUBTRACT:
                PushKey(ITK_ALT_MINUS); return 0;
            default: break;
            }
        }
        /* Ctrl combos (no WM_CHAR is generated for these) */
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            int shifted = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wp == VK_OEM_PLUS || wp == VK_ADD) {
                PushKey(ITK_CTRL_PLUS);
                return 0;
            }
            if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) {
                PushKey(ITK_CTRL_MINUS);
                return 0;
            }
            if (shifted && wp >= '1' && wp <= '4') {
                PushKey(ITK_CTRL_SHIFT_1 + (int)(wp - '1'));
                return 0;
            }
            if (!shifted && wp >= '0' && wp <= '5') {
                PushKey(ITK_CTRL_0 + (int)(wp - '0'));
                return 0;
            }
            switch (wp) {
            case VK_UP:     PushKey(ITK_CTRL_UP);        return 0;
            case VK_DOWN:   PushKey(ITK_CTRL_DOWN);      return 0;
            case VK_LEFT:   PushKey(ITK_CTRL_LEFT);      return 0;
            case VK_RIGHT:  PushKey(ITK_CTRL_RIGHT);     return 0;
            case VK_HOME:   PushKey(ITK_CTRL_HOME);      return 0;
            case VK_END:    PushKey(ITK_CTRL_END);       return 0;
            case VK_PRIOR:  PushKey(ITK_CTRL_PGUP);      return 0;
            case VK_NEXT:   PushKey(ITK_CTRL_PGDN);      return 0;
            case VK_INSERT: PushKey(ITK_CTRL_INS);       return 0;
            case VK_DELETE: PushKey(ITK_CTRL_DEL);       return 0;
            case VK_BACK:   PushKey(ITK_CTRL_BACKSPACE); return 0;
            case VK_F1:     PushKey(ITK_CTRL_F1);        return 0;
            case VK_F3:     PushKey(ITK_CTRL_F3);        return 0;
            case VK_F7:     PushKey(ITK_CTRL_F7);        return 0;
            case VK_F4:     PushKey(ITK_CTRL_F4);        return 0;
            case VK_F5:     PushKey(ITK_CTRL_F5);        return 0;
            case VK_F6:     PushKey(ITK_CTRL_F6);        return 0;
            case VK_RETURN:     /* Right-Ctrl+Enter = PE_ShowPatternLength;
                                   either way WM_CHAR follows with 0Ah,
                                   which would read as Ctrl-J */
                PushKey((GetKeyState(VK_RCONTROL) & 0x8000)
                        ? ITK_RCTRL_ENTER : ITK_ENTER);
                SkipChar = 1;
                return 0;
            case VK_F2:     PushKey(ITK_CTRL_F2);        return 0;
            case 'H':       PushKey(0x08);               return 0;
                            /* Ctrl-H (row hilight); WM_CHAR drops 08h */
            default: break;
            }
        }
        /* Shift combos (movement keys report distinct codes) */
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            switch (wp) {
            case VK_UP:     PushKey(ITK_SHIFT_UP);    return 0;
            case VK_DOWN:   PushKey(ITK_SHIFT_DOWN);  return 0;
            case VK_LEFT:   PushKey(ITK_SHIFT_LEFT);  return 0;
            case VK_RIGHT:  PushKey(ITK_SHIFT_RIGHT); return 0;
            case VK_PRIOR:  PushKey(ITK_SHIFT_PGUP);  return 0;
            case VK_NEXT:   PushKey(ITK_SHIFT_PGDN);  return 0;
            case VK_HOME:   PushKey(ITK_SHIFT_HOME);  return 0;
            case VK_END:    PushKey(ITK_SHIFT_END);   return 0;
            case VK_ADD:        /* grey + / - (14Eh/14Ah): Next/Last4
                                   Patterns; WM_CHAR would add '+'/'-' */
                PushKey(ITK_SHIFT_PLUS);  SkipChar = 1; return 0;
            case VK_SUBTRACT:
                PushKey(ITK_SHIFT_MINUS); SkipChar = 1; return 0;
            default: break;
            }
        }
        if (wp == VK_DIVIDE) {          /* keypad '/', scan 1B5h */
            PushKey(ITK_KP_DIVIDE);
            SkipChar = 1;               /* not also a '/' character */
            return 0;
        }
        if (wp == VK_MULTIPLY) {        /* keypad '*', scan 137h */
            PushKey(ITK_KP_MULTIPLY);
            SkipChar = 1;
            return 0;
        }
        {
            int k = MapVKey(wp);
            if (k != ITK_NONE) {
                PushKey(k);
                return 0;
            }
        }
        break;                          /* let WM_CHAR deliver ASCII */
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp == VK_SHIFT)
            PushKey(ITK_SHIFT_RELEASE);
        break;
    case WM_SYSCHAR:
        /* TranslateMessage turns every Alt+key into a WM_SYSCHAR after
         * the WM_SYSKEYDOWN we already handled. Left to DefWindowProc it
         * is treated as a menu mnemonic; there is no menu, so Windows
         * plays the "default beep" on every Alt shortcut. Swallow it --
         * except Alt+Space, which opens the system menu. */
        if (wp == ' ')
            break;
        return 0;
    case WM_SIZE:
        /* maximized some other way (Win+Up, snapping to the top edge):
         * go fullscreen once the size change has finished */
        if (wp == SIZE_MAXIMIZED && !FullScr)
            PostMessageA(h, WM_APP + 1, 0, 0);
        break;
    case WM_APP + 1:
        if (!FullScr)
            ToggleFullscreen();
        return 0;
    case WM_SYSCOMMAND:
        /* Maximizing (button, title-bar double click) means borderless
         * fullscreen, the same as Alt-Enter outside the pattern editor,
         * where Alt-Enter belongs to IT (store pattern). */
        if ((wp & 0xFFF0) == SC_MAXIMIZE) {
            if (!FullScr)
                ToggleFullscreen();
            return 0;
        }
        /* A lone Alt tap (or F10, IT's save key) sends SC_KEYMENU with
         * lParam 0, which puts the window into menu mode: the next key
         * is eaten and beeps. Refuse keyboard menu activation; mouse
         * and Alt+Space (lParam = ' ') still work. */
        if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ')
            return 0;
        break;
    case WM_CHAR:
        if (SkipChar) {                 /* duplicate of a pushed key */
            SkipChar = 0;
            return 0;
        }
        /* wp is a UTF-16 code unit; CP437 has nothing outside the BMP,
         * so an unpaired surrogate simply converts to 0 = reject. */
        if (wp >= 32 && wp < 127)
            PushKeyCh((int)wp, (uint16_t)wp);
        else if (wp >= 127) {
            /* National characters (feature 014). Only what CP437 can
             * represent gets through: a character with no CP437 code
             * produces NO event at all, so the field and cursor are
             * left untouched rather than taking a wrong glyph. */
            uint16_t cp = Screen_UnicodeToCP437((uint32_t)wp);
            if (cp >= 32)
                PushKeyCh((int)cp, cp);
        }
        else if (wp >= 1 && wp <= 26 &&
                 wp != 8 && wp != 9 && wp != 13)
            PushKeyCh((int)wp, 0);      /* Ctrl-A..Z (BS/Tab/CR are
                                           delivered as VK keys above) */
        return 0;
    case WM_MOUSEMOVE:
        MouseFromLParam(lp);
        return 0;
    case WM_LBUTTONDOWN:
        MouseFromLParam(lp);
        MouseB = 1;
        SetCapture(h);
        PushKey(ITK_MOUSE);
        return 0;
    case WM_LBUTTONUP:
        MouseB = 0;
        ReleaseCapture();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* Global hotkeys of other programs (the NVIDIA overlay's Alt+F1/F2/F3,
 * Alt+F9/F10, Alt+Z, Alt+R, ...) swallow keys IT needs before they
 * reach this window. While ittrack is in front, a low-level keyboard
 * hook takes left-Alt + F-key/letter/digit first -- the most recently
 * installed hook runs first -- and hands it to our own window as the
 * WM_SYSKEYDOWN it would have been. AltGr (right Alt, or Ctrl+Alt) is
 * left alone so national characters still type; Alt+Tab/Esc/Space are
 * not touched. */
static HHOOK KbHook;

static LRESULT CALLBACK KbHookProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && Wnd && GetForegroundWindow() == Wnd) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        DWORD vk = k->vkCode;
        int ours = (vk >= VK_F1 && vk <= VK_F12) ||
                   (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9');
        if (ours && (k->flags & LLKHF_ALTDOWN) &&
            (GetAsyncKeyState(VK_LMENU) & 0x8000) &&
            !(GetAsyncKeyState(VK_RMENU) & 0x8000) &&
            !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            int up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
            LPARAM l = 1 | ((LPARAM)(k->scanCode & 0xFF) << 16) |
                       ((k->flags & LLKHF_EXTENDED) ? (1L << 24) : 0) |
                       (1L << 29) |                     /* Alt held */
                       (up ? (3L << 30) : 0);
            PostMessageA(Wnd, up ? WM_SYSKEYUP : WM_SYSKEYDOWN,
                         (WPARAM)vk, l);
            return 1;                   /* nobody else sees it */
        }
    }
    return CallNextHookEx(KbHook, code, wp, lp);
}

static void PumpMessages(void)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static int W32_Init(void)
{
    WNDCLASSA wc;
    RECT rc = { 0, 0, PIX_W * SCALE, PIX_H * SCALE };
    DWORD style = WIN_STYLE;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ITEDScreen";
    if (!RegisterClassA(&wc))
        return 0;

    memset(&Bmi, 0, sizeof(Bmi));
    Bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    Bmi.bmiHeader.biWidth = PIX_W;
    Bmi.bmiHeader.biHeight = -PIX_H;    /* top-down */
    Bmi.bmiHeader.biPlanes = 1;
    Bmi.bmiHeader.biBitCount = 32;
    Bmi.bmiHeader.biCompression = BI_RGB;

    AdjustWindowRect(&rc, style, FALSE);
    Wnd = CreateWindowA("ITEDScreen",
                        "Impulse Tracker - 2026 AI port by Elias W. / original by Jeffrey Lim",
                        style,
                        CW_USEDEFAULT, CW_USEDEFAULT,
                        rc.right - rc.left, rc.bottom - rc.top,
                        NULL, NULL, wc.hInstance, NULL);
    if (!Wnd)
        return 0;

    ShowWindow(Wnd, SW_SHOW);
    KbHook = SetWindowsHookExA(WH_KEYBOARD_LL, KbHookProc,
                               GetModuleHandle(NULL), 0);
    return 1;
}

static void W32_UnInit(void)
{
    if (KbHook) {
        UnhookWindowsHookEx(KbHook);
        KbHook = NULL;
    }
    FreeBackBuffer();
    if (Wnd) {
        DestroyWindow(Wnd);
        Wnd = NULL;
    }
    UnregisterClassA("ITEDScreen", GetModuleHandle(NULL));
}

static void W32_Present(const screen_cell_t *cells)
{
    HDC dc;
    (void)cells;                /* rasterizer reads the shared buffer */

    Screen_Rasterize(Pixels);
    dc = GetDC(Wnd);
    PaintWindow(dc);
    ReleaseDC(Wnd, dc);
    PumpMessages();
}

static int W32_Key(void)
{
    PumpMessages();
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
static int W32_KeyEvent(it_key_t *k)
{
    PumpMessages();
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

static void W32_Mouse(it_mouse_t *m)
{
    PumpMessages();
    m->px = MousePX < 0 ? 0 : (MousePX >= PIX_W ? PIX_W - 1 : MousePX);
    m->py = MousePY < 0 ? 0 : (MousePY >= PIX_H ? PIX_H - 1 : MousePY);
    m->x = m->px / 8;
    m->y = m->py / 8;
    m->b = MouseB;
}

/* feature 016: the system file dialog (it_dialog_win32.c). Afterwards
 * nothing typed into the dialog may reach the tracker and no modifier
 * may count as held: drop queued keys and pending key messages, and
 * close any Shift chord/marking with a release event. */
int Dialog_Win32(HWND owner, const it_dialog_req_t *req, it_dialog_res_t *res);

static int W32_FileDialog(const it_dialog_req_t *req, it_dialog_res_t *res)
{
    MSG msg;
    int r = Dialog_Win32(Wnd, req, res);

    while (PeekMessage(&msg, Wnd, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
        ;
    KeyHead = KeyTail = 0;
    SkipChar = 0;
    CurScan = 0;
    CurFlags = ITKF_PRESSED;
    PushKey(ITK_SHIFT_RELEASE);
    if (Wnd)
        SetForegroundWindow(Wnd);
    return r;
}

const screen_backend_t Screen_BackendWin32 = {
    W32_Init, W32_UnInit, W32_Present, W32_Key, W32_Mouse, W32_KeyEvent,
    W32_FileDialog, NULL
};

#endif /* _WIN32 */
