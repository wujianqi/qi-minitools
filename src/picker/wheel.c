/* wheel.c - hue ring, S/V triangle and colour harmonies for the picker. */
#include "wheel.h"

/* ------------------------------------------------------------------ */
/* harmonies                                                            */
/* ------------------------------------------------------------------ */

#define OFF(a)      { (a), 1.0, 1.0 }
#define MUTE(a)     { (a), 0.42, 1.0 }
#define PALE(a)     { (a), 0.55, 1.0 }
#define DEEP(a)     { (a), 0.85, 0.62 }
#define SOFT(a)     { (a), 0.30, 0.55 }

static const WheelScheme SCHEMES[] = {
    { L"analogous", L"类似色",
      L"色相相邻 ±60°，同饱和度，安全好用",
      /* starts at the base hue: slot 0 is the colour on the wheel in every
       * scheme, so the row always leads with what the user just picked */
      { OFF(0), OFF(30), OFF(-30), OFF(60), OFF(-60) } },
    { L"complement", L"互补色",
      L"色相对立 180°，一强一弱最醒目",
      { OFF(0), OFF(180), PALE(180), DEEP(0), SOFT(180) } },
    { L"triadic", L"三角色",
      L"等分色轮 120°，活泼而均衡",
      { OFF(0), OFF(120), OFF(240), PALE(120), PALE(240) } },
    { L"split", L"分裂补",
      L"互补色两侧各 30°，比互补更柔和",
      { OFF(0), OFF(150), OFF(210), PALE(150), PALE(210) } },
    { L"tetradic", L"四方色",
      L"两组互补 90°，层次丰富",
      { OFF(0), OFF(90), OFF(180), OFF(270), PALE(270) } }
};

int wheel_scheme_count(void) { return (int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])); }

const WheelScheme *wheel_scheme_at(int i)
{
    if (i < 0 || i >= wheel_scheme_count()) return &SCHEMES[0];
    return &SCHEMES[i];
}

int wheel_scheme_index_of(const wchar_t *id)
{
    int i;
    for (i = 0; i < wheel_scheme_count(); i++)
        if (wcscmp(SCHEMES[i].id, id) == 0) return i;
    return 0;
}

void wheel_scheme_colors(const WheelScheme *s, Rgb base, Rgb *out)
{
    double h, sat, val;
    int i;
    if (!s || !out) return;
    rgb_to_hsv(base, &h, &sat, &val);
    for (i = 0; i < WHEEL_SLOTS; i++) {
        double hh = h + s->slot[i].hOff;
        double ss = sat * s->slot[i].sMul;
        double vv = val * s->slot[i].vMul;
        /* fmod of a negative would keep the sign; wrap by hand instead. */
        while (hh < 0.0) hh += 360.0;
        while (hh >= 360.0) hh -= 360.0;
        if (ss < 0.0) ss = 0.0; else if (ss > 1.0) ss = 1.0;
        if (vv < 0.0) vv = 0.0; else if (vv > 1.0) vv = 1.0;
        hsv_to_rgb(hh, ss, vv, &out[i]);
    }
}

/* ------------------------------------------------------------------ */
/* geometry                                                             */
/* ------------------------------------------------------------------ */

void wheel_geom(WheelGeom *g, int boxW, int boxH, int ringPx)
{
    double rOut = (boxW < boxH ? boxW : boxH) / 2.0;
    if (rOut < 4.0) rOut = 4.0;
    g->cx = boxW / 2.0;
    g->cy = boxH / 2.0;
    g->rOut = rOut - 1.0;                 /* keep the outer edge on-canvas */
    if (ringPx < 1) ringPx = 1;
    g->rIn = g->rOut - ringPx;
    if (g->rIn < 1.0) g->rIn = 1.0;
    /* Inscribed equilateral triangle: half-width r*cos(30), height r*1.5. */
    g->triHalf = g->rIn * 0.8660254;
    g->triTop  = g->cy - g->rIn;
    g->triBot  = g->cy + g->rIn * 0.5;
}

int wheel_ring_hue(const WheelGeom *g, double x, double y, double *hue)
{
    double dx = x - g->cx, dy = y - g->cy;
    double r = sqrt(dx * dx + dy * dy);
    double deg;
    if (r < g->rIn || r > g->rOut) return 0;
    /* atan2 measures from +x; shift by 90 so 0 lands at 12 o'clock. */
    deg = atan2(dx, -dy) * 180.0 / 3.14159265358979323846;
    if (deg < 0.0) deg += 360.0;
    if (hue) *hue = deg;
    return 1;
}

int wheel_hue_nearest(const WheelGeom *g, double x, double y, double *hue)
{
    double dx = x - g->cx, dy = y - g->cy;
    double r = sqrt(dx * dx + dy * dy);
    double deg;
    if (r < 0.5) return 0;              /* dead centre: no angle to report */
    deg = atan2(dx, -dy) * 180.0 / 3.14159265358979323846;
    if (deg < 0.0) deg += 360.0;
    if (hue) *hue = deg;
    return 1;
}

