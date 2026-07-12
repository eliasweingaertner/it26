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

static HWND      Wnd;
static uint32_t  Pixels[PIX_W * PIX_H];
static BITMAPINFO Bmi;
static int       KeyQueue[64];
static int       KeyHead, KeyTail;
static int       WantQuit;
static int       MousePX, MousePY;      /* logical pixels 0..639/0..399 */
static int       MouseB;

static void PushKey(int k)
{
    int next = (KeyTail + 1) % 64;
    if (next != KeyHead) {
        KeyQueue[KeyTail] = k;
        KeyTail = next;
    }
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

static void PaintWindow(HDC dc)
{
    RECT rc;
    GetClientRect(Wnd, &rc);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom,
                  0, 0, PIX_W, PIX_H,
                  Pixels, &Bmi, DIB_RGB_COLORS, SRCCOPY);
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
        if (wp == VK_F9 && (GetKeyState(VK_SHIFT) & 0x8000)) {
            PushKey(ITK_SHIFT_F9);      /* message editor */
            return 0;
        }
        if (wp == VK_SCROLL) {
            PushKey(ITK_SCROLL_LOCK);
            return 0;
        }
        /* Alt combos (WM_SYSKEYDOWN with the menu key held) */
        if (GetKeyState(VK_MENU) & 0x8000) {
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
            case VK_F7:     PushKey(ITK_CTRL_F7);        return 0;
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
            default: break;
            }
        }
        if (wp == VK_DIVIDE) {          /* keypad '/', scan 135h */
            PushKey(ITK_KP_DIVIDE);
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
    case WM_CHAR:
        if (wp >= 32 && wp < 127)
            PushKey((int)wp);
        else if (wp >= 1 && wp <= 26 &&
                 wp != 8 && wp != 9 && wp != 13)
            PushKey((int)wp);           /* Ctrl-A..Z (BS/Tab/CR are
                                           delivered as VK keys above) */
        return 0;
    case WM_MOUSEMOVE:
        MousePX = (int)(short)LOWORD(lp) / SCALE;
        MousePY = (int)(short)HIWORD(lp) / SCALE;
        return 0;
    case WM_LBUTTONDOWN:
        MousePX = (int)(short)LOWORD(lp) / SCALE;
        MousePY = (int)(short)HIWORD(lp) / SCALE;
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
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

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
    Wnd = CreateWindowA("ITEDScreen", "Impulse Tracker", style,
                        CW_USEDEFAULT, CW_USEDEFAULT,
                        rc.right - rc.left, rc.bottom - rc.top,
                        NULL, NULL, wc.hInstance, NULL);
    if (!Wnd)
        return 0;

    ShowWindow(Wnd, SW_SHOW);
    return 1;
}

static void W32_UnInit(void)
{
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
        int k = KeyQueue[KeyHead];
        KeyHead = (KeyHead + 1) % 64;
        return k;
    }
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

const screen_backend_t Screen_BackendWin32 = {
    W32_Init, W32_UnInit, W32_Present, W32_Key, W32_Mouse
};

#endif /* _WIN32 */
