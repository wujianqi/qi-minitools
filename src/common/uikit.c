/* uikit.c - GDI+ drawing helpers shared by the tools. */
#include "uikit.h"

GpPen *gx_pen(ARGB c, REAL w, GpLineCap cap, GpLineJoin join)
{
    GpPen *p = NULL;
    if (w < 1.0f) w = 1.0f;
    if (GdipCreatePen1(c, w, UnitPixel, &p) != Ok) return NULL;
    GdipSetPenStartCap(p, cap);
    GdipSetPenEndCap(p, cap);
    GdipSetPenLineJoin(p, join);
    return p;
}

int gx_fill_rect(GpGraphics *g, ARGB c, REAL x, REAL y, REAL w, REAL h)
{
    GpSolidFill *b = NULL;
    int r = 0;
    if (w <= 0 || h <= 0) return 0;
    if (GdipCreateSolidFill(c, &b) != Ok) return 0;
    r = (GdipFillRectangle(g, b, x, y, w, h) == Ok);
    GdipDeleteBrush((GpBrush *)b);
    return r;
}

int gx_fill_ellipse(GpGraphics *g, ARGB c, REAL x, REAL y, REAL w, REAL h)
{
    GpSolidFill *b = NULL;
    int r = 0;
    if (w <= 0 || h <= 0) return 0;
    if (GdipCreateSolidFill(c, &b) != Ok) return 0;
    r = (GdipFillEllipse(g, b, x, y, w, h) == Ok);
    GdipDeleteBrush((GpBrush *)b);
    return r;
}

int gx_fill_poly(GpGraphics *g, ARGB c, const GpPointF *p, int n)
{
    GpSolidFill *b = NULL;
    int r = 0;
    if (!p || n < 3) return 0;
    if (GdipCreateSolidFill(c, &b) != Ok) return 0;
    r = (GdipFillPolygon(g, b, p, n, FillModeAlternate) == Ok);
    GdipDeleteBrush((GpBrush *)b);
    return r;
}

int gx_fill_path(GpGraphics *g, ARGB c, GpPath *path, GpFillMode mode)
{
    GpSolidFill *b = NULL;
    int r = 0;
    if (!path) return 0;
    if (GdipCreateSolidFill(c, &b) != Ok) return 0;
    /* GdipFillPath takes the fill mode from the path, not as an argument. */
    GdipSetPathFillMode(path, mode);
    r = (GdipFillPath(g, b, path) == Ok);
    GdipDeleteBrush((GpBrush *)b);
    return r;
}

int gx_stroke_line_f(GpGraphics *g, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2)
{
    if (!pen) return 0;
    return (GdipDrawLine(g, pen, x1, y1, x2, y2) == Ok);
}

int gx_stroke_line(GpGraphics *g, ARGB c, REAL pw, REAL x1, REAL y1, REAL x2, REAL y2)
{
    GpPen *p = gx_pen(c, pw, LineCapRound, LineJoinRound);
    int r;
    if (!p) return 0;
    r = (GdipDrawLine(g, p, x1, y1, x2, y2) == Ok);
    GdipDeletePen(p);
    return r;
}

int gx_stroke_rect(GpGraphics *g, ARGB c, REAL pw, REAL x, REAL y, REAL w, REAL h)
{
    GpPen *p = gx_pen(c, pw, LineCapSquare, LineJoinMiter);
    int r;
    if (!p) return 0;
    r = (GdipDrawRectangle(g, p, x, y, w, h) == Ok);
    GdipDeletePen(p);
    return r;
}

int gx_stroke_ellipse(GpGraphics *g, ARGB c, REAL pw, REAL x, REAL y, REAL w, REAL h)
{
    GpPen *p = gx_pen(c, pw, LineCapSquare, LineJoinMiter);
    int r;
    if (!p) return 0;
    r = (GdipDrawEllipse(g, p, x, y, w, h) == Ok);
    GdipDeletePen(p);
    return r;
}

int gx_stroke_dashed_rect(GpGraphics *g, ARGB c, REAL pw, REAL x, REAL y, REAL w, REAL h)
{
    GpPen *p = gx_pen(c, pw, LineCapSquare, LineJoinMiter);
    int r;
    if (!p) return 0;
    GdipSetPenDashStyle(p, DashStyleDash);
    r = (GdipDrawRectangle(g, p, x, y, w, h) == Ok);
    GdipDeletePen(p);
    return r;
}

int gx_clear(GpGraphics *g, ARGB c, REAL w, REAL h)
{
    GpSolidFill *b = NULL;
    int r = 0;
    if (GdipCreateSolidFill(c, &b) != Ok) return 0;
    GdipSetCompositingMode(g, CompositingModeSourceCopy);
    r = (GdipFillRectangle(g, b, 0, 0, w, h) == Ok);
    GdipSetCompositingMode(g, CompositingModeSourceOver);
    GdipDeleteBrush((GpBrush *)b);
    return r;
}

