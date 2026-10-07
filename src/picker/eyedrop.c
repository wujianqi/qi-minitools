/* eyedrop.c - freeze the screen, then pick a pixel through a magnifier. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "eyedrop.h"

#define ED_CLASS   L"MuskPickerEyeDrop"
#define ED_CELLS   9      /* sample window, in pixels per side            */
#define ED_ZOOM    12     /* magnification factor                          */
/* GetStockObject's monochrome stock bitmap index; the stock_* spelling for it
 * is missing from this toolchain's headers, OBJ_BITMAP is a different enum. */
#define ED_STOCK_BITMAP 0
#define ED_WATCH_ID    1     /* cursor watcher, armed only while standing down */
#define ED_WATCH_MS    60
/* app_scale_px(1) rounds to 1 even at 125% DPI, so an int unit multiplier
 * silently under-scales every chrome dimension. Use a REAL factor instead;
 * ED_SCALE_PX is the same scale for plain integer pixel maths. */
#define ED_SCALE     ((REAL)app_scale_px(100) / 100.0f)
#define ED_SCALE_PX(n) app_scale_px(n)

static const wchar_t *ED_FONT = L"Consolas";

typedef struct {
    int      vsx, vsy, vsw, vsh;
    /* The capture lives in bmp/bits and is never drawn on; the overlay frame
     * is rebuilt into wBmp/wBits on every WM_PAINT. Painting straight into
     * the capture would stack the dim wash and leave ghost loupes behind. */
    HBITMAP  bmp;
    void    *bits;
    HDC      srcDC;           /* bmp stays selected here, the pristine source */
    HBITMAP  wBmp;
    void    *wBits;
    GpImage *gp;               /* GDI+ view of the work canvas               */
    int      magX, magY;         /* cursor, screen coords                  */
    int      gridX, gridY;       /* hovered cell inside the loupe, -1 none  */
    Rgb      hovered;
    int      quit, picked;
    Rgb      result;
    GpFontFamily *fam;
    /* The taskbar belongs to the shell - see ed_stand_down. */
    int      down;              /* standing down: hidden, tray clickable        */
    HWND     tray[4];           /* Shell_TrayWnd, and the secondary monitor's   */
    int      ntray;
    DWORD    trayScan;          /* GetTickCount() of the last scan              */
} EyeDrop;

static void ed_free(EyeDrop *e)
{
    if (e->fam)  { GdipDeleteFontFamily(e->fam); e->fam = NULL; }
    if (e->gp)   { GdipDisposeImage((GpImage *)e->gp); e->gp = NULL; }
    /* srcDC must be torn down before the DIB it holds, or DeleteObject fails. */
    if (e->srcDC) { DeleteDC(e->srcDC); e->srcDC = NULL; }
    if (e->bmp)   { DeleteObject(e->bmp); e->bmp = NULL; }
    if (e->wBmp)  { DeleteObject(e->wBmp); e->wBmp = NULL; }
    e->bits = NULL;
    e->wBits = NULL;
}

static Rgb ed_sample(EyeDrop *e, int sx, int sy)
{
    Rgb c;
    BGRAPX *row;
    if (sx < e->vsx) sx = e->vsx;
    if (sy < e->vsy) sy = e->vsy;
    if (sx >= e->vsx + e->vsw) sx = e->vsx + e->vsw - 1;
    if (sy >= e->vsy + e->vsh) sy = e->vsy + e->vsh - 1;
    row = (BGRAPX *)((BYTE *)e->bits + (size_t)e->vsw * 4 * (size_t)(sy - e->vsy));
    c.r = row[sx - e->vsx].r;
    c.g = row[sx - e->vsx].g;
    c.b = row[sx - e->vsx].b;
    return c;
}

