/* editor.c - the fullscreen overlay: region selection, annotation input,
 * and the small modal used to type text onto a capture. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"
#include <gdiplus.h>

static const wchar_t *OVERLAY_CLASS = L"ShotOverlayCls";
static const wchar_t *TEXTDLG_CLASS = L"ShotTextDlgCls";

/* Overlay timer that re-checks the hover delay. See the WM_MOUSEMOVE case. */
#define TIP_TIMER   2
#define TIP_POLL_MS 120

static void clamp_point(ShotApp *a, int *sx, int *sy)
{
    if (*sx < a->vsx) *sx = a->vsx;
    if (*sy < a->vsy) *sy = a->vsy;
    if (*sx > a->vsx + a->vsw) *sx = a->vsx + a->vsw;
    if (*sy > a->vsy + a->vsh) *sy = a->vsy + a->vsh;
}

/* ------------------------------------------------------------------ */
/* snapping to window edges                                            */
/* ------------------------------------------------------------------ */
/*
 * Dragging a region out to the last pixel by hand is the one thing a capture
 * tool cannot do for you, and lining up with the edge of the window next to it
 * is the case that comes up constantly: a window border, a panel, the edge of
 * the taskbar. So while the button is down, the moving corner is pulled to the
 * nearest window edge within a few pixels, the way a drawing tool does.
 *
 * The edges are collected once when the drag starts rather than on every
 * WM_MOUSEMOVE, because a drag is a burst of messages and this walks every
 * window on the desktop. A drag lasts a fraction of a second, so nothing on
 * screen can meaningfully change underneath it.
 */

/* How near an edge has to be before it takes the corner, and so how twitchy
 * the snap feels. SNAP_PX is in shot.h because the self-test measures against
 * it too. */

static BOOL CALLBACK snap_collect(HWND w, LPARAM p)
{
    ShotApp *a = (ShotApp *)p;
    DWORD pid = 0;
    RECT r;
    wchar_t cls[64];

    if (a->nedge >= MAX_EDGES) return FALSE;
    if (!IsWindowVisible(w)) return TRUE;
    /* Our own windows are not something to align a capture to - the overlay is
     * in the way and the toolbar moves with the selection. */
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    if (!GetWindowRect(w, &r)) return TRUE;
    /* Off-screen and zero-sized windows are still enumerable: a suspended
     * app's windows and the shell's hidden desktop hosts are both real
     * rectangles that nothing is drawn in. */
    if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;
    if (r.right <= a->vsx || r.bottom <= a->vsy ||
        r.left  >= a->vsx + a->vsw || r.top >= a->vsy + a->vsh) return TRUE;
    /* The desktop itself spans the whole screen, so its edges are the screen
     * edges - which are already a snap target on their own. */
    if (GetClassNameW(w, cls, 64) &&
        (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0))
        return TRUE;
    a->edge[a->nedge++] = r;
    return TRUE;
}

/* Gathers the snap targets for a drag that is starting. Exported so the
 * self-test can hand it a fixed set of rectangles: the real thing enumerates
 * live windows, and a self-test running on a console has none of its own. */
void shot_snap_collect(ShotApp *a)
{
    a->nedge = 0;
    EnumWindows(snap_collect, (LPARAM)a);
    a->snapX = a->snapY = -1;
}

/* Pulls (x,y) to the nearest edge in reach and records which way, so the
 * caller can draw the guide. The two axes are decided independently: a corner
 * that is lined up on one side and 4px off on the other should still get the
 * one guide it earned. */
void shot_snap_apply(ShotApp *a, int *px, int *py)
{
    int limit = app_scale_px(SNAP_PX);
    int bestX = -1, bestY = -1, bestDX = limit, bestDY = limit;
    int i;

    for (i = 0; i < a->nedge; i++) {
        const RECT *r = &a->edge[i];
        int d;
        d = abs(r->left - *px);   if (d < bestDX) { bestDX = d; bestX = r->left;  }
        d = abs(r->right - *px);  if (d < bestDX) { bestDX = d; bestX = r->right; }
        d = abs(r->top - *py);    if (d < bestDY) { bestDY = d; bestY = r->top;   }
        d = abs(r->bottom - *py); if (d < bestDY) { bestDY = d; bestY = r->bottom; }
    }
    /* The screen's own edges are always in reach - a capture that runs to the
     * very edge of the display has no window to line up with, and this is the
     * case people reach for the tool to get exactly right. */
    {
        int d;
        d = abs(a->vsx - *px);            if (d < bestDX) { bestDX = d; bestX = a->vsx; }
        d = abs((a->vsx + a->vsw) - *px); if (d < bestDX) { bestDX = d; bestX = a->vsx + a->vsw; }
        d = abs(a->vsy - *py);            if (d < bestDY) { bestDY = d; bestY = a->vsy; }
        d = abs((a->vsy + a->vsh) - *py); if (d < bestDY) { bestDY = d; bestY = a->vsy + a->vsh; }
    }
    a->snapX = bestX;
    a->snapY = bestY;
    if (bestX >= 0) *px = bestX;
    if (bestY >= 0) *py = bestY;
}

/* ------------------------------------------------------------------ */
/* picking the window under the cursor                                  */
/* ------------------------------------------------------------------ */

/* How far the pointer has to travel before a press counts as a drag. Below
 * this the hand shakes, and a release that never meant to draw anything takes
 * the window under the cursor instead - which is the default way to select
 * now: click a window, get its exact bounds, no corner-dragging required. */
