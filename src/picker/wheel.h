/* wheel.h - the interactive colour wheel: hue ring, S/V triangle, harmonies.
 *
 * Everything here is plain arithmetic on doubles with no GDI+ and no window,
 * so the whole widget can be exercised by the self-test without a graphics
 * context. picker.c owns the cached pixels and the mouse handling.
 */
#ifndef PICKER_WHEEL_H
#define PICKER_WHEEL_H

#include "color.h"
#include "../common/winutil.h"

#define WHEEL_SLOTS 5     /* colours produced by every harmony */

/* A harmony slot: a hue rotation applied to the base colour, plus multipliers
 * on its saturation and value. Keeping the "extra" members as multipliers of
 * the picked colour (instead of hard-coded tints) is what makes the row track
 * whatever is on the wheel right now. */
typedef struct {
    double hOff;     /* degrees, added to the base hue          */
    double sMul;     /* saturation multiplier, 0..1             */
    double vMul;     /* value multiplier, 0..1                  */
} WheelSlot;

typedef struct {
    const wchar_t *id;
    const wchar_t *label;                  /* tab text              */
    const wchar_t *hint;                   /* tooltip               */
    WheelSlot     slot[WHEEL_SLOTS];
} WheelScheme;

int                 wheel_scheme_count(void);
const WheelScheme  *wheel_scheme_at(int i);
int                 wheel_scheme_index_of(const wchar_t *id);
/* Fills out[0..WHEEL_SLOTS-1] from `base`; slot 0 is always `base` itself. */
void                wheel_scheme_colors(const WheelScheme *s, Rgb base, Rgb *out);

/* ------------------------------------------------------------------ */
/* geometry                                                             */
/* ------------------------------------------------------------------ */

/* The ring is a full circle of hues with the S/V triangle inscribed in the
 * hole. Coordinates are relative to the wheel box's top-left corner. */
typedef struct {
    double cx, cy;      /* ring centre                */
    double rOut, rIn;   /* outer / inner ring radius  */
    double triHalf;     /* triangle half-width        */
    double triTop;      /* triangle apex y            */
    double triBot;      /* triangle base y            */
} WheelGeom;

void wheel_geom(WheelGeom *g, int boxW, int boxH, int ringPx);

/* Ring: hue in degrees 0..360, 0 at 12 o'clock, growing clockwise (red at the
 * top). Returns 0 when the point is not in the ring band. */
int  wheel_ring_hue(const WheelGeom *g, double x, double y, double *hue);
/* Inverse of the above, for drawing the marker. */
void wheel_ring_point(const WheelGeom *g, double hue, double *x, double *y);

/* Dragging variants: the pointer keeps reporting an angle as it slides off
 * the band, and keeps reporting a saturation as it slides off the triangle,
 * which is what makes a drag feel continuous instead of freezing at the edge.
 * Both return 0 only where the geometry has no answer at all. */
int  wheel_hue_nearest(const WheelGeom *g, double x, double y, double *hue);
int  wheel_tri_sv_loose(const WheelGeom *g, double x, double y, double *s, double *v);

/* Triangle: the classic HSV triangle. The apex is white (S=0,V=1), the whole
 * bottom edge is black (V=0), the left edge runs white to black (S=0) and the
 * right edge is the pure hue (S=1). Returns 0 when the point is outside. */
int  wheel_tri_sv(const WheelGeom *g, double x, double y, double *s, double *v);
void wheel_tri_point(const WheelGeom *g, double s, double v, double *x, double *y);

/* ------------------------------------------------------------------ */
/* raster                                                               */
/* ------------------------------------------------------------------ */

/* Both render straight into a 32bpp BGRA buffer (the format make_dib hands
 * out) rather than issuing thousands of GDI+ fills: a wheel is ~25k pixels and
 * the ring would otherwise cost one polygon fill per degree on every repaint.
 * 2x2 supersampling keeps the circle edge from looking like a staircase. */
void wheel_render_ring(void *bits, int stride, int w, int h, const WheelGeom *g);
void wheel_render_tri(void *bits, int stride, int w, int h,
                      const WheelGeom *g, double hue);

#endif /* PICKER_WHEEL_H */