/* Place the loupe so it never covers the pixel being inspected. */
static void ed_loupe_pos(EyeDrop *e, int *lx, int *ly, int box)
{
    int x = e->magX + box / 2 + ED_SCALE_PX(18);
    int y = e->magY + box / 2 + ED_SCALE_PX(18);
    if (x + box + ED_SCALE_PX(24) > e->vsx + e->vsw)
        x = e->magX - box / 2 - ED_SCALE_PX(18);
    if (y + box + ED_SCALE_PX(60) > e->vsy + e->vsh)
        y = e->magY - box / 2 - ED_SCALE_PX(18);
    if (x < e->vsx + ED_SCALE_PX(4)) x = e->vsx + ED_SCALE_PX(4);
    if (y < e->vsy + ED_SCALE_PX(4)) y = e->vsy + ED_SCALE_PX(4);
    *lx = x; *ly = y;
}

/* ------------------------------------------------------------------ */
/* standing down for the shell taskbar                                 */
/* ------------------------------------------------------------------ */
/*
 * The overlay is fullscreen and topmost, so while it is up it wins hit-testing
 * everywhere - the notification area included. Arm the picker, then click the
 * screenshot icon in the tray, and that click lands here instead: the
 * eyedropper reports a colour, the screenshot never starts, and the two tray
 * entries read as one button. That is the mixing, and it is not a tray-menu
 * problem: each app's own menu is already its own.
 *
 * The taskbar is not the picker's to swallow, so the overlay gives it back.
 * Hiding is the switch - a hidden window is skipped by the hit test, which
 * hands the click to the real taskbar with no hook, no region and no second
 * topmost window to fight over z-order. Move the cursor onto the taskbar and
 * the loupe steps aside; move it back and the loupe returns.
 */

/* Shell_TrayWnd is the primary taskbar, Shell_SecondaryTrayWnd the one on a
 * second monitor. Either can own the notification area, and which one does
 * depends on where the shell put the icons, so both are collected. */
static BOOL CALLBACK ed_collect_tray(HWND h, LPARAM p)
{
    EyeDrop *e = (EyeDrop *)p;
    wchar_t cls[64];
    if (e->ntray >= (int)(sizeof(e->tray) / sizeof(e->tray[0]))) return FALSE;
    if (!IsWindowVisible(h)) return TRUE;
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (_wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0)
        e->tray[e->ntray++] = h;
    return TRUE;
}

/* Is this point inside one of the shell's taskbars? The list is refreshed at
 * most twice a second: a taskbar does not travel under the cursor, and this
 * runs out of a mouse-move handler, where a full top-level enumeration on
 * every move is exactly what makes a loupe feel sticky. An empty list - no
 * shell window found - answers "no", which leaves the overlay behaving the
 * way it always has rather than standing down over nothing. */
static int ed_over_tray(EyeDrop *e, int x, int y)
{
    DWORD now = GetTickCount();
    int i;
    if (!e->ntray || now - e->trayScan >= 500) {
        e->ntray = 0;
        e->trayScan = now;
        EnumWindows(ed_collect_tray, (LPARAM)e);
    }
    for (i = 0; i < e->ntray; i++) {
        RECT r;
        if (!IsWindow(e->tray[i])) continue;
        if (!GetWindowRect(e->tray[i], &r)) continue;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return 1;
    }
    return 0;
}

/* Is the window under the cursor one of the shell taskbars we stepped aside
 * for? Compared on the root, because the hit test hands back whichever child
 * of the taskbar the point is over - the notification area is a child of it. */
static int ed_is_shell_tray(EyeDrop *e, HWND w)
{
    HWND root = GetAncestor(w, GA_ROOT);
    int i;
    for (i = 0; i < e->ntray; i++)
        if (e->tray[i] == root) return 1;
    return 0;
}

/* Has something else taken the screen from us?
 *
 * Asked with the real hit test, which is why nothing has to be said between
 * the programs: this overlay is topmost and covers the desktop, so the window
 * under the cursor is us - unless something newer has put itself above us,
 * and that *is* the answer. No protocol, no shared flag, no way for two tools
 * to disagree about what is happening, and nothing to keep in step when one of
 * them is not even running.
 *
 * Only meaningful while we are up: hidden over the taskbar, the hit test
 * answers for the taskbar, and that is us standing down rather than us being
 * covered. The resume path asks the question separately. */