#define PICK_SLOP 4

typedef struct {
    POINT pt;
    HWND  skipA, skipB;   /* our own overlay and host window */
    RECT  rect;
    int   found;
} WindowPick;

static BOOL CALLBACK window_pick_cb(HWND w, LPARAM p)
{
    WindowPick *c = (WindowPick *)p;
    RECT r;
    LONG ex;

    if (w == c->skipA || w == c->skipB) return TRUE;
    if (!IsWindowVisible(w)) return TRUE;
    ex = GetWindowLongW(w, GWL_EXSTYLE);
    /* a click-through layer cannot be what the user is aiming at, even
     * though its rectangle happens to cover the point */
    if (ex & WS_EX_TRANSPARENT) return TRUE;
    if (!GetWindowRect(w, &r)) return TRUE;
    if (r.right - r.left <= 1 || r.bottom - r.top <= 1) return TRUE;
    if (!PtInRect(&r, c->pt)) return TRUE;
    /* Progman/WorkerW are deliberately NOT skipped, unlike the snap-edge
     * list: the desktop is a window whose rectangle is the whole screen, so
     * a click on empty desktop selects the full picture the same way it
     * selects any other window. */
    c->rect = r;
    c->found = 1;
    return FALSE;          /* EnumWindows walks in Z order: first hit wins */
}

/* The frontmost window containing the point, clipped to the virtual screen -
 * the capture only covers that rectangle, and a window hanging off the edge
 * would otherwise crop into coordinates the frozen frame does not have. */
int shot_window_under_cursor(int sx, int sy, HWND skipA, HWND skipB, RECT *out)
{
    WindowPick c;
    RECT vs, clipped;

    c.pt.x = sx; c.pt.y = sy;
    c.skipA = skipA; c.skipB = skipB;
    c.found = 0;
    SetRect(&c.rect, 0, 0, 0, 0);
    EnumWindows(window_pick_cb, (LPARAM)&c);
    if (!c.found) return 0;
    vs.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vs.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vs.right  = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!IntersectRect(&clipped, &c.rect, &vs)) return 0;
    if (clipped.right - clipped.left <= 1 || clipped.bottom - clipped.top <= 1)
        return 0;
    if (out) *out = clipped;
    return 1;
}

static void sel_from_drag(ShotApp *a, int x0, int y0, int x1, int y1)
{
    RECT r;
    /* Only the corner being dragged is snapped; the one the button went down
     * on stays where it was put. Snapping both would move a region the user
     * had already placed, which is the opposite of helpful. */
    if (a->selDrag) shot_snap_apply(a, &x1, &y1);
    SetRect(&r, x0, y0, x1, y1);
    r.right  = max(r.right,  r.left + 1);
    r.bottom = max(r.bottom, r.top + 1);
    a->sel = r;
    a->magSel = 1;
}

/* Turns a->sel into the committed selection and brings up the toolbar. Both
 * ways of selecting land here: a drag (through finish_selection, which has
 * already snapped the moving corner) and a click on a window, whose bounds
 * are exact by construction and must not be nudged by the snap. */
static void commit_selection(ShotApp *a)
{
    a->selecting = 0;
    a->selDrag  = 0;
    a->btnDown  = 0;
    a->haveSel  = 1;
    a->snapX = a->snapY = -1;
    a->hoverValid = 0;
    if (a->sel.right - a->sel.left >= 2 && a->sel.bottom - a->sel.top >= 2) {
        shot_ensure_base(a);
        shot_ensure_layer(a);
        render_layer(a, NULL);
        layout_toolbar(a);
    } else {
        shot_clear_selection(a);
    }
}

/* Turns the region being dragged into the committed selection, and brings up
 * the toolbar. This used to live only in WM_LBUTTONUP, which made the button-up
 * the single point of failure for the whole interaction: anything that ate it -
 * an IME grabbing the cursor, another window taking the capture - left the
 * overlay stuck showing a growing rectangle with no toolbar and no dim, which
 * is indistinguishable from a hang. Esc was the only way out. */