void wheel_ring_point(const WheelGeom *g, double hue, double *x, double *y)
{
    /* mid-band radius: the marker sits between the two edges. */
    double r = (g->rOut + g->rIn) / 2.0;
    double a = hue * 3.14159265358979323846 / 180.0;
    if (x) *x = g->cx + r * sin(a);
    if (y) *y = g->cy - r * cos(a);
}

int wheel_tri_sv(const WheelGeom *g, double x, double y, double *s, double *v)
{
    double hgt = g->triBot - g->triTop;
    double t, vv, half, ss;
    if (hgt <= 0.0) return 0;
    t = (y - g->triTop) / hgt;
    if (t < 0.0 || t > 1.0) return 0;
    vv = 1.0 - t;
    if (t <= 0.0) {                 /* the apex is a single point: white */
        if (s) *s = 0.0;
        if (v) *v = vv;
        return 1;
    }
    /* A cross-section t of the way down spans t*triHalf either side of the
     * axis: nothing at the apex, full width at the base. */
    half = g->triHalf * t;
    ss = (x - (g->cx - half)) / (2.0 * half);
    if (ss < 0.0) ss = 0.0; else if (ss > 1.0) ss = 1.0;
    if (s) *s = ss;
    if (v) *v = vv;
    return 1;
}

int wheel_tri_sv_loose(const WheelGeom *g, double x, double y, double *s, double *v)
{
    double hgt = g->triBot - g->triTop;
    double t, vv, half, ss;
    if (hgt <= 0.0) return 0;
    t = (y - g->triTop) / hgt;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    vv = 1.0 - t;
    half = g->triHalf * t;
    if (half <= 0.0001) {
        /* Past the apex there is no width left to read a saturation from;
         * report white, which is what that end of the gradient is. */
        ss = 0.0;
    } else {
        ss = (x - (g->cx - half)) / (2.0 * half);
        if (ss < 0.0) ss = 0.0; else if (ss > 1.0) ss = 1.0;
    }
    if (s) *s = ss;
    if (v) *v = vv;
    return 1;
}

void wheel_tri_point(const WheelGeom *g, double s, double v, double *x, double *y)
{
    double half;
    if (s < 0.0) s = 0.0; else if (s > 1.0) s = 1.0;
    if (v < 0.0) v = 0.0; else if (v > 1.0) v = 1.0;
    half = g->triHalf * (1.0 - v);
    if (x) *x = g->cx + (2.0 * s - 1.0) * half;
    if (y) *y = g->triTop + (1.0 - v) * (g->triBot - g->triTop);
}

/* ------------------------------------------------------------------ */
/* raster                                                               */
/* ------------------------------------------------------------------ */

#define SS 2                     /* supersampling factor, per axis */

static void put_px(void *bits, int stride, int x, int y,
                   int r, int g, int b)
{
    BGRAPX *row;
    if (x < 0 || y < 0) return;
    row = (BGRAPX *)((BYTE *)bits + (size_t)stride * (size_t)y);
    row[x].b = (BYTE)b;
    row[x].g = (BYTE)g;
    row[x].r = (BYTE)r;
    row[x].a = 255;
}

void wheel_render_ring(void *bits, int stride, int w, int h, const WheelGeom *g)
{
    int x, y, i, j, r, gg, b, n;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            r = gg = b = 0; n = 0;
            for (j = 0; j < SS; j++) {
                for (i = 0; i < SS; i++) {
                    double px = x + (i + 0.5) / SS;
                    double py = y + (j + 0.5) / SS;
                    double hue;
                    Rgb c;
                    if (!wheel_ring_hue(g, px, py, &hue)) continue;
                    hsv_to_rgb(hue, 1.0, 1.0, &c);
                    r += c.r; gg += c.g; b += c.b; n++;
                }
            }
            if (n) put_px(bits, stride, x, y, r / n, gg / n, b / n);
        }
    }
}

void wheel_render_tri(void *bits, int stride, int w, int h,
                      const WheelGeom *g, double hue)
{
    int x, y, i, j, r, gg, b, n;
    double hgt = g->triBot - g->triTop;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            r = gg = b = 0; n = 0;
            for (j = 0; j < SS; j++) {
                for (i = 0; i < SS; i++) {
                    double px = x + (i + 0.5) / SS;
                    double py = y + (j + 0.5) / SS;
                    double t = (hgt > 0.0) ? (py - g->triTop) / hgt : -1.0;
                    double half = g->triHalf * t;
                    double dx = px - g->cx, s, v;
                    Rgb c;
                    /* Containment has to be tested here, not through
                     * wheel_tri_sv: that one clamps saturation instead of
                     * rejecting, which is right for a drag and wrong for a
                     * fill - it would paint the triangle's bounding box as a
                     * solid rectangle and bury the ring behind it. */
                    if (t < 0.0 || t > 1.0) continue;
                    if (dx < -half || dx > half) continue;
                    s = (dx + half) / (2.0 * half);
                    if (s < 0.0) s = 0.0; else if (s > 1.0) s = 1.0;
                    v = 1.0 - t;
                    hsv_to_rgb(hue, s, v, &c);
                    r += c.r; gg += c.g; b += c.b; n++;
                }
            }
            if (n) put_px(bits, stride, x, y, r / n, gg / n, b / n);
        }
    }
}