static int ed_covered(EyeDrop *e, HWND h)
{
    POINT p;
    HWND over;
    if (e->down) return 0;
    GetCursorPos(&p);
    over = WindowFromPoint(p);
    return over != NULL && over != h;
}

/* Sample the cursor and repaint. Shared by the move handler and by the resume
 * path, where the cursor has moved while the overlay was hidden and nothing
 * was delivered to find out. */
static void ed_track(EyeDrop *e, HWND h, int cx, int cy)
{
    int cell = (int)(ED_SCALE * ED_ZOOM + 0.5f);
    int box  = ED_CELLS * cell;
    int lx, ly;
    if (cx < e->vsx) cx = e->vsx;
    if (cy < e->vsy) cy = e->vsy;
    if (cx >= e->vsx + e->vsw) cx = e->vsx + e->vsw - 1;
    if (cy >= e->vsy + e->vsh) cy = e->vsy + e->vsh - 1;
    e->magX = cx; e->magY = cy;
    e->hovered = ed_sample(e, cx, cy);
    ed_loupe_pos(e, &lx, &ly, box);
    e->gridX = e->gridY = -1;
    if (cx >= lx && cx < lx + box && cy >= ly && cy < ly + box) {
        e->gridX = (cx - lx) / cell;
        e->gridY = (cy - ly) / cell;
        e->hovered = ed_sample(e,
                              e->magX + e->gridX - ED_CELLS / 2,
                              e->magY + e->gridY - ED_CELLS / 2);
    }
    InvalidateRect(h, NULL, FALSE);
}

static void ed_stand_down(EyeDrop *e, HWND h)
{
    if (e->down) return;
    e->down = 1;
    /* The crosshair belongs to the loupe; over the taskbar the user is
     * reaching for a button, and a crosshair there reads as "pick this". */
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
}

static void ed_resume(EyeDrop *e, HWND h)
{
    POINT p;
    HWND over;
    if (!e->down) return;
    e->down = 0;
    /* Asked while still hidden, so the hit test reports what is really there.
     * The taskbar is us - that is what standing down is for, and it is the one
     * thing that may be underneath when we come back. Anything else is a tool
     * mid-capture, and coming back on top of it is the single thing this
     * overlay must never do. */
    GetCursorPos(&p);
    over = WindowFromPoint(p);
    if (over != NULL && over != h && !ed_is_shell_tray(e, over)) {
        e->quit = 1;
        DestroyWindow(h);
        return;
    }
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    /* Hiding the foreground window gives the foreground away, so take it back
     * or Esc stops cancelling the pick. */
    SetForegroundWindow(h);
    SetFocus(h);
    ed_track(e, h, p.x, p.y);
}

/* The watcher that stands in for the mouse moves a hidden window never gets,
 * and for the one thing that has no message at all: another tool taking the
 * screen. */
static void ed_watch(EyeDrop *e, HWND h)
{
    /* Standing down is decided in the move handler and undone here, because a
     * hidden window gets no moves and so cannot notice the cursor coming back
     * by itself. */
    if (e->down) {
        POINT p;
        GetCursorPos(&p);
        /* Asked first, and on purpose. A press is judged against where the
         * cursor is now: if it has already left the taskbar, that press
         * belonged to the desktop and the pick resumes to receive it. */
        if (ed_over_tray(e, p.x, p.y)) {
            if (GetAsyncKeyState(VK_LBUTTON)  & 0x8001 ||
                GetAsyncKeyState(VK_RBUTTON) & 0x8001 ||
                GetAsyncKeyState(VK_ESCAPE)   & 0x8001) {
                e->quit = 1;
                DestroyWindow(h);
            }
            return;
        }
        ed_resume(e, h);
        return;
    }

    /* Something put itself above us while we were armed and waiting. This is
     * the case no tray click ever warned us about - the capture came from its
     * own hotkey, or the pick was armed with the loupe showing - and an armed
     * pick outliving it is a fullscreen loupe coming back over whatever the
     * user did next. End it here; we do not find out who it was, and do not
     * need to. */
    if (ed_covered(e, h)) {
        e->quit = 1;
        DestroyWindow(h);
    }
}