static void finish_selection(ShotApp *a, int sx, int sy)
{
    clamp_point(a, &sx, &sy);
    /* The committed rectangle is the dragged one, snapping and all, so it is
     * snapped by the same call - and the guides go with it, because a guide
     * left on screen after the corner has landed on the edge is a lie about
     * where the edge is. */
    sel_from_drag(a, a->sel.left, a->sel.top, sx, sy);
    commit_selection(a);
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

/* Dims everything outside one rectangle (4 bands). The selection and the
 * click-to-pick hover preview share it: both say "this part of the screen is
 * what the shot will be" the same way. */
static void dim_outside(GpGraphics *g, ARGB d, int ox, int oy, int ow, int oh,
                        int vsw, int vsh)
{
    gx_fill_rect(g, d, 0, 0, (REAL)vsw, (REAL)max(0, oy));
    gx_fill_rect(g, d, 0, (REAL)(oy + oh), (REAL)vsw,
                 (REAL)max(0, vsh - oy - oh));
    gx_fill_rect(g, d, 0, (REAL)oy, (REAL)max(0, ox), (REAL)oh);
    gx_fill_rect(g, d, (REAL)(ox + ow), (REAL)oy,
                 (REAL)max(0, vsw - ox - ow), (REAL)oh);
}

static void paint(ShotApp *a, HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc, mem;
    HBITMAP old;
    GpGraphics *g = NULL;
    int i;
    int ow, oh, ox, oy;

    dc = BeginPaint(hwnd, &ps);
    if (!dc || !a->compBmp || !a->screenBmp) { if (dc) EndPaint(hwnd, &ps); return; }

    mem = CreateCompatibleDC(dc);
    if (!mem) { EndPaint(hwnd, &ps); return; }
    old = SelectObject(mem, a->compBmp);

    /* 1. the frozen capture */
    BitBlt(mem, 0, 0, a->vsw, a->vsh, a->screenDC, 0, 0, SRCCOPY);

    if (GdipGetImageGraphicsContext((GpImage *)a->compGp, &g) != Ok) {
        SelectObject(mem, old); DeleteDC(mem); EndPaint(hwnd, &ps); return;
    }
    gx_set_quality(g);
    /* No clear here: the BitBlt above already laid down a pristine copy of the
     * capture, and wiping it to transparent black would turn the dim wash into
     * a solid black frame instead of a darkened screen. */

    ow = a->sel.right - a->sel.left;
    oh = a->sel.bottom - a->sel.top;
    ox = a->sel.left - a->vsx;
    oy = a->sel.top - a->vsy;

    /* 2. dim everything except the selection (4 bands) */
    if ((a->haveSel || a->selecting) && ow > 0 && oh > 0) {
        dim_outside(g, shot_dim_color(), ox, oy, ow, oh, a->vsw, a->vsh);
    }

    /* 2b. hover preview, before anything is selected: dim outside the window
     * a click would take and outline it, so what a release will select is on
     * screen before it happens. Nothing is dimmed while the drag is growing -
     * the rectangle then speaks for itself. */
    if (a->selecting && !a->selDrag && a->hoverValid) {
        int hx = a->hoverRect.left - a->vsx;
        int hy = a->hoverRect.top  - a->vsy;
        int hw = a->hoverRect.right  - a->hoverRect.left;
        int hh = a->hoverRect.bottom - a->hoverRect.top;
        dim_outside(g, shot_dim_color(), hx, hy, hw, hh, a->vsw, a->vsh);
        gx_stroke_rect(g, 0xFF0A84FF, (REAL)app_scale_px(2),
                       (REAL)hx, (REAL)hy, (REAL)hw, (REAL)hh);
    }

    /* 3. repaint the selection from the clean base.
     * The GDI+ view is built once per selection, not once per frame: this
     * runs on every mouse move, and allocating a bitmap per frame was a
     * large part of why dragging across a big selection felt like a hang. */
    if (a->haveSel && a->baseBmp) {
        if (!a->baseGp)
            GdipCreateBitmapFromHBITMAP(a->baseBmp, NULL, &a->baseGp);
        if (a->baseGp)
            gx_draw_image_rect_i(g, (GpImage *)a->baseGp, ox, oy, ow, oh,
                                 InterpolationModeNearestNeighbor);
    }
    /* 4. annotation layer, plus the stroke still being dragged.
     * The clone is only needed while a stroke is in flight, because the live
     * stroke has to be drawn somewhere without touching the real layer. */
    if (a->haveSel && a->layer) {
        GpImage  *src = (GpImage *)a->layer;
        GpBitmap *lc  = NULL;
        if (a->dragging && a->active &&
            GdipCloneBitmapAreaI(0, 0, a->baseW, a->baseH, PixelFormat32bppPARGB,
                                 (GpBitmap *)a->layer, &lc) == Ok) {
            GpGraphics *lg = NULL;
            if (GdipGetImageGraphicsContext((GpImage *)lc, &lg) == Ok) {
                gx_set_quality(lg);
                draw_stroke(a, lg, a->active);
                GdipDeleteGraphics(lg);
            }
            src = (GpImage *)lc;
        }
        /* drawn 1:1, so anything but a straight copy is wasted work */
        gx_draw_image_rect_i(g, src, ox, oy, a->baseW, a->baseH,
                             InterpolationModeNearestNeighbor);
        if (lc) GdipDisposeImage((GpImage *)lc);
    }

    /* 5. selection chrome */
    if ((a->selecting || a->haveSel) && ow > 0 && oh > 0) {
        REAL fx = (REAL)ox, fy = (REAL)oy, fw = (REAL)ow, fh = (REAL)oh;
        REAL S = (REAL)app_scale_px(1);
        gx_stroke_rect(g, 0xFF0A84FF, S * 2.0f, fx, fy, fw, fh);
        gx_stroke_dashed_rect(g, 0xFFFFFFFF, S, fx + 1, fy + 1, fw - 2, fh - 2);
        {
            REAL t = S * 7.0f;
            REAL cxx[4] = { fx, fx + fw, fx, fx + fw };
            REAL cyy[4] = { fy, fy, fy + fh, fy + fh };
            int sxx[4] = { 1, -1, 1, -1 };
            int syy[4] = { 1, 1, -1, -1 };
            for (i = 0; i < 4; i++) {
                gx_stroke_line(g, 0xFF0A84FF, S * 3.0f, cxx[i], cyy[i],
                               cxx[i] + sxx[i] * t, cyy[i]);
                gx_stroke_line(g, 0xFF0A84FF, S * 3.0f, cxx[i], cyy[i],
                               cxx[i], cyy[i] + syy[i] * t);
            }
        }
    }

    /* 6. toolbar + the crosshair lines */
    if (a->haveSel) draw_toolbar(a, g);
    if (a->selecting) draw_selection_guides(a, g);

    /* 6b. the hover bubble, on top of everything, once the cursor has rested
     * on one button long enough for the reader to want it. */
    if (a->haveSel) draw_toolbar_tip(a, g);
    else a->tipShown = -1;

    /* 7. hint before the first drag */
    if (a->selecting && !a->magSel) {
        const wchar_t *hint = L"点击窗口直接选取   ·   拖动可自由框选   ·   Esc 取消";
        int wpx = app_scale_px(420), hpx = app_scale_px(32);
        int hx = a->vsx + (a->vsw - wpx) / 2;
        int hy = a->vsy + a->vsh - app_scale_px(70);
        gx_fill_rect(g, 0xC01C1C1E, (REAL)hx, (REAL)hy, (REAL)wpx, (REAL)hpx);
        draw_hud_text(a, g, hint, hx, hy + app_scale_px(6), wpx,
                      app_scale_px(20), 0xFFFFFFFF, app_scale_px(13));
    }

    GdipDeleteGraphics(g);
    BitBlt(dc, 0, 0, a->vsw, a->vsh, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------------ */
/* strokes                                                             */
/* ------------------------------------------------------------------ */

static Stroke *new_stroke(ShotApp *a)
{
    Stroke *s = (Stroke *)xcalloc(1, sizeof(Stroke));
    if (!s) return NULL;
    s->kind = a->tool;
    s->color = a->color;
    s->width = a->width;
    s->filled = a->fillMode;
    s->block = a->mosaicBlock;
    s->fontSize = a->fontSize;
    return s;
}

static void commit_stroke(ShotApp *a)
{
    if (!a->active) return;
    if (a->nstrokes == a->cstrokes) {
        int cap = a->cstrokes ? a->cstrokes * 2 : 32;
        Stroke **ns = (Stroke **)xrealloc(a->strokes, (size_t)cap * sizeof(Stroke *));
        if (!ns) { stroke_free(a->active); a->active = NULL; a->dragging = 0; return; }
        a->strokes = ns; a->cstrokes = cap;
    }
    /* a new stroke invalidates the redo tail */
    while (a->nstrokes > a->cursor) { stroke_free(a->strokes[--a->nstrokes]); }
    a->strokes[a->nstrokes++] = a->active;
    a->cursor = a->nstrokes;
    a->active = NULL;
    a->dragging = 0;
    render_layer(a, NULL);
}

/* ------------------------------------------------------------------ */
/* toolbar actions                                                     */
/* ------------------------------------------------------------------ */

static void set_tool(ShotApp *a, int tbId)
{
    switch (tbId) {
    case TB_PEN:     a->tool = T_PEN;     break;
    case TB_ERASER:  a->tool = T_ERASER;  break;
    case TB_LINE:    a->tool = T_LINE;    break;
    case TB_RECT:    a->tool = T_RECT;    break;
    case TB_ELLIPSE: a->tool = T_ELLIPSE; break;
    case TB_ARROW:   a->tool = T_ARROW;   break;
    case TB_TEXT:    a->tool = T_TEXT;    break;
    case TB_MOSAIC:  a->tool = T_MOSAIC;  break;
    case TB_BLUR:    a->tool = T_BLUR;    break;
    case TB_FILL:    a->tool = T_FILL;    break;
    default: return;
    }
}

static void activate(ShotApp *a, int idx)
{
    TbItem *t;
    int id;
    if (idx < 0 || idx >= a->ntb) return;
    t = &a->tb[idx];
    id = t->id;

    if (id >= TB_SWATCH0) {
        int ci = t->colorIndex;
        if (ci >= 0) {
            extern ARGB shot_palette(int i);
            ARGB c = shot_palette(ci);
            if (c != 0) a->color = c;
        }
        return;
    }
    if (id >= TB_PEN && id <= TB_FILL) { set_tool(a, id); return; }

    switch (id) {
    case TB_UNDO:     shot_undo(a); break;
    case TB_REDO:     shot_redo(a); break;
    case TB_FILLMODE: a->fillMode = !a->fillMode; break;
    case TB_THICKER:
        if (a->tool == T_MOSAIC) a->mosaicBlock = min(64, a->mosaicBlock + 2);
        else a->width = min(64, a->width + 2);
        break;
    case TB_THINNER:
        if (a->tool == T_MOSAIC) a->mosaicBlock = max(2, a->mosaicBlock - 2);
        else a->width = max(1, a->width - 2);
        break;
    case TB_COPY:     shot_finish_and_copy(a); return;
    case TB_SAVE: {
        wchar_t *p = shot_default_filename();
        int ok = 0;
        if (p) {
            ok = shot_save_to_file(a, p);
            xfree(p);
        }
        shot_cancel(a);
        if (!ok) shot_notify(a, L"保存失败");
        return;
    }
    case TB_DONE:     shot_finish_and_copy(a); return;
    case TB_CANCEL:   shot_cancel(a); return;
    default: return;
    }
}

static void adjust_size(ShotApp *a, int delta)
{
    if (a->tool == T_MOSAIC) {
        a->mosaicBlock = max(2, min(64, a->mosaicBlock + delta * 2));
    } else if (a->tool == T_TEXT) {
        a->fontSize = max(10, min(200, a->fontSize + delta * 4));
    } else {
        a->width = max(1, min(64, a->width + delta * 2));
    }
}

/* ------------------------------------------------------------------ */
/* text input dialog                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    HWND   hwnd;
    HWND   edit;
    wchar_t *out;
    int   *outSize;
    int    size;
} TextDlg;

static TextDlg g_td;

static LRESULT CALLBACK textdlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* Modal dialog: the instance is a file-static, not window user-data. */
    TextDlg *td = &g_td;
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == 1 || LOWORD(w) == 2) {     /* OK / Cancel */
            if (td && LOWORD(w) == 1) {
                int n = GetWindowTextLengthW(td->edit) + 1;
                td->out = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
                if (td->out) {
                    GetWindowTextW(td->edit, td->out, n);
                    if (td->outSize) *td->outSize = n;
                }
            }
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int shot_text_input(HWND owner, wchar_t **outText, int *outSize)
{
    WNDCLASSEXW wc;
    TextDlg *td = &g_td;
    MSG msg;
    int S = app_scale_px(1);
    int W = app_scale_px(420), H = app_scale_px(190);
    RECT r;

    *outText = NULL;
    if (outSize) *outSize = 0;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = textdlg_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = TEXTDLG_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;

    memset(td, 0, sizeof(*td));
    td->out = NULL; td->outSize = outSize;

    GetWindowRect(owner, &r);
    td->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TEXTDLG_CLASS,
                               L"添加文字", WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
                               r.left + 40, r.top + 40, W, H, owner, NULL,
                               wc.hInstance, NULL);
    if (!td->hwnd) return 0;

    td->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                              WS_VSCROLL | ES_WANTRETURN,
                              S * 12, S * 34, W - S * 24, H - S * 80,
                              td->hwnd, (HMENU)(INT_PTR)101, wc.hInstance, NULL);
    {
        HFONT f = CreateFontW(-S * 15, 0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
        if (f) SendMessageW(td->edit, WM_SETFONT, (WPARAM)f, TRUE);
    }
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    W - S * 100, H - S * 40, S * 44, S * 26, td->hwnd,
                    (HMENU)(INT_PTR)2, wc.hInstance, NULL);
    CreateWindowExW(0, L"STATIC", L"输入要标注的文字（支持中文）",
                    WS_CHILD | WS_VISIBLE, S * 12, S * 10, W - S * 24, S * 18,
                    td->hwnd, (HMENU)(INT_PTR)102, wc.hInstance, NULL);
    SetFocus(td->edit);
    SetForegroundWindow(td->hwnd);

    while (IsWindow(td->hwnd) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(td->hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    /* Drain any messages that arrived after DestroyWindow. */
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(TEXTDLG_CLASS, wc.hInstance);
    *outText = td->out;
    return td->out != NULL;
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */

static void track_mouse(ShotApp *a, int sx, int sy)
{
    int cx = sx - a->sel.left, cy = sy - a->sel.top;
    RECT r;
    if (!a->active) return;
    switch (a->active->kind) {
    case T_PEN:
    case T_ERASER:
        stroke_add_point(a->active, cx, cy);
        break;
    default:
        r = a->active->r;
        r.right = cx; r.bottom = cy;
        a->active->r = r;
        break;
    }
}

static void start_stroke(ShotApp *a, int sx, int sy)
{
    if (a->tool == T_TEXT) {
        wchar_t *text = NULL;
        int size = 0;
        if (shot_text_input(a->overlay, &text, &size) && text) {
            Stroke *s = new_stroke(a);
            if (s) {
                int wpx, hpx;
                s->text = text;
                /* rough box; the text is drawn from the top-left anchor */
                s->r.left = sx - a->sel.left;
                s->r.top  = sy - a->sel.top;
                wpx = shot_text_width(a, s->text, s->fontSize);
                hpx = (s->fontSize * 3) / 2 + 4;
                s->r.right  = s->r.left + wpx;
                s->r.bottom = s->r.top  + hpx;
                if (a->nstrokes == a->cstrokes) {
                    int cap = a->cstrokes ? a->cstrokes * 2 : 32;
                    Stroke **ns = (Stroke **)xrealloc(a->strokes,
                                        (size_t)cap * sizeof(Stroke *));
                    if (ns) { a->strokes = ns; a->cstrokes = cap; }
                }
                if (a->nstrokes < a->cstrokes) {
                    while (a->nstrokes > a->cursor) stroke_free(a->strokes[--a->nstrokes]);
                    a->strokes[a->nstrokes++] = s;
                    a->cursor = a->nstrokes;
                    render_layer(a, NULL);
                } else {
                    stroke_free(s);
                }
            } else {
                xfree(text);
            }
        }
        return;
    }
    a->active = new_stroke(a);
    if (!a->active) return;
    a->dragging = 1;
    a->active->r.left = a->active->r.right = sx - a->sel.left;
    a->active->r.top  = a->active->r.bottom = sy - a->sel.top;
    if (a->tool == T_PEN || a->tool == T_ERASER)
        stroke_add_point(a->active, sx - a->sel.left, sy - a->sel.top);
}

/* The overlay is a single top-level window owned by the running ShotApp.
 * Reference it through a file-static instead of window user-data, which does
 * not round-trip reliably under this toolchain. */
static ShotApp *g_overlay_app;

static LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    ShotApp *a = g_overlay_app;
    int sx, sy;

    if (!a) return DefWindowProcW(h, m, w, l);

    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(a, h);
        return 0;

    case WM_MOUSEMOVE: {
        sx = GET_X_LPARAM(l) + a->vsx;
        sy = GET_Y_LPARAM(l) + a->vsy;
        a->magX = sx; a->magY = sy;
        if (a->selecting) {
            if (a->selDrag) {
                if (!(w & MK_LBUTTON) && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                    /* the button-up never arrived, so finish it here rather
                     * than leaving the region growing forever */
                    finish_selection(a, sx, sy);
                } else {
                    sel_from_drag(a, a->sel.left, a->sel.top, sx, sy);
                }
            } else if (a->btnDown) {
                if (!(w & MK_LBUTTON) && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                    /* the press was lost before it became a drag: nothing to
                     * finish, the next press decides again */
                    a->btnDown = 0;
                } else if (abs(sx - a->downX) > PICK_SLOP ||
                           abs(sy - a->downY) > PICK_SLOP) {
                    /* it moved: this press is a drag after all, and the
                     * free-form rectangle starts where the button did */
                    SetRect(&a->sel, a->downX, a->downY, a->downX + 1, a->downY + 1);
                    a->magSel = 0;
                    a->selDrag = 1;
                    shot_snap_collect(a);
                    sel_from_drag(a, a->downX, a->downY, sx, sy);
                }
            } else {
                /* hovering, nothing pressed: track the window a click would
                 * take, so the highlight on screen is exactly what a release
                 * selects. The trailing InvalidateRect repaints it. */
                RECT r;
                int got = shot_window_under_cursor(sx, sy, a->overlay, NULL, &r);
                a->hoverValid = got;
                if (got) a->hoverRect = r;
            }
        } else if (a->haveSel) {
            int h2 = hit_toolbar(a, sx, sy);
            if (h2 != a->tbHot) {
                a->tbHot = h2;
                /* Restart the hover delay, and make sure one repaint lands
                 * after it expires - a cursor that comes to rest sends no
                 * further WM_MOUSEMOVE, so without this the bubble would only
                 * ever appear while the mouse was still moving. */
                a->tipShown = -1;
                a->tipSince = GetTickCount();
                SetTimer(h, TIP_TIMER, TIP_POLL_MS, NULL);
            }
            if (a->dragging) track_mouse(a, sx, sy);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (w == TIP_TIMER) {
            if (a->tbHot < 0 || a->tbHot == a->tipShown)
                KillTimer(h, TIP_TIMER);
            else
                InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        /* Deliberately does NOT touch tbDown. ReleaseCapture() inside
         * WM_LBUTTONUP delivers this message synchronously, so clearing the
         * pressed index here would cancel the very click that released the
         * button - every toolbar button would go dead. A press that ends
         * somewhere other than the button is already handled in WM_LBUTTONUP,
         * which only activates when hit_toolbar still agrees. */
        return 0;

    case WM_LBUTTONDOWN: {
        SetCapture(h);
        sx = GET_X_LPARAM(l) + a->vsx;
        sy = GET_Y_LPARAM(l) + a->vsy;
        if (a->selecting) {
            /* The press is undecided: movement past PICK_SLOP grows into a
             * drag (the rectangle is started then, in WM_MOUSEMOVE), a
             * release without movement takes the window under the cursor.
             * Committing a 1x1 rect here, as this used to do, made a plain
             * click a guaranteed discard. */
            a->btnDown = 1;
            a->downX = sx; a->downY = sy;
        } else if (a->haveSel) {
            int idx = hit_toolbar(a, sx, sy);
            POINT p;
            p.x = sx; p.y = sy;
            if (idx >= 0) { a->tbDown = idx; }
            else if (PtInRect(&a->sel, p)) start_stroke(a, sx, sy);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        sx = GET_X_LPARAM(l) + a->vsx;
        sy = GET_Y_LPARAM(l) + a->vsy;
        ReleaseCapture();
        if (a->selecting) {
            a->btnDown = 0;
            if (a->selDrag) {
                finish_selection(a, sx, sy);
            } else {
                /* a click, not a drag: take the window under the cursor,
                 * bounds exact. The desktop counts - its window spans the
                 * screen, so a click on empty space selects the whole
                 * picture. The overlay stays up when nothing was found. */
                RECT r;
                if (shot_window_under_cursor(sx, sy, a->overlay, NULL, &r)) {
                    a->sel = r;
                    commit_selection(a);
                }
            }
        } else if (a->haveSel) {
            if (a->tbDown >= 0) {
                int idx = a->tbDown;
                a->tbDown = -1;
                if (hit_toolbar(a, sx, sy) == idx) activate(a, idx);
            }
            if (a->dragging) commit_stroke(a);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        if ((GetKeyState(VK_CONTROL) & 0x8000) || a->tool == T_MOSAIC) {
            adjust_size(a, delta > 0 ? 1 : -1);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_CHAR:
        if (w == VK_ESCAPE) {
            if (a->dragging) { a->dragging = 0; stroke_free(a->active); a->active = NULL; }
            else if (a->haveSel) shot_clear_selection(a);
            else shot_cancel(a);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (w == VK_RETURN) { shot_finish_and_copy(a); return 0; }
        break;

    case WM_KEYDOWN: {
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl) {
            switch (w) {
            case 'Z': if (shift) shot_redo(a); else shot_undo(a); goto redraw;
            case 'Y': shot_redo(a); goto redraw;
            case 'C': shot_finish_and_copy(a); return 0;
            case 'S': {
                wchar_t *p = shot_default_filename();
                int ok = 0;
                if (p) { ok = shot_save_to_file(a, p); xfree(p); }
                shot_cancel(a);
                if (!ok) shot_notify(a, L"保存失败");
                return 0;
            }
            default: break;
            }
        }
        if (!a->haveSel) return 0;
        switch (w) {
        case 'P': a->tool = T_PEN;     goto redraw;
        case 'E': a->tool = T_ERASER;  goto redraw;
        case 'L': a->tool = T_LINE;    goto redraw;
        case 'R': a->tool = T_RECT;    goto redraw;
        case 'O': a->tool = T_ELLIPSE; goto redraw;
        case 'A': a->tool = T_ARROW;   goto redraw;
        case 'T': a->tool = T_TEXT;    goto redraw;
        case 'M': a->tool = T_MOSAIC;  goto redraw;
        case 'B': a->tool = T_BLUR;    goto redraw;
        case 'F': a->tool = T_FILL;    goto redraw;
        case '[': adjust_size(a, -1);   goto redraw;
        case ']': adjust_size(a, 1);    goto redraw;
        case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            extern ARGB shot_palette(int i);
            ARGB c = shot_palette((int)w - '1');
            if (c) a->color = c;
            goto redraw;
        }
        default: break;
        }
        return 0;
redraw:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_APP + 2:
        /* State query for the outside probes. Hover, the drawn bubble and the
         * selection are all coordinates the app owns, so a probe can check
         * them without having to agree with it about DPI. */
        app_logw(L"query: sel=%ld,%ld,%ld,%ld tbHot=%d tbDown=%d tipShown=%d "
                 L"tool=%d strokes=%d cursor=%d base=%dx%d layer=%d",
                 (long)a->sel.left, (long)a->sel.top, (long)a->sel.right,
                 (long)a->sel.bottom, a->tbHot, a->tbDown, a->tipShown, (int)a->tool,
                 a->nstrokes, a->cursor, a->baseW, a->baseH, a->layer != NULL);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void shot_cancel(ShotApp *a)
{
    if (a->overlay && IsWindow(a->overlay)) DestroyWindow(a->overlay);
}

/* ------------------------------------------------------------------ */
/* keyboard ownership                                                  */
/* ------------------------------------------------------------------ */

/* A capture overlay is modal by construction: it covers every pixel of every
 * monitor, so it swallows every click, and the app it covers cannot be reached
 * with the mouse either. The only ways out are therefore the mouse and the
 * keyboard - and the keyboard only works if this window owns the focus.
 *
 * The overlay used to be created with WS_EX_NOACTIVATE, which forbids exactly
 * that. Esc, Enter, Ctrl+C and the tool letters all arrived nowhere, and a user
 * who had not managed to drag a selection - so had no toolbar to click Cancel
 * on - was stuck behind a wall they could not see. Task Manager could not help
 * either: it sits underneath a topmost fullscreen overlay, so its End Task
 * button could not be clicked, and Alt+F4 went to whichever app the keyboard
 * still belonged to. The only recovery was signing out of Windows.
 *
 * So the overlay takes the focus properly, and if that still fails a keyboard
 * hook forwards the keys it acts on. Being unable to get out is not a state
 * this window is allowed to be in. */

static int focus_is_ours(void)
{
    GUITHREADINFO gti;
    HWND  focus;
    DWORD fgTid, pid = 0;

    fgTid = (DWORD)GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    if (!fgTid) return 0;
    gti.cbSize = (DWORD)sizeof(gti);
    if (!GetGUIThreadInfo(fgTid, &gti)) return 0;
    focus = gti.hwndFocus;
    if (!focus) return 0;
    GetWindowThreadProcessId(focus, &pid);
    return pid == (DWORD)GetCurrentProcessId();
}

static int overlay_owns_keyboard(HWND h)
{
    GUITHREADINFO gti;
    DWORD fgTid;

    fgTid = (DWORD)GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    if (!fgTid) return 0;
    gti.cbSize = (DWORD)sizeof(gti);
    if (!GetGUIThreadInfo(fgTid, &gti)) return 0;
    return gti.hwndFocus == h;
}

static void focus_overlay(HWND h)
{
    HWND  fg    = GetForegroundWindow();
    DWORD fgTid = fg ? (DWORD)GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD myTid = GetCurrentThreadId();

    /* SetForegroundWindow is only honoured for a process the foreground window
     * admits, and a capture opened from the global hotkey has no such grant -
     * a hotkey message is not a click. Borrowing the foreground thread's input
     * queue across the call is what makes the request stick. */
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
}

/* ------------------------------------------------------------------ */
/* the rescue path                                                     */
/* ------------------------------------------------------------------ */

/* Installed only when the overlay could not take the focus for itself, so in
 * the normal case nothing is hooked at all. It forwards exactly the keys the
 * overlay binds and eats them, so they do not also reach the app being
 * captured. */

static HHOOK g_rescueHook   = NULL;
static HWND  g_rescueTarget = NULL;

static LRESULT CALLBACK rescue_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_rescueTarget && IsWindow(g_rescueTarget)) {
        UINT vk = (UINT)((const KBDLLHOOKSTRUCT *)lp)->vkCode;
        /* While one of our own windows holds the focus - the text dialog does
         * while it is open - the keys belong to that window, not to us. */
        if (!focus_is_ours()) {
            if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_BACK) {
                /* overlay_proc acts on these through WM_CHAR, which is also
                 * what a real keyboard delivers for them */
                PostMessageW(g_rescueTarget, WM_CHAR, (WPARAM)vk, 0);
            } else {
                /* the tool letters, the digits, [ ] and Ctrl+Z/Y/C/S are all
                 * read in WM_KEYDOWN */
                PostMessageW(g_rescueTarget, WM_KEYDOWN, (WPARAM)vk, 0);
            }
            return 1;
        }
    }
    return CallNextHookEx(g_rescueHook, code, wp, lp);
}

static void rescue_hook_install(HWND overlay)
{
    if (g_rescueHook) return;
    g_rescueTarget = overlay;
    g_rescueHook = SetWindowsHookExW(WH_KEYBOARD_LL, rescue_hook,
                                     (HINSTANCE)GetModuleHandleW(NULL), 0);
    if (!g_rescueHook) {
        g_rescueTarget = NULL;
        app_log("overlay: no keyboard focus and the rescue hook was refused");
    }
}

static void rescue_hook_remove(void)
{
    if (g_rescueHook) { UnhookWindowsHookEx(g_rescueHook); g_rescueHook = NULL; }
    g_rescueTarget = NULL;
}

/* ------------------------------------------------------------------ */
/* start / run                                                         */
/* ------------------------------------------------------------------ */

static void ensure_comp_gp(ShotApp *a)
{
    if (a->compGp) return;
    if (GdipCreateBitmapFromScan0(a->vsw, a->vsh, a->vsw * 4,
                                  PixelFormat32bppARGB, a->compBits,
                                  &a->compGp) != Ok)
        a->compGp = NULL;
}

/* Every nested window here ends with PostQuitMessage, and any of those posts
 * that is not consumed by the loop that owns it lands in the thread queue.
 * The main loop treats a WM_QUIT as "shut down", so a stray one makes the
 * process exit and the tray icon vanish with it - which is exactly the
 * failure this guard exists to prevent. Swallow anything left over on the way
 * out of a capture, and again before starting one. */
static void clear_pending_quit(void)
{
    MSG m;
    int guard = 0;
    while (PeekMessageW(&m, NULL, WM_QUIT, WM_QUIT, PM_REMOVE) && guard++ < 64)
        ;
}

int shot_begin(ShotApp *a, int delayMs)
{
    WNDCLASSEXW wc;
    MSG msg;
    DWORD start;
    int ok = 0;

    /* `capturing` rather than `overlay != NULL` as the guard: the delay pump
     * below runs before the overlay exists, and it dispatches whatever arrives
     * in the meantime. A second tray click (or hotkey press) landing in that
     * window would otherwise start a whole nested capture, and once the inner
     * one finished the outer one would open a second overlay - which reads as
     * the capture flashing and then popping back up. */
    if (a->capturing || a->overlay) return 0;
    a->capturing = 1;
    clear_pending_quit();

    if (!gdiplus_start()) goto done;
    draw_init(a);

    if (delayMs > 0) {
        /* let the caller (a menu, a tooltip) close first */
        start = GetTickCount();
        while ((GetTickCount() - start) < (DWORD)delayMs) {
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(20);
        }
    }

    if (!shot_capture_screen(a)) goto done;
    ensure_comp_gp(a);
    if (!a->compGp) { shot_free_surface(a); goto done; }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_proc;
    wc.hInstance     = a->inst;
    wc.hCursor       = NULL;                 /* keep the system arrow */
    wc.hbrBackground = NULL;
    wc.lpszClassName = OVERLAY_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        goto done;

    a->selecting = 1;
    a->haveSel = 0;
    a->magSel = 0;
    a->selDrag = 0;
    a->btnDown = 0;
    a->hoverValid = 0;
    a->tbHot = a->tbDown = -1;
    a->tipShown = -1;
    SetRectEmpty(&a->sel);

    a->overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, L"", WS_POPUP,
        a->vsx, a->vsy, a->vsw, a->vsh,
        NULL, NULL, a->inst, NULL);
    if (!a->overlay) { shot_free_surface(a); goto done; }
    g_overlay_app = a;

    ShowWindow(a->overlay, SW_SHOW);
    UpdateWindow(a->overlay);
    /* Not WS_EX_NOACTIVATE: the overlay has to own the keyboard, or Esc cannot
     * reach it and the user is trapped behind a fullscreen window. See the
     * "keyboard ownership" section above. */
    focus_overlay(a->overlay);
    if (!overlay_owns_keyboard(a->overlay)) rescue_hook_install(a->overlay);

    while (!a->quit && IsWindow(a->overlay)) {
        /* GetMessage returns -1 on error as well as 0 on WM_QUIT; treating
         * -1 as truthy kept the loop running on a dead queue, and a loop
         * that spins on immediate -1 returns is a busy core with a window
         * that answers nothing - the freeze shape, at full CPU. */
        int got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    a->overlay = NULL;
    g_overlay_app = NULL;
    shot_free_surface(a);
    UnregisterClassW(OVERLAY_CLASS, a->inst);
    clear_pending_quit();
    ok = 1;

done:
    rescue_hook_remove();
    clear_pending_quit();
    a->capturing = 0;
    return ok;
}

int shot_is_running(ShotApp *a)
{
    return a && (a->capturing || a->overlay != NULL);
}
