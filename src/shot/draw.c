/* draw.c - stroke rendering and the annotation layer. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"
#include <gdiplus.h>

/* strokes                                                             */
/* ================================================================== */

void stroke_free(Stroke *s)
{
    if (!s) return;
    xfree(s->pts);
    xfree(s->text);
    xfree(s);
}

void stroke_add_point(Stroke *s, int x, int y)
{
    if (!s) return;
    if (s->npts == s->cpts) {
        int cap = s->cpts ? s->cpts * 2 : 64;
        POINT *np = (POINT *)xrealloc(s->pts, (size_t)cap * sizeof(POINT));
        if (!np) return;
        s->pts = np; s->cpts = cap;
    }
    /* drop duplicates so the smoothing pass sees no zero-length segments */
    if (s->npts && s->pts[s->npts - 1].x == x && s->pts[s->npts - 1].y == y) return;
    s->pts[s->npts].x = x;
    s->pts[s->npts].y = y;
    s->npts++;
}

/* ------------------------------------------------------------------ */
/* base-image pixel access (for mosaic / blur)                         */
/* ------------------------------------------------------------------ */

typedef struct {
    BYTE *bits;      /* BGRA, top-down */
    int   stride;
    int   w, h;
} BaseView;

static void base_pixel(BaseView *v, int x, int y, BGRAPX *out)
{
    BGRAPX *row, px;
    if (!v || !v->bits || v->w <= 0 || v->h <= 0) { memset(out, 0, sizeof(*out)); return; }
    if (x < 0) x = 0; else if (x >= v->w) x = v->w - 1;
    if (y < 0) y = 0; else if (y >= v->h) y = v->h - 1;
    row = (BGRAPX *)(v->bits + (size_t)v->stride * (size_t)y);
    px = row[x];
    if (px.a != 0 && px.a != 255) {
        /* captures are opaque, but stay correct for translucent sources */
        px.b = (BYTE)min(255, (int)((unsigned)px.b * 255 / px.a));
        px.g = (BYTE)min(255, (int)((unsigned)px.g * 255 / px.a));
        px.r = (BYTE)min(255, (int)((unsigned)px.r * 255 / px.a));
    }
    *out = px;
}

static int base_view(ShotApp *a, BaseView *v)
{
    memset(v, 0, sizeof(*v));
    if (!a->baseBits) return 0;
    v->bits   = (BYTE *)a->baseBits;
    v->stride = a->baseW * 4;
    v->w = a->baseW;
    v->h = a->baseH;
    return 1;
}

/* ------------------------------------------------------------------ */

static void draw_polyline(GpGraphics *g, Stroke *s, GpPen *pen)
{
    GpPointF *pts;
    int i;
    REAL r;

    if (s->npts <= 0 || !pen) return;

    if (s->npts == 1) {
        r = (REAL)s->width / 2.0f;
        gx_fill_ellipse(g, s->color, (REAL)s->pts[0].x - r, (REAL)s->pts[0].y - r,
                        r * 2.0f, r * 2.0f);
        return;
    }

    pts = (GpPointF *)xcalloc((size_t)s->npts, sizeof(GpPointF));
    if (!pts) return;
    for (i = 0; i < s->npts; i++) {
        pts[i].X = (REAL)s->pts[i].x;
        pts[i].Y = (REAL)s->pts[i].y;
    }
    /* Quadratic smoothing through midpoints gives a natural hand-drawn feel. */
    {
        GpPath *path = NULL;
        if (GdipCreatePath(FillModeAlternate, &path) == Ok) {
            GdipAddPathLine(path, pts[0].X, pts[0].Y, pts[1].X, pts[1].Y);
            for (i = 1; i + 1 < s->npts; i++) {
                REAL mx = (pts[i].X + pts[i + 1].X) / 2.0f;
                REAL my = (pts[i].Y + pts[i + 1].Y) / 2.0f;
                GdipAddPathBezier(path, pts[i].X, pts[i].Y, pts[i].X, pts[i].Y,
                                  pts[i + 1].X, pts[i + 1].Y, mx, my);
            }
            GdipAddPathLine(path, pts[s->npts - 1].X, pts[s->npts - 1].Y,
                            pts[s->npts - 1].X, pts[s->npts - 1].Y);
            GdipDrawPath(g, pen, path);
            GdipDeletePath(path);
        }
    }
    xfree(pts);
}