static void ed_paint(EyeDrop *e, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc, mem;
    HGDIOBJ old;
    GpGraphics *g = NULL;
    REAL S = ED_SCALE;
    int cell = (int)(S * ED_ZOOM + 0.5f);
    int box  = ED_CELLS * cell;
    int lx, ly, i, j;

    dc = BeginPaint(h, &ps);
    if (!dc) return;
    /* A DIB cannot be selected straight into a BeginPaint window DC here —
     * SelectObject returns NULL and the overlay stays blank — so render via a
     * scratch memory DC and blit once, the same way the shot overlay does. */
    mem = CreateCompatibleDC(dc);
    if (!mem) { EndPaint(h, &ps); return; }
    old = SelectObject(mem, e->wBmp);
    if (!old || old == HGDI_ERROR) {
        DeleteDC(mem);
        EndPaint(h, &ps);
        return;
    }
    /* Reset the canvas to the untouched capture: without this the dim wash
     * compounds on every repaint and the previous loupe stays on screen. */
    BitBlt(mem, 0, 0, e->vsw, e->vsh, e->srcDC, 0, 0, SRCCOPY);
    if (e->gp && GdipGetImageGraphicsContext((GpImage *)e->gp, &g) == Ok) {
        gx_set_quality(g);

        ed_loupe_pos(e, &lx, &ly, box);

        /* dim wash so the loupe reads clearly. gx_clear would use
         * CompositingModeSourceCopy and write a literal 0x30000000 (i.e. black
         * with an alpha byte that GDI's blit path discards), so blend a
         * translucent black instead. GDI+ renders the brush alpha at roughly
         * half its nominal strength, hence 0x60 for the ~18% wash we want. */
        gx_fill_rect(g, 0x60000000, 0, 0, (REAL)e->vsw, (REAL)e->vsh);

        /* loupe backdrop */
        gx_round_rect_bg(g, 0xF01C1C1E, (REAL)lx, (REAL)ly,
                         (REAL)box, (REAL)box, (REAL)(S * 8));

        for (j = 0; j < ED_CELLS; j++) {
            for (i = 0; i < ED_CELLS; i++) {
                Rgb c = ed_sample(e,
                                  e->magX + i - ED_CELLS / 2,
                                  e->magY + j - ED_CELLS / 2);
                ARGB argb = 0xFF000000u | ((ARGB)c.r << 16) | ((ARGB)c.g << 8) | c.b;
                gx_fill_rect(g, argb, (REAL)(lx + i * cell), (REAL)(ly + j * cell),
                             (REAL)cell, (REAL)cell);
            }
        }
        /* grid */
        for (i = 1; i < ED_CELLS; i++) {
            gx_stroke_line(g, 0x30FFFFFF, 1.0f, (REAL)(lx + i * cell), (REAL)ly,
                           (REAL)(lx + i * cell), (REAL)(ly + box));
            gx_stroke_line(g, 0x30FFFFFF, 1.0f, (REAL)lx, (REAL)(ly + i * cell),
                           (REAL)(lx + box), (REAL)(ly + i * cell));
        }
        /* hovered cell + centre cell */
        if (e->gridX >= 0) {
            ARGB hc = 0xFF000000u | ((ARGB)e->hovered.r << 16) |
                      ((ARGB)e->hovered.g << 8) | e->hovered.b;
            gx_stroke_rect(g, 0xFFFFD60F, (REAL)S * 2.0f,
                           (REAL)(lx + e->gridX * cell), (REAL)(ly + e->gridY * cell),
                           (REAL)cell, (REAL)cell);
            gx_stroke_rect(g, hc, (REAL)S,
                           (REAL)(lx + e->gridX * cell), (REAL)(ly + e->gridY * cell),
                           (REAL)cell, (REAL)cell);
        }
        gx_stroke_rect(g, 0xFFFF3B30, (REAL)S * 2.0f,
                       (REAL)(lx + (ED_CELLS / 2) * cell) - 1,
                       (REAL)(ly + (ED_CELLS / 2) * cell) - 1,
                       (REAL)cell + 2, (REAL)cell + 2);
        gx_stroke_rect(g, 0xFFFFFFFF, (REAL)S, (REAL)lx, (REAL)ly, (REAL)box, (REAL)box);

        /* readout under the loupe: hex on the first line, coordinates on the
         * second, with the centre-pixel swatch pinned to the right edge. */
        {
            GpFont *f  = gx_font(e->fam, S * 15.0f);
            GpFont *f2 = gx_font(e->fam, S * 11.0f);
            wchar_t hex[32], pos[48];
            REAL sw = S * 22.0f;                     /* swatch width       */
            REAL pad = S * 8.0f;
            REAL tx = (REAL)lx + pad;                /* text left edge     */
            REAL tw = (REAL)box - pad * 2.0f - sw - S * 6.0f;
            Rgb ctr = ed_sample(e, e->magX, e->magY);
            _snwprintf_s(hex, 32, _TRUNCATE, L"#%02X%02X%02X",
                         e->hovered.r, e->hovered.g, e->hovered.b);
            _snwprintf_s(pos, 48, _TRUNCATE, L"%d, %d",
                         e->magX - e->vsx, e->magY - e->vsy);
            if (tw < S * 20.0f) tw = S * 20.0f;

            gx_round_rect_bg(g, 0xF01C1C1E, (REAL)lx, (REAL)(ly + box + S * 6),
                             (REAL)box, (REAL)(S * 42), (REAL)(S * 8));
            if (f) {
                gx_draw_text(g, f, 0xFFFFFFFF, hex, tx,
                             (REAL)(ly + box + S * 9), tw, (REAL)(S * 20));
                GdipDeleteFont(f);
            }
            if (f2) {
                gx_draw_text(g, f2, 0x90FFFFFF, pos, tx,
                             (REAL)(ly + box + S * 27), tw, (REAL)(S * 14));
                GdipDeleteFont(f2);
            }
            {
                ARGB cc = 0xFF000000u | ((ARGB)ctr.r << 16) |
                          ((ARGB)ctr.g << 8) | ctr.b;
                REAL sx = (REAL)(lx + box) - pad - sw;
                REAL sy = (REAL)(ly + box + S * 11);
                gx_fill_rect(g, cc, sx, sy, sw, (REAL)(S * 32));
                gx_stroke_rect(g, 0x60FFFFFF, 1.0f, sx, sy, sw, (REAL)(S * 32));
            }
        }

        /* hint */
        {
            GpFont *f = gx_font(e->fam, (REAL)(S * 13));
            if (f) {
                int w = app_scale_px(430), h = app_scale_px(30);
                int hx = e->vsx + (e->vsw - w) / 2;
                int hy = e->vsy + e->vsh - app_scale_px(64);
                gx_round_rect_bg(g, 0xC01C1C1E, (REAL)hx, (REAL)hy,
                                 (REAL)w, (REAL)h, (REAL)(S * 8));
                gx_draw_text(g, f, 0xFFFFFFFF,
                             L"左键取色   ·   Esc / 右键取消",
                             (REAL)hx, (REAL)(hy + S * 7), (REAL)w, (REAL)(S * 18));
                GdipDeleteFont(f);
            }
        }

        /* crosshair */
        gx_stroke_line(g, 0x40FFFFFF, (REAL)S, (REAL)e->vsx, (REAL)e->magY,
                       (REAL)(e->vsx + e->vsw), (REAL)e->magY);
        gx_stroke_line(g, 0x40FFFFFF, (REAL)S, (REAL)e->magX, (REAL)e->vsy,
                       (REAL)e->magX, (REAL)(e->vsy + e->vsh));

        GdipDeleteGraphics(g);
    }
    BitBlt(dc, 0, 0, e->vsw, e->vsh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, GetStockObject(ED_STOCK_BITMAP));
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static EyeDrop *g_ed;      /* the active overlay instance */
static int      ed_running; /* one overlay at a time; see eyedrop_active */

int eyedrop_active(void)
{
    return ed_running;
}

static LRESULT CALLBACK ed_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    EyeDrop *e = g_ed;

    if (!e) return DefWindowProcW(h, m, w, l);

    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        ed_paint(e, h);
        return 0;

    case WM_TIMER:
        if (w == ED_WATCH_ID) ed_watch(e, h);
        return 0;

    case WM_MOUSEMOVE:
        SetCursor(LoadCursorW(NULL, IDC_CROSS));
        ed_track(e, h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        /* Standing down is decided here and undone by the watcher: once hidden
         * the window is told nothing, so it cannot notice the cursor leaving
         * the taskbar by itself. */
        if (!e->down && ed_over_tray(e, e->magX, e->magY)) ed_stand_down(e, h);
        return 0;

    case WM_LBUTTONDOWN:
        /* A press already in the queue when the overlay stepped aside was
         * aimed at the taskbar. Taking it would be picking the colour of a
         * tray icon the user was trying to click. */
        if (e->down) return 0;
        e->result = e->hovered;
        e->picked = 1;
        e->quit = 1;
        DestroyWindow(h);
        return 0;

    case WM_LBUTTONUP:
        return 0;

    case WM_RBUTTONDOWN:
    case WM_CONTEXTMENU:
        if (e->down) return 0;
        e->quit = 1;
        DestroyWindow(h);
        return 0;

    case WM_CHAR:
        if (w == VK_ESCAPE) { e->quit = 1; DestroyWindow(h); return 0; }
        if (w == VK_RETURN) {
            e->result = e->hovered;
            e->picked = 1;
            e->quit = 1;
            DestroyWindow(h);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        /* arrow keys nudge the sample point for pixel-hunting */
        switch (w) {
        case VK_LEFT:  e->magX = max(e->vsx, e->magX - 1); e->hovered = ed_sample(e, e->magX, e->magY); InvalidateRect(h, NULL, FALSE); return 0;
        case VK_RIGHT: e->magX = min(e->vsx + e->vsw - 1, e->magX + 1); e->hovered = ed_sample(e, e->magX, e->magY); InvalidateRect(h, NULL, FALSE); return 0;
        case VK_UP:    e->magY = max(e->vsy, e->magY - 1); e->hovered = ed_sample(e, e->magX, e->magY); InvalidateRect(h, NULL, FALSE); return 0;
        case VK_DOWN:  e->magY = min(e->vsy + e->vsh - 1, e->magY + 1); e->hovered = ed_sample(e, e->magX, e->magY); InvalidateRect(h, NULL, FALSE); return 0;
        default: break;
        }
        break;

    case WM_DESTROY:
        /* No PostQuitMessage: this overlay runs its own nested loop and must
         * not leave a WM_QUIT in the thread queue for the host window. */
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int eyedrop_begin(HWND owner, Rgb *out)
{
    WNDCLASSEXW wc;
    EyeDrop e;
    MSG msg;
    HDC screen;

    memset(&e, 0, sizeof(e));
    e.vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    e.vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    e.vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    e.vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (e.vsw <= 0) e.vsw = GetSystemMetrics(SM_CXSCREEN);
    if (e.vsh <= 0) e.vsh = GetSystemMetrics(SM_CYSCREEN);
    if (e.vsw <= 0 || e.vsh <= 0) return 0;

    e.bmp = make_dib(e.vsw, e.vsh, 32, &e.bits);
    if (!e.bmp) return 0;
    screen = GetDC(NULL);
    if (!screen) { ed_free(&e); return 0; }
    e.srcDC = CreateCompatibleDC(screen);
    if (!e.srcDC) { ReleaseDC(NULL, screen); ed_free(&e); return 0; }
    if (!SelectObject(e.srcDC, e.bmp)) {
        ReleaseDC(NULL, screen);
        ed_free(&e);
        return 0;
    }
    BitBlt(e.srcDC, 0, 0, e.vsw, e.vsh, screen, 0, 0, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, screen);

    /* The work canvas is what GDI+ draws into and what gets blitted out. */
    e.wBmp = make_dib(e.vsw, e.vsh, 32, &e.wBits);
    if (!e.wBmp) { ed_free(&e); return 0; }
    {   /* seed the canvas once; every repaint re-copies from the pristine frame */
        HDC tmp = CreateCompatibleDC(e.srcDC);
        if (tmp) {
            HGDIOBJ o = SelectObject(tmp, e.wBmp);
            if (o && o != HGDI_ERROR) {
                BitBlt(tmp, 0, 0, e.vsw, e.vsh, e.srcDC, 0, 0, SRCCOPY);
                SelectObject(tmp, GetStockObject(ED_STOCK_BITMAP));
            }
            DeleteDC(tmp);
        }
    }

    /* A screen BitBlt leaves the alpha byte at 0. GDI+ composites against it,
     * so a translucent wash would sit on a "fully transparent" canvas and
     * collapse to black. Force both buffers opaque. */
    {
        int y;
        for (y = 0; y < e.vsh; y++) {
            BGRAPX *row = (BGRAPX *)((BYTE *)e.bits + (size_t)e.vsw * 4 * (size_t)y);
            BGRAPX *wrow = (BGRAPX *)((BYTE *)e.wBits + (size_t)e.vsw * 4 * (size_t)y);
            int x;
            for (x = 0; x < e.vsw; x++) { row[x].a = 0xFF; wrow[x].a = 0xFF; }
        }
    }

    /* GDI+ needs its own image object over the canvas pixels; an HBITMAP
     * handle is not one. */
    if (GdipCreateBitmapFromScan0(e.vsw, e.vsh, e.vsw * 4,
                                 PixelFormat32bppARGB, e.wBits, &e.gp) != Ok) {
        e.gp = NULL;
        ed_free(&e);
        return 0;
    }

    if (GdipCreateFontFamilyFromName(ED_FONT, NULL, &e.fam) != Ok)
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &e.fam);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize      = sizeof(wc);
    wc.lpfnWndProc = ed_proc;
    wc.hInstance   = GetModuleHandleW(NULL);
    wc.lpszClassName = ED_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ed_free(&e);
        return 0;
    }

    /* The overlay sits on top of the frozen frame, so it can be click-through
     * in spirit only: we need the mouse, so keep it interactive but do not
     * steal focus from whatever the user is inspecting. */
    {
        HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                 ED_CLASS, L"", WS_POPUP,
                                 e.vsx, e.vsy, e.vsw, e.vsh,
                                 NULL, NULL, wc.hInstance, NULL);
        if (!h) { ed_free(&e); return 0; }
        g_ed = &e;
        {
            POINT pt;
            GetCursorPos(&pt);
            e.magX = pt.x; e.magY = pt.y;
        }
        e.hovered = ed_sample(&e, e.magX, e.magY);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(h);
        SetFocus(h);
        /* Armed from the tray the cursor is still sitting on the taskbar, so
         * the first thing this overlay would do is swallow the tray. Stand
         * down straight away; the watcher brings it back on the first move
         * out onto the desktop. */
        if (ed_over_tray(&e, e.magX, e.magY)) ed_stand_down(&e, h);

        /* Runs for the whole life of the overlay, not only while standing
         * down: half of what it watches for has no message at all. */
        SetTimer(h, ED_WATCH_ID, ED_WATCH_MS, NULL);
        ed_running = 1;
        while (IsWindow(h)) {
            BOOL got = GetMessageW(&msg, NULL, 0, 0);
            if (got <= 0) break;                 /* WM_QUIT or error */
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ed_running = 0;
        KillTimer(h, ED_WATCH_ID);
        if (IsWindow(h)) DestroyWindow(h);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnregisterClassW(ED_CLASS, wc.hInstance);
        g_ed = NULL;
    }

    ed_free(&e);
    if (e.picked && out) { *out = e.result; return 1; }
    return 0;
}
