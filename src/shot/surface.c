/* surface.c - screen capture and selection/crop buffers. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"
#include <gdiplus.h>

int shot_capture_screen(ShotApp *a)
{
    HDC screen;
    HDC mem = NULL;
    HBITMAP old = NULL;
    int w, h;

    shot_free_surface(a);

    a->vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    a->vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    a->vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    a->vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (a->vsw <= 0) a->vsw = GetSystemMetrics(SM_CXSCREEN);
    if (a->vsh <= 0) a->vsh = GetSystemMetrics(SM_CYSCREEN);
    if (a->vsw <= 0 || a->vsh <= 0) return 0;

    w = a->vsw; h = a->vsh;
    a->screenBmp = make_dib(w, h, 32, &a->screenBits);
    if (!a->screenBmp) return 0;

    screen = GetDC(NULL);
    if (!screen) { shot_free_surface(a); return 0; }

    /* Keep screenBmp selected into a private DC for the lifetime of the
     * capture: both the overlay paint and the crop read from a->screenDC, and
     * an unselected DC would fall back to its 1x1 monochrome default bitmap. */
    a->screenDC = CreateCompatibleDC(screen);
    if (!a->screenDC) { ReleaseDC(NULL, screen); shot_free_surface(a); return 0; }
    mem = a->screenDC;
    old = SelectObject(mem, a->screenBmp);
    BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY | CAPTUREBLT);
    (void)old;
    ReleaseDC(NULL, screen);

    /* A screen BitBlt leaves the alpha byte at 0. GDI+ composites against it,
     * so the dim wash drawn over the canvas would collapse to solid black.
     * Mark the capture opaque; every repaint copies these bytes across. */
    {
        int y;
        for (y = 0; y < h; y++) {
            BGRAPX *row = (BGRAPX *)((BYTE *)a->screenBits + (size_t)w * 4 * (size_t)y);
            int x;
            for (x = 0; x < w; x++) row[x].a = 0xFF;
        }
    }

    if (a->compBmp) {
        free_dib(a->compBmp, a->compBits);
        a->compBmp = NULL; a->compBits = NULL;
    }
    a->compBmp = make_dib(w, h, 32, &a->compBits);
    if (!a->compBmp) { shot_free_surface(a); return 0; }
    return 1;
}

static void shot_free_base(ShotApp *a);

void shot_free_surface(ShotApp *a)
{
    if (a->screenGp) { GdipDisposeImage((GpImage *)a->screenGp); a->screenGp = NULL; }
    if (a->screenDC) { DeleteDC(a->screenDC); a->screenDC = NULL; }
    if (a->screenBmp) { DeleteObject(a->screenBmp); a->screenBmp = NULL; }
    a->screenBits = NULL;
    if (a->compGp) { GdipDisposeImage((GpImage *)a->compGp); a->compGp = NULL; }
    if (a->compBmp) { free_dib(a->compBmp, a->compBits); a->compBmp = NULL; }
    a->compBits = NULL;
    shot_free_base(a);
    shot_free_layer(a);
    if (a->active) { stroke_free(a->active); a->active = NULL; }
    a->dragging = 0;
    a->haveSel = 0;
    a->selecting = 0;
    a->selDrag = 0;
    a->btnDown = 0;
    a->hoverValid = 0;
    shot_clear_strokes(a);
}

/* Crop the current selection out of the virtual-screen capture. Mosaic and
 * blur read from this so they always sample the *original* pixels, never the
 * user's own annotations. */
/* One place that tears the crop down. baseGp wraps baseBmp's HBITMAP, so the
 * GDI+ view has to go first - and it has to be *disposed*, not just forgotten:
 * a GpBitmap left alive over a deleted HBITMAP is both a leak and a handle
 * onto freed memory. */
static void shot_free_base(ShotApp *a)
{
    if (a->baseGp) { GdipDisposeImage((GpImage *)a->baseGp); a->baseGp = NULL; }
    if (a->baseBmp) { free_dib(a->baseBmp, a->baseBits); a->baseBmp = NULL; }
    a->baseBits = NULL;
    a->baseW = a->baseH = 0;
}

int shot_ensure_base(ShotApp *a)
{
    int w, h;
    HDC dc, mem;
    HBITMAP old;

    if (!a->haveSel || !a->screenBmp) return 0;
    w = a->sel.right - a->sel.left;
    h = a->sel.bottom - a->sel.top;
    if (w <= 0 || h <= 0) return 0;
    if (a->baseBmp && a->baseW == w && a->baseH == h) return 1;

    shot_free_base(a);
    a->baseBmp = make_dib(w, h, 32, &a->baseBits);
    if (!a->baseBmp) return 0;
    a->baseW = w; a->baseH = h;

    dc = GetDC(NULL);
    if (!dc) { shot_free_base(a); return 0; }
    mem = CreateCompatibleDC(dc);
    if (!mem) { ReleaseDC(NULL, dc); shot_free_base(a); return 0; }

    old = SelectObject(mem, a->baseBmp);
    BitBlt(mem, 0, 0, w, h, a->screenDC,
           a->sel.left - a->vsx, a->sel.top - a->vsy, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(NULL, dc);
    return 1;
}

void shot_clear_strokes(ShotApp *a)
{
    int i;
    for (i = 0; i < a->nstrokes; i++) stroke_free(a->strokes[i]);
    xfree(a->strokes);
    a->strokes = NULL;
    a->nstrokes = a->cstrokes = a->cursor = 0;
    if (a->active) { stroke_free(a->active); a->active = NULL; }
    a->dragging = 0;
}

void shot_undo(ShotApp *a)
{
    if (a->cursor > 0) a->cursor--;
    render_layer(a, NULL);
}

void shot_redo(ShotApp *a)
{
    if (a->cursor < a->nstrokes) a->cursor++;
    render_layer(a, NULL);
}

void shot_clear_selection(ShotApp *a)
{
    a->haveSel = 0;
    a->selecting = 0;
    a->selDrag = 0;
    SetRectEmpty(&a->sel);
    shot_clear_strokes(a);
    shot_free_base(a);
    shot_free_layer(a);
}