static void draw_arrow_head(GpGraphics *g, GpPen *pen, Stroke *s)
{
    REAL dx, dy, len, ang, hw, hl;
    GpPointF tip, l1, l2;
    dx = (REAL)(s->r.right - s->r.left);
    dy = (REAL)(s->r.bottom - s->r.top);
    len = (REAL)sqrt((double)(dx * dx + dy * dy));
    if (len < 0.5f) return;
    ang = (REAL)atan2((double)dy, (double)dx);
    hw  = (REAL)s->width * 1.2f;
    if (hw < 7)  hw = 7;
    if (hw > 30) hw = 30;
    hl  = hw * 2.2f;
    tip.X = (REAL)s->r.right;
    tip.Y = (REAL)s->r.bottom;
    l1.X = tip.X - (REAL)cos((double)ang) * hl + (REAL)sin((double)ang) * hw;
    l1.Y = tip.Y - (REAL)sin((double)ang) * hl - (REAL)cos((double)ang) * hw;
    l2.X = tip.X - (REAL)cos((double)ang) * hl - (REAL)sin((double)ang) * hw;
    l2.Y = tip.Y - (REAL)sin((double)ang) * hl + (REAL)cos((double)ang) * hw;
    gx_stroke_line_f(g, pen, tip.X, tip.Y, l1.X, l1.Y);
    gx_stroke_line_f(g, pen, tip.X, tip.Y, l2.X, l2.Y);
}

static int draw_text(ShotApp *a, GpGraphics *g, Stroke *s)
{
    GpFont *font;
    GpRectF layout;
    REAL em = (REAL)s->fontSize * 1.4f;
    int ok;

    if (!a->fontFamily) return 0;
    font = gx_font(a->fontFamily, (REAL)s->fontSize);
    if (!font) return 0;
    layout.X   = (REAL)s->r.left;
    layout.Y   = (REAL)s->r.top;
    layout.Width  = (REAL)(s->r.right - s->r.left);
    layout.Height = (REAL)(s->r.bottom - s->r.top);
    if (layout.Height < em) layout.Height = em;
    ok = gx_draw_text(g, font, s->color, s->text,
                      layout.X, layout.Y, layout.Width, layout.Height);
    GdipDeleteFont(font);
    return ok;
}

/* A w*h 32bpp bitmap over a buffer we own.
 *
 * GdipBitmapLockBits is not usable on this build - it rejects every bitmap
 * with InvalidParameter, whether the bits came from GDI+ or from us. That is
 * invisible to a leak check but fatal for any tool that has to read pixels
 * back, which is exactly what mosaic and blur do. Both build a small
 * intermediate image, so both can simply own the buffer and fill it directly,
 * then hand the finished bitmap to GDI+ as a draw source. */
static GpBitmap *make_scratch(int w, int h, BYTE **bitsOut)
{
    GpBitmap *bmp = NULL;
    BYTE *bits;

    *bitsOut = NULL;
    if (w <= 0 || h <= 0) return NULL;
    bits = (BYTE *)xcalloc((size_t)w * (size_t)h * 4u, 1);
    if (!bits) return NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  bits, &bmp) != Ok || !bmp) {
        xfree(bits);
        return NULL;
    }
    *bitsOut = bits;
    return bmp;
}

static int draw_mosaic(ShotApp *a, GpGraphics *g, Stroke *s)
{
    BaseView v;
    GpBitmap *small = NULL;
    BYTE  *bits = NULL;
    int cols, rows, bx, by, w, h, x0, y0, x1, y1, cell, sy, sx;
    BGRAPX *dst;

    if (!base_view(a, &v)) return 0;
    x0 = max(0, s->r.left);    y0 = max(0, s->r.top);
    x1 = min(v.w, s->r.right); y1 = min(v.h, s->r.bottom);
    w = x1 - x0; h = y1 - y0;
    if (w <= 0 || h <= 0) return 0;

    cell  = s->block > 0 ? s->block : 8;
    cols = (w + cell - 1) / cell;
    rows = (h + cell - 1) / cell;
    if (cols <= 0 || rows <= 0) return 0;
    if ((LONGLONG)cols * rows > 4000000) return 0;

    small = make_scratch(cols, rows, &bits);
    if (!small) return 0;

    dst = (BGRAPX *)bits;
    for (by = 0; by < rows; by++) {
        for (bx = 0; bx < cols; bx++) {
            int px = x0 + bx * cell, py = y0 + by * cell;
            int sw = min(cell, x0 + w - px);
            int sh = min(cell, y0 + h - py);
            int accR = 0, accG = 0, accB = 0, n = 0;
            BGRAPX c;
            for (sy = 0; sy < sh; sy += 2) {
                for (sx = 0; sx < sw; sx += 2) {
                    base_pixel(&v, px + sx, py + sy, &c);
                    accR += c.r; accG += c.g; accB += c.b; n++;
                }
            }
            if (!n) n = 1;
            c.b = (BYTE)(accB / n); c.g = (BYTE)(accG / n);
            c.r = (BYTE)(accR / n); c.a = 255;
            dst[(size_t)by * (size_t)cols + bx] = c;
        }
    }

    gx_draw_image_rect_i(g, (GpImage *)small, x0, y0, w, h,
                         InterpolationModeNearestNeighbor);

    GdipDisposeImage((GpImage *)small);
    xfree(bits);
    return 1;
}