GpFont *gx_font(GpFontFamily *fam, REAL px)
{
    GpFont *f = NULL;
    if (!fam || px < 1.0f) return NULL;
    if (GdipCreateFont(fam, px, FontStyleRegular, UnitPixel, &f) != Ok)
        return NULL;
    return f;
}

int gx_draw_text(GpGraphics *g, GpFont *font, ARGB c, const wchar_t *s,
                 REAL x, REAL y, REAL w, REAL h)
{
    GpSolidFill *b = NULL;
    GpRectF layout;
    int r = 0;
    if (!font || !s || !*s) return 0;
    if (GdipCreateSolidFill(c, &b) != Ok) return 0;
    layout.X = x; layout.Y = y; layout.Width = w; layout.Height = h;
    /* NULL string format = default near/near alignment */
    r = (GdipDrawString(g, s, -1, font, &layout, NULL, b) == Ok);
    GdipDeleteBrush((GpBrush *)b);
    return r;
}

/* GdipMeasureString needs a real Graphics; NULL makes it fail, and a layout
 * built on a 0-width measurement collapses every button. A 1x1 scratch surface
 * created once is enough and costs nothing per call.
 *
 * It has file scope, not function scope, for a reason: these objects belong to
 * the GDI+ instance that created them, so they must be released before
 * GdiplusShutdown. Left static inside the function they survive the shutdown and
 * the next GdiplusStartup inherits a dangling context - which is an access
 * violation the moment anything is measured again. gx_purge_cache() does the
 * releasing, and gdiplus_stop() calls it. */
static GpBitmap    *g_measureBmp = NULL;
static GpGraphics *g_measureCtx = NULL;
static int         g_measureTried = 0;

void gx_purge_cache(void)
{
    if (g_measureCtx) { GdipDeleteGraphics(g_measureCtx); g_measureCtx = NULL; }
    if (g_measureBmp) { GdipDisposeImage((GpImage *)g_measureBmp); g_measureBmp = NULL; }
    g_measureTried = 0;
}

int gx_measure_text(GpFont *font, const wchar_t *s, REAL *w, REAL *h)
{
    GpRectF layout, out;
    GpBitmap   *bmp = g_measureBmp;
    GpGraphics *mg  = g_measureCtx;

    if (!font || !s) return 0;
    if (!g_measureTried) {
        g_measureTried = 1;
        if (GdipCreateBitmapFromScan0(1, 1, 4, PixelFormat32bppARGB, NULL, &bmp) == Ok) {
            if (GdipGetImageGraphicsContext((GpImage *)bmp, &mg) != Ok) mg = NULL;
        }
        if (!mg && bmp) { GdipDisposeImage((GpImage *)bmp); bmp = NULL; }
        g_measureBmp = bmp;
        g_measureCtx = mg;
    }
    if (!mg) return 0;

    layout.X = layout.Y = 0;
    layout.Width = 20000; layout.Height = 2000;
    out.Width = out.Height = 0;
    if (GdipMeasureString(mg, s, -1, font, &layout, NULL, &out, NULL, NULL) != Ok)
        return 0;
    if (w) *w = out.Width;
    if (h) *h = out.Height;
    return 1;
}

void gx_set_quality(GpGraphics *g)
{
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHighQuality);
    GdipSetInterpolationMode(g, InterpolationModeHighQualityBicubic);
    GdipSetTextRenderingHint(g, TextRenderingHintAntiAliasGridFit);
    GdipSetCompositingQuality(g, CompositingQualityHighQuality);
}

void gx_draw_image_rect_i(GpGraphics *g, GpImage *img,
                          INT x, INT y, INT w, INT h, GpInterpolationMode mode)
{
    GdipSetInterpolationMode(g, mode);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    GdipDrawImageRectI(g, img, x, y, w, h);
    gx_set_quality(g);
}

void gx_round_rect_bg(GpGraphics *g, ARGB c, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    if (r * 2 > w) r = w / 2.0f;
    if (r * 2 > h) r = h / 2.0f;
    gx_fill_rect(g, c, x + r, y, w - r * 2.0f, h);
    gx_fill_rect(g, c, x, y + r, r, h - r * 2.0f);
    gx_fill_rect(g, c, x + w - r, y + r, r, h - r * 2.0f);
    gx_fill_ellipse(g, c, x, y, r * 2.0f, r * 2.0f);
    gx_fill_ellipse(g, c, x + w - r * 2.0f, y, r * 2.0f, r * 2.0f);
    gx_fill_ellipse(g, c, x, y + h - r * 2.0f, r * 2.0f, r * 2.0f);
    gx_fill_ellipse(g, c, x + w - r * 2.0f, y + h - r * 2.0f, r * 2.0f, r * 2.0f);
}
