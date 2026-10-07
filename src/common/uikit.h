/* uikit.h - small GDI+ drawing helpers shared by the tools.
 *
 * The MinGW C headers shipped with zig take Graphics* as the first argument to
 * every draw/fill call and have no GpColor type (colours are plain ARGB), so
 * all drawing funnels through these wrappers to keep call sites readable. */
#ifndef UIKIT_H
#define UIKIT_H

#include "winutil.h"
#include <gdiplus.h>

#ifdef __cplusplus
extern "C" {
#endif

GpPen  *gx_pen(ARGB c, REAL w, GpLineCap cap, GpLineJoin join);
int     gx_fill_rect(GpGraphics *g, ARGB c, REAL x, REAL y, REAL w, REAL h);
int     gx_fill_ellipse(GpGraphics *g, ARGB c, REAL x, REAL y, REAL w, REAL h);
int     gx_fill_poly(GpGraphics *g, ARGB c, const GpPointF *p, int n);
int     gx_fill_path(GpGraphics *g, ARGB c, GpPath *path, GpFillMode mode);
int     gx_stroke_rect(GpGraphics *g, ARGB c, REAL pw, REAL x, REAL y, REAL w, REAL h);
int     gx_stroke_ellipse(GpGraphics *g, ARGB c, REAL pw, REAL x, REAL y, REAL w, REAL h);
int     gx_stroke_line(GpGraphics *g, ARGB c, REAL pw, REAL x1, REAL y1, REAL x2, REAL y2);
int     gx_stroke_line_f(GpGraphics *g, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2);
int     gx_stroke_dashed_rect(GpGraphics *g, ARGB c, REAL pw, REAL x, REAL y, REAL w, REAL h);
int     gx_clear(GpGraphics *g, ARGB c, REAL w, REAL h);
int     gx_draw_text(GpGraphics *g, GpFont *font, ARGB c, const wchar_t *s,
                     REAL x, REAL y, REAL w, REAL h);
int     gx_measure_text(GpFont *font, const wchar_t *s, REAL *w, REAL *h);
GpFont *gx_font(GpFontFamily *fam, REAL px);
void    gx_set_quality(GpGraphics *g);
void    gx_draw_image_rect_i(GpGraphics *g, GpImage *img,
                             INT x, INT y, INT w, INT h, GpInterpolationMode mode);
/* Rounded rectangle background: centre rect plus four corner discs. */
void    gx_round_rect_bg(GpGraphics *g, ARGB c, REAL x, REAL y, REAL w, REAL h, REAL r);
/* Releases the scratch surface gx_measure_text() caches. Must run before
 * GdiplusShutdown, which gdiplus_stop() takes care of; anything still holding
 * those objects across a restart would be pointing at freed GDI+. */
void    gx_purge_cache(void);

#ifdef __cplusplus
}
#endif
#endif /* UIKIT_H */