static int draw_blur(ShotApp *a, GpGraphics *g, Stroke *s)
{
    BaseView v;
    GpBitmap *small = NULL, *up = NULL;
    BYTE  *smallBits = NULL, *upBits = NULL;
    GpGraphics *ug = NULL;
    int ok = 0, factor, x0, y0, x1, y1, w, h, x, y, sw2, sh2;
    BGRAPX *dst;

    if (!base_view(a, &v)) return 0;
    x0 = max(0, s->r.left);    y0 = max(0, s->r.top);
    x1 = min(v.w, s->r.right); y1 = min(v.h, s->r.bottom);
    w = x1 - x0; h = y1 - y0;
    if (w < 4 || h < 4) return 0;

    /* box blur by round-tripping through a 1/factor bitmap */
    factor = 12;
    if (factor > w / 2) factor = max(1, w / 2);
    if (factor > h / 2) factor = max(1, h / 2);
    sw2 = (w + factor - 1) / factor;
    sh2 = (h + factor - 1) / factor;
    if (sw2 <= 0 || sh2 <= 0) return 0;

    small = make_scratch(sw2, sh2, &smallBits);
    if (!small) return 0;
    dst = (BGRAPX *)smallBits;
    for (y = 0; y < sh2; y++)
        for (x = 0; x < sw2; x++)
            base_pixel(&v, x0 + x * factor, y0 + y * factor,
                       &dst[(size_t)y * (size_t)sw2 + x]);

    /* scale back up through a full-size intermediate, then place it */
    up = make_scratch(w, h, &upBits);
    if (up && GdipGetImageGraphicsContext((GpImage *)up, &ug) == Ok) {
        gx_draw_image_rect_i(ug, (GpImage *)small, 0, 0, w, h,
                             InterpolationModeBilinear);
        GdipDeleteGraphics(ug);
        gx_draw_image_rect_i(g, (GpImage *)up, x0, y0, w, h,
                             InterpolationModeBilinear);
        ok = 1;
    }

    if (up) GdipDisposeImage((GpImage *)up);
    xfree(upBits);
    GdipDisposeImage((GpImage *)small);
    xfree(smallBits);
    return ok;
}

int draw_stroke(ShotApp *a, GpGraphics *g, Stroke *s)
{
    GpPen *pen = NULL;
    GpSolidFill *fill = NULL;
    ARGB col;
    REAL w;
    int ok = 1;
    RECT rr;

    if (!s) return 0;
    rr = s->r;
    if (rr.right < rr.left) { int t = rr.left; rr.left = rr.right; rr.right = t; }
    if (rr.bottom < rr.top) { int t = rr.top; rr.top = rr.bottom; rr.bottom = t; }

    switch (s->kind) {
    case T_MOSAIC: return draw_mosaic(a, g, s);
    case T_BLUR:   return draw_blur(a, g, s);
    case T_TEXT:   return draw_text(a, g, s);
    default: break;
    }

    col = s->color;
    /* The eraser punches a hole in the annotation layer. */
    GdipSetCompositingMode(g, s->kind == T_ERASER
                               ? CompositingModeSourceCopy
                               : CompositingModeSourceOver);

    w = (REAL)(s->width > 0 ? s->width : 1);
    pen = gx_pen(col, w,
                 (s->kind == T_LINE || s->kind == T_ARROW) ? LineCapFlat : LineCapRound,
                 s->kind == T_PEN ? LineJoinRound : LineJoinMiter);
    if (!pen) { GdipSetCompositingMode(g, CompositingModeSourceOver); return 0; }
    if (GdipCreateSolidFill(col, &fill) != Ok) {
        GdipDeletePen(pen);
        GdipSetCompositingMode(g, CompositingModeSourceOver);
        return 0;
    }

    switch (s->kind) {
    case T_PEN:
    case T_ERASER:
        draw_polyline(g, s, pen);
        break;
    case T_LINE:
        gx_stroke_line_f(g, pen, (REAL)s->r.left, (REAL)s->r.top,
                         (REAL)s->r.right, (REAL)s->r.bottom);
        break;
    case T_ARROW:
        gx_stroke_line_f(g, pen, (REAL)s->r.left, (REAL)s->r.top,
                         (REAL)s->r.right, (REAL)s->r.bottom);
        draw_arrow_head(g, pen, s);
        break;
    case T_RECT:
        if (s->filled)
            gx_fill_rect(g, col, (REAL)rr.left, (REAL)rr.top,
                         (REAL)(rr.right - rr.left), (REAL)(rr.bottom - rr.top));
        gx_stroke_rect(g, col, w, (REAL)rr.left, (REAL)rr.top,
                       (REAL)(rr.right - rr.left), (REAL)(rr.bottom - rr.top));
        break;
    case T_ELLIPSE:
        if (s->filled)
            gx_fill_ellipse(g, col, (REAL)rr.left, (REAL)rr.top,
                            (REAL)(rr.right - rr.left), (REAL)(rr.bottom - rr.top));
        gx_stroke_ellipse(g, col, w, (REAL)rr.left, (REAL)rr.top,
                          (REAL)(rr.right - rr.left), (REAL)(rr.bottom - rr.top));
        break;
    case T_FILL:
        gx_fill_rect(g, col, (REAL)rr.left, (REAL)rr.top,
                     (REAL)(rr.right - rr.left), (REAL)(rr.bottom - rr.top));
        break;
    default:
        ok = 0;
        break;
    }

    GdipSetCompositingMode(g, CompositingModeSourceOver);
    GdipDeleteBrush((GpBrush *)fill);
    GdipDeletePen(pen);
    return ok;
}

/* ================================================================== */
/* layer management                                                    */
/* ================================================================== */

void shot_free_layer(ShotApp *a)
{
    if (!a) return;
    if (a->layer) { GdipDisposeImage((GpImage *)a->layer); a->layer = NULL; }
    xfree(a->layerBits);
    a->layerBits = NULL;
}

int shot_ensure_layer(ShotApp *a)
{
    int w = a->sel.right - a->sel.left;
    int h = a->sel.bottom - a->sel.top;
    GpBitmap *bmp = NULL;
    void *bits = NULL;
    UINT iw = 0, ih = 0;

    if (a->layer) {
        if (GdipGetImageWidth((GpImage *)a->layer, &iw) == Ok &&
            GdipGetImageHeight((GpImage *)a->layer, &ih) == Ok &&
            (int)iw == w && (int)ih == h)
            return 1;
        GdipDisposeImage((GpImage *)a->layer);
        a->layer = NULL;
        xfree(a->layerBits);
        a->layerBits = NULL;
    }
    if (w <= 0 || h <= 0) return 0;

    /* The bitmap is created over a buffer we own rather than a NULL scan0.
     * A NULL scan0 hands the allocation to GDI+, and a bitmap allocated that
     * way rejects GdipBitmapLockBits on this build (InvalidParameter) - which
     * is why the annotation layer is kept readable from plain memory. */
    bits = xcalloc((size_t)w * (size_t)h * 4u, 1);
    if (!bits) return 0;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppPARGB,
                                  bits, &bmp) != Ok || !bmp) {
        xfree(bits);
        return 0;
    }
    a->layer = bmp;
    a->layerBits = bits;
    return 1;
}

int render_layer(ShotApp *a, Stroke *inProgress)
{
    GpGraphics *g = NULL;
    int i;

    if (!a->layer || !a->haveSel) return 0;
    if (GdipGetImageGraphicsContext((GpImage *)a->layer, &g) != Ok) return 0;
    gx_set_quality(g);
    gx_clear(g, 0x00000000, (REAL)(a->sel.right - a->sel.left),
                         (REAL)(a->sel.bottom - a->sel.top));

    for (i = 0; i < a->cursor && i < a->nstrokes; i++)
        if (a->strokes[i]) draw_stroke(a, g, a->strokes[i]);
    if (inProgress) draw_stroke(a, g, inProgress);

    GdipDeleteGraphics(g);
    return 1;
}

void draw_init(ShotApp *a)
{
    if (!a->fontFamily) {
        GpFontFamily *fam = NULL;
        if (GdipCreateFontFamilyFromName(L"Microsoft YaHei UI", NULL, &fam) != Ok)
            GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam);
        if (fam) a->fontFamily = fam;
    }
}

void draw_shutdown(ShotApp *a)
{
    if (a->fontFamily) { GdipDeleteFontFamily(a->fontFamily); a->fontFamily = NULL; }
    shot_free_layer(a);
    if (a->active) { stroke_free(a->active); a->active = NULL; }
    shot_clear_strokes(a);
    xfree(a->tb);
    a->tb = NULL; a->ntb = a->ctb = 0;
}
