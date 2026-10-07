/* picker.c - main window: current colour, format list, palette, tray, hotkey. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "eyedrop.h"
#include "wheel.h"
#include "../common/trayicon.h"
#include <commctrl.h>
#include <commdlg.h>

#define PKG_CLASS L"MuskPickerMain"
#define HOTKEY_ID 1

/* This program's fill for logo.svg: the same fuchsia res\logo-picker.ico is
 * filled with, so the tray icon and the .exe icon can never disagree. */
#define TRAY_ICON_COLOR APP_ICON_COLOR_PICKER

/* palette geometry, in logical px before DPI scaling */
#define SW_CELL_W   22
#define SW_CELL_H   18
#define SW_GAP      3
#define SW_PER_ROW  9

/* colour wheel geometry, logical px */
#define WHEEL_BOX   144     /* square ring area          */
#define WHEEL_RING  14      /* ring band thickness      */
#define WHEEL_PANEL 148     /* height of the whole strip */
#define WHEEL_TABH  21
#define WHEEL_SW_H  44      /* harmony swatch height    */

typedef enum {
    ZEY_NONE = 0, ZEY_BIGSWATCH, ZEY_EYEDROP, ZEY_CUSTOM, ZEY_FAV,
    ZEY_FORMAT_TAB, ZEY_COPY_ROW, ZEY_PAL_TAB, ZEY_SWATCH, ZEY_COPY_ALL,
    ZEY_WHEEL_RING, ZEY_WHEEL_TRI, ZEY_SCHEME_TAB, ZEY_SCHEME_SW
} Zone;

/* Which part of the wheel a drag started on. The drag keeps this mode for its
 * whole life: sliding off the ring into the hole would otherwise flip the
 * colour over to whatever the pointer happened to cross. */
typedef enum { WDRAG_NONE = 0, WDRAG_RING, WDRAG_TRI } WheelDrag;

typedef struct {
    RECT  r;
    Zone  z;
    int   idx;        /* format row, palette id, or swatch index */
    Rgb   color;      /* for swatches */
    const wchar_t *tip;
} Hit;

typedef struct Picker {
    HINSTANCE   inst;
    HWND        hwnd;
    Tray        tray;
    int         quit;

    Rgb         cur;
    Rgb         lastPick;
    PaletteId   pal;
    int         hover;          /* index into hit[]  */
    int         pressed;
    int         hoverSwatch;    /* palette index under the cursor, or -1 */
    wchar_t     status[160];
    ULONGLONG   statusUntil;

    /* colour wheel ---------------------------------------------------- */
    double      wheelHue;       /* hue the triangle is drawn for; sticks
                                 * even while the current colour is grey  */
    WheelGeom   geom;
    WheelDrag   drag;
    int         scheme;         /* index into wheel_scheme_at()          */
    Rgb         schemeCol[WHEEL_SLOTS];
    /* The ring and the triangle are cached as real DIB sections and blitted,
     * because re-rasterising ~25k pixels on every mouse move would make
     * dragging the ring feel like dragging a brick. The ring only depends on
     * its size; the triangle only on the hue. */
    HBITMAP   ringBmp;
    void     *ringBits;
    GpBitmap *ringImg;
    int       ringW, ringH;
    int       ringDirty;
    HBITMAP   triBmp;
    void     *triBits;
    GpBitmap *triImg;
    int       triW, triH;
    double    triHue;
    int       triHueValid;

    int         swScroll;       /* first visible swatch row              */

    GpFontFamily *fam, *mono;
    /* Back buffer: a real DIB section that GDI+ draws into, so the same
     * bitmap can be selected into a memory DC for the final BitBlt. */
    HBITMAP   memBmp;
    void     *memBits;
    GpBitmap *mem;           /* GDI+ view over memBits          */
    int       memW, memH;

    Hit        *hit;
    int         nhit, chit;

    wchar_t    *values[CF_COUNT];   /* cached formatted strings */
    wchar_t     tipBuf[96];         /* tooltip for the hovered swatch */
} Picker;

static Picker g_pk;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static ARGB rgb_to_argb(Rgb c)
{
    return 0xFF000000u | ((ARGB)c.r << 16) | ((ARGB)c.g << 8) | c.b;
}

static void refresh_values(void)
{
    int i;
    for (i = 0; i < CF_COUNT; i++) {
        xfree(g_pk.values[i]);
        g_pk.values[i] = color_format_value((ColorFormat)i, g_pk.cur);
    }
    wheel_scheme_colors(wheel_scheme_at(g_pk.scheme), g_pk.cur, g_pk.schemeCol);
}

static void set_color(Rgb c, const wchar_t *note)
{
    double h, s, v;
    g_pk.cur = c;
    /* Keep the wheel on the hue the user can actually see. A grey has no hue
     * of its own, so hold the last one instead of snapping the triangle back
     * to red every time an eyedrop lands on a shadow. */
    rgb_to_hsv(c, &h, &s, &v);
    if (s > 0.01) g_pk.wheelHue = h;
    refresh_values();
    if (note) {
        wcsncpy(g_pk.status, note, 159);
        g_pk.status[159] = 0;
    }
    InvalidateRect(g_pk.hwnd, NULL, FALSE);
}

static void set_status(const wchar_t *s)
{
    wcsncpy(g_pk.status, s, 159);
    g_pk.status[159] = 0;
    g_pk.statusUntil = GetTickCount64() + 2500;
    InvalidateRect(g_pk.hwnd, NULL, FALSE);
}

static int hit_add(RECT r, Zone z, int idx, Rgb c, const wchar_t *tip)
{
    Hit *h;
    if (g_pk.nhit == g_pk.chit) {
        int cap = g_pk.chit ? g_pk.chit * 2 : 64;
        Hit *nh = (Hit *)xrealloc(g_pk.hit, (size_t)cap * sizeof(Hit));
        if (!nh) return -1;
        g_pk.hit = nh;
        g_pk.chit = cap;
    }
    h = &g_pk.hit[g_pk.nhit++];
    h->r = r;
    h->z = z;
    h->idx = idx;
    h->color = c;
    h->tip = tip;
    return g_pk.nhit - 1;
}

static int hit_test(int x, int y)
{
    int i;
    POINT p;
    p.x = x; p.y = y;
    for (i = g_pk.nhit - 1; i >= 0; i--) {
        if (PtInRect(&g_pk.hit[i].r, p)) return i;
    }
    return -1;
}

/* Index of the single hit registered for a given zone, or -1. */
static int hit_zone(Zone z)
{
    int i;
    for (i = 0; i < g_pk.nhit; i++)
        if (g_pk.hit[i].z == z) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    RECT big, eyedrop, custom, fav;
    RECT swatchNote;                         /* RGB + nearest-name readouts */
    RECT wheel;                              /* the square ring area      */
    RECT schemeTab[8];
    RECT schemeSw[WHEEL_SLOTS];
    RECT wheelInfo;                          /* H/S/V readout             */
    RECT fmtTabs[CF_COUNT];
    RECT fmtRow[CF_COUNT], fmtCopy[CF_COUNT];
    RECT copyAll;
    RECT palNote;
    RECT palTab[PAL_COUNT];
    int  palTabRows;
    RECT swatch[512];
    int  nswatch;
    int  swPerRow, swRows, swMaxScroll;
    RECT swBar;                            /* scroll indicator, if any    */
} Layout;

static Layout g_lay;

static void build_layout(int W, int H)
{
    int S = app_scale_px(1);
    int x, y, i, r, c;
    int pad = S * 10;
    int bigH = S * 62;
    int rowH = S * 22;
    int gap  = S * 6;
    int tabH = S * 22;
    int box  = S * WHEEL_BOX;
    int panelH = S * WHEEL_PANEL;
    int bottom = H - S * 24;   /* status line lives below this */
    int usable, colGap, lw, rw, lx, rx, bodyTop, bodyBot;

    memset(&g_lay, 0, sizeof(g_lay));
    g_lay.nswatch = 0;

    /* header: big swatch + eyedropper + custom + favourite */
    SetRect(&g_lay.big, pad, pad, pad + S * 110, pad + bigH);
    x = g_lay.big.right + gap;
    y = pad;
    SetRect(&g_lay.eyedrop, x, y, W - pad, y + S * 30);
    y += S * 30 + gap;
    SetRect(&g_lay.custom, x, y, x + (W - pad - x) / 2 - gap / 2, y + S * 26);
    SetRect(&g_lay.fav, g_lay.custom.right + gap, y, W - pad, y + S * 26);

    /* The readout band under the swatch belongs to the two text lines drawn
     * there - the RGB values and the nearest colour name. The wheel strip
     * used to start 6px under the swatch, on top of this band, and the wheel's
     * opaque backdrop wiped the name line outright and clipped the RGB one,
     * which read as the ring hiding useful text. The band is reserved here
     * and the wheel starts below it; the self-test pins the order. */
    SetRect(&g_lay.swatchNote, pad, pad + bigH + S * 2, pad + S * 110,
            pad + bigH + S * 2 + S * 32);
    y = g_lay.swatchNote.bottom + gap;

    /* colour wheel strip: ring on the left, harmonies on the right */
    SetRect(&g_lay.wheel, pad, y, pad + box, y + box);
    {
        int sx = g_lay.wheel.right + gap;
        int sw = W - pad - sx;
        int n = wheel_scheme_count();
        if (n > (int)(sizeof(g_lay.schemeTab) / sizeof(g_lay.schemeTab[0])))
            n = (int)(sizeof(g_lay.schemeTab) / sizeof(g_lay.schemeTab[0]));
        for (i = 0; i < n; i++) {
            REAL tw = 0;
            GpFont *f = gx_font(g_pk.fam, (REAL)(S * 11));
            if (f) {
                gx_measure_text(f, wheel_scheme_at(i)->label, &tw, NULL);
                GdipDeleteFont(f);
            }
            if (tw < (REAL)(S * 28)) tw = (REAL)(S * 28);
            SetRect(&g_lay.schemeTab[i], sx, y,
                    sx + (int)lround(tw) + S * 8, y + S * WHEEL_TABH);
            sx += (int)lround(tw) + S * 8;
        }
        /* harmony swatches share whatever width the tabs left over */
        {
            int avail = W - pad - (g_lay.wheel.right + gap);
            int cellW = (avail - S * 4) / WHEEL_SLOTS;
            if (cellW < S * 20) cellW = S * 20;
            for (i = 0; i < WHEEL_SLOTS; i++) {
                int cx = g_lay.wheel.right + gap + i * (cellW + S * 4);
                SetRect(&g_lay.schemeSw[i], cx, y + S * WHEEL_TABH + S * 5,
                        cx + cellW, y + S * WHEEL_TABH + S * 5 + S * WHEEL_SW_H);
            }
        }
        SetRect(&g_lay.wheelInfo, g_lay.wheel.right + gap,
                y + S * WHEEL_TABH + S * 9 + S * WHEEL_SW_H, W - pad,
                y + S * WHEEL_TABH + S * 9 + S * WHEEL_SW_H + S * 14);
    }
    y += panelH + gap;

    /* Two columns below the wheel. A single column cannot hold the wheel, all
     * eleven format rows and a swatch grid on a 125% display - the window is
     * clamped to the work area long before that stack fits - so the formats and
     * the palettes sit side by side instead. */
    usable = W - pad * 2;
    if (usable < S * 240) usable = S * 240;
    colGap = S * 8;
    lw = usable * 57 / 100;
    rw = usable - lw - colGap;
    lx = pad;
    rx = lx + lw + colGap;
    bodyTop = y;
    bodyBot = bottom;

    /* ---- left column: the colour formats ---------------------------- */
    x = lx;
    y = bodyTop;
    for (i = 0; i < CF_COUNT; i++) {
        REAL tw = 0;
        GpFont *f = gx_font(g_pk.fam, (REAL)(S * 10));
        if (f) { gx_measure_text(f, color_format_label((ColorFormat)i), &tw, NULL); GdipDeleteFont(f); }
        if (tw < (REAL)(S * 30)) tw = (REAL)(S * 30);
        if (x > lx && x + (int)lround(tw) + S * 6 > lx + lw) {
            x = lx;
            y += tabH + S * 3;
        }
        SetRect(&g_lay.fmtTabs[i], x, y, x + (int)lround(tw) + S * 6, y + tabH);
        x += (int)lround(tw) + S * 6;
    }
    y += tabH + gap;

    for (i = 0; i < CF_COUNT; i++) {
        SetRect(&g_lay.fmtRow[i], lx, y, lx + lw - S * 40 - gap, y + rowH);
        SetRect(&g_lay.fmtCopy[i], lx + lw - S * 38, y, lx + lw, y + rowH);
        y += rowH + S * 2;
    }
    {
        GpFont *f = gx_font(g_pk.fam, (REAL)(S * 11));
        REAL tw = 0;
        if (f) {
            gx_measure_text(f, L"复制全部  Ctrl+Shift+C", &tw, NULL);
            GdipDeleteFont(f);
        }
        if (tw > (REAL)(lw - S * 20)) tw = (REAL)(lw - S * 20);
        SetRect(&g_lay.copyAll, lx, y + S * 4, lx + (int)lround(tw) + S * 16,
                y + S * 4 + S * 24);
    }

    /* ---- right column: palettes, the theme rule, the swatch grid ---- */
    y = bodyTop;
    if (palette_note(g_pk.pal)) {
        /* the rule text is long, so give it two lines rather than clipping */
        SetRect(&g_lay.palNote, rx, y, rx + rw, y + S * 26);
        y += S * 26 + gap;
    }
    {
        int rowTop = y;
        x = rx;
        g_lay.palTabRows = 1;
        for (i = 0; i < PAL_COUNT; i++) {
            const wchar_t *lbl = palette_get((PaletteId)i)->label;
            REAL tw = 0;
            GpFont *f = gx_font(g_pk.fam, (REAL)(S * 10));
            if (f) { gx_measure_text(f, lbl, &tw, NULL); GdipDeleteFont(f); }
            if (tw < (REAL)(S * 28)) tw = (REAL)(S * 28);
            if (x > rx && x + (int)lround(tw) + S * 6 > rx + rw) {
                x = rx;
                rowTop += tabH + S * 3;
                g_lay.palTabRows++;
            }
            SetRect(&g_lay.palTab[i], x, rowTop,
                    x + (int)lround(tw) + S * 6, rowTop + tabH);
            x += (int)lround(tw) + S * 6;
        }
        y = rowTop + tabH + gap;
    }
    {
        int cellW = S * SW_CELL_W, cellH = S * SW_CELL_H, g = S * SW_GAP;
        const Palette *pal = palette_get(g_pk.pal);
        int count = pal->count;
        int perRow = (rw + g) / (cellW + g);
        int rows, visible, maxScroll;
        if (perRow < 2) perRow = 2;
        rows = count > 0 ? (count + perRow - 1) / perRow : 0;
        visible = (bodyBot - y) / (cellH + g);
        if (visible < 0) visible = 0;
        if (rows > visible && visible > 0) {
            int a2 = rw - S * 9;
            int p2 = (a2 + g) / (cellW + g);
            if (p2 < 2) p2 = 2;
            perRow = p2;
            rows = count > 0 ? (count + perRow - 1) / perRow : 0;
        }
        maxScroll = rows - visible;
        if (maxScroll < 0) maxScroll = 0;
        if (g_pk.swScroll > maxScroll) g_pk.swScroll = maxScroll;
        if (g_pk.swScroll < 0) g_pk.swScroll = 0;
        g_lay.swPerRow = perRow;
        g_lay.swRows = rows;
        g_lay.swMaxScroll = maxScroll;
        for (r = 0; r < visible && g_lay.nswatch < 512; r++) {
            for (c = 0; c < perRow; c++) {
                int idx = (g_pk.swScroll + r) * perRow + c;
                int sx, sy;
                if (idx >= count) break;
                sx = rx + c * (cellW + g);
                sy = y + r * (cellH + g);
                SetRect(&g_lay.swatch[g_lay.nswatch], sx, sy, sx + cellW, sy + cellH);
                g_lay.nswatch++;
            }
        }
        if (maxScroll > 0 && rows > 0 && g_lay.nswatch > 0) {
            int barX = rx + rw - S * 6;
            int barH = bodyBot - y;
            int thumb = barH * visible / rows;
            int travel = barH - thumb;
            int pos = maxScroll ? g_pk.swScroll * travel / maxScroll : 0;
            if (thumb < S * 16) thumb = S * 16;
            if (thumb > barH) thumb = barH;
            SetRect(&g_lay.swBar, barX, y + pos, barX + S * 6, y + pos + thumb);
        }
    }
}

static void build_hits(void)
{
    int i;
    const Palette *pal = palette_get(g_pk.pal);
    g_pk.nhit = 0;
    hit_add(g_lay.big, ZEY_BIGSWATCH, 0, g_pk.cur, L"点击选择自定义颜色");
    hit_add(g_lay.eyedrop, ZEY_EYEDROP, 0, g_pk.cur, L"从屏幕任意位置取色  (D)");
    hit_add(g_lay.custom, ZEY_CUSTOM, 0, g_pk.cur, L"打开系统颜色对话框");
    hit_add(g_lay.fav, ZEY_FAV, 0, g_pk.cur, L"加入我的收藏  (F)");
    hit_add(g_lay.wheel, ZEY_WHEEL_RING, 0, g_pk.cur,
            L"拖动色环选色相 · 拖动三角选饱和与明度");
    for (i = 0; i < CF_COUNT; i++)
        hit_add(g_lay.fmtTabs[i], ZEY_FORMAT_TAB, i, g_pk.cur, color_format_hint((ColorFormat)i));
    for (i = 0; i < CF_COUNT; i++) {
        hit_add(g_lay.fmtRow[i], ZEY_COPY_ROW, i, g_pk.cur, L"点击复制该格式");
        hit_add(g_lay.fmtCopy[i], ZEY_COPY_ROW, i, g_pk.cur, L"复制");
    }
    hit_add(g_lay.copyAll, ZEY_COPY_ALL, 0, g_pk.cur, L"一次性复制全部格式  (Ctrl+Shift+C)");
    for (i = 0; i < wheel_scheme_count(); i++)
        hit_add(g_lay.schemeTab[i], ZEY_SCHEME_TAB, i, g_pk.cur,
                wheel_scheme_at(i)->hint);
    for (i = 0; i < WHEEL_SLOTS; i++)
        hit_add(g_lay.schemeSw[i], ZEY_SCHEME_SW, i, g_pk.schemeCol[i], NULL);
    for (i = 0; i < PAL_COUNT; i++) {
        const wchar_t *note = palette_note((PaletteId)i);
        hit_add(g_lay.palTab[i], ZEY_PAL_TAB, i, g_pk.cur,
                note ? note : palette_get((PaletteId)i)->label);
    }
    for (i = 0; i < g_lay.nswatch; i++) {
        int si = (g_pk.swScroll * g_lay.swPerRow) + i;
        Rgb c = (si < pal->count) ? pal->items[si].c : g_pk.cur;
        /* The tooltip for the hovered swatch is built into tipBuf once per
         * paint, so 145 theme swatches cost no allocations per frame. */
        if (si == g_pk.hoverSwatch && g_pk.tipBuf[0]) {
            g_pk.hit[g_pk.nhit].r = g_lay.swatch[i];
            g_pk.hit[g_pk.nhit].z = ZEY_SWATCH;
            g_pk.hit[g_pk.nhit].idx = si;
            g_pk.hit[g_pk.nhit].color = c;
            g_pk.hit[g_pk.nhit].tip = g_pk.tipBuf;
            g_pk.nhit++;
        } else {
            hit_add(g_lay.swatch[i], ZEY_SWATCH, si, c, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* wheel caches                                                         */
/* ------------------------------------------------------------------ */

static void wheel_cache_release(void)
{
    if (g_pk.ringImg) { GdipDisposeImage((GpImage *)g_pk.ringImg); g_pk.ringImg = NULL; }
    if (g_pk.ringBmp) { free_dib(g_pk.ringBmp, g_pk.ringBits); g_pk.ringBmp = NULL; }
    g_pk.ringBits = NULL; g_pk.ringW = g_pk.ringH = 0; g_pk.ringDirty = 1;
    if (g_pk.triImg) { GdipDisposeImage((GpImage *)g_pk.triImg); g_pk.triImg = NULL; }
    if (g_pk.triBmp) { free_dib(g_pk.triBmp, g_pk.triBits); g_pk.triBmp = NULL; }
    g_pk.triBits = NULL; g_pk.triW = g_pk.triH = 0; g_pk.triHueValid = 0;
}

static HBITMAP wheel_make(int w, int h, void **bits, GpBitmap **img)
{
    HBITMAP bmp = make_dib(w, h, 32, bits);
    *img = NULL;
    if (bmp)
        GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  *bits, img);
    if (!bmp || !*img) {
        if (bmp) { free_dib(bmp, *bits); *bits = NULL; }
        return NULL;
    }
    return bmp;
}

/* Recreates the ring/triangle surfaces when the layout changed and re-renders
 * whichever of them is stale. The triangle is redrawn only when the hue really
 * moved, so dragging inside it costs nothing. */
static void wheel_sync(int S)
{
    int w = g_lay.wheel.right - g_lay.wheel.left;
    int h = g_lay.wheel.bottom - g_lay.wheel.top;
    int stride;

    if (w < 8 || h < 8) return;
    if (!g_pk.ringBmp || g_pk.ringW != w || g_pk.ringH != h) {
        if (g_pk.ringImg) { GdipDisposeImage((GpImage *)g_pk.ringImg); g_pk.ringImg = NULL; }
        if (g_pk.ringBmp) { free_dib(g_pk.ringBmp, g_pk.ringBits); g_pk.ringBmp = NULL; }
        g_pk.ringBits = NULL;
        g_pk.ringBmp = wheel_make(w, h, &g_pk.ringBits, &g_pk.ringImg);
        if (!g_pk.ringBmp) return;
        g_pk.ringW = w; g_pk.ringH = h;
        g_pk.ringDirty = 1;
        /* the triangle lives in the same box, so it has to be rebuilt too */
        if (g_pk.triImg) { GdipDisposeImage((GpImage *)g_pk.triImg); g_pk.triImg = NULL; }
        if (g_pk.triBmp) { free_dib(g_pk.triBmp, g_pk.triBits); g_pk.triBmp = NULL; }
        g_pk.triBits = NULL;
        g_pk.triBmp = wheel_make(w, h, &g_pk.triBits, &g_pk.triImg);
        if (!g_pk.triBmp) return;
        g_pk.triW = w; g_pk.triH = h;
        g_pk.triHueValid = 0;
    }
    wheel_geom(&g_pk.geom, w, h, S * WHEEL_RING);
    stride = w * 4;
    if (g_pk.ringDirty) {
        wheel_render_ring(g_pk.ringBits, stride, w, h, &g_pk.geom);
        g_pk.ringDirty = 0;
    }
    if (g_pk.triBmp &&
        (!g_pk.triHueValid || fabs(g_pk.wheelHue - g_pk.triHue) > 0.05)) {
        wheel_render_tri(g_pk.triBits, stride, w, h, &g_pk.geom, g_pk.wheelHue);
        g_pk.triHue = g_pk.wheelHue;
        g_pk.triHueValid = 1;
    }
}

/* The ring and the triangle overlap in the hit list, so a point can match
 * either. Ring first: the band and the triangle never actually intersect, but
 * the hole's middle does, and there the triangle is the only sane answer. */
static int wheel_hit(double x, double y, int *isRing)
{
    double rel_x = x - g_lay.wheel.left;
    double rel_y = y - g_lay.wheel.top;
    if (rel_x < 0 || rel_y < 0) return 0;
    if (wheel_ring_hue(&g_pk.geom, rel_x, rel_y, NULL)) { *isRing = 1; return 1; }
    if (wheel_tri_sv(&g_pk.geom, rel_x, rel_y, NULL, NULL)) { *isRing = 0; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static void draw_buttons(GpGraphics *g, RECT r, const wchar_t *label,
                         int hot, int down, int primary, int S)
{
    ARGB bg = primary ? 0xFF0A84FF : (down ? 0xFFE5E5EA : (hot ? 0xFFF2F2F5 : 0xFFEFEFF2));
    ARGB fg = primary ? 0xFFFFFFFF : 0xFF1C1C1E;
    GpFont *f;
    gx_round_rect_bg(g, bg, (REAL)r.left, (REAL)r.top,
                     (REAL)(r.right - r.left), (REAL)(r.bottom - r.top),
                     (REAL)(S * 7));
    f = gx_font(g_pk.fam, (REAL)(S * 12));
    if (f) {
        gx_draw_text(g, f, fg, label, (REAL)r.left, (REAL)(r.top + S * 4),
                     (REAL)(r.right - r.left), (REAL)(r.bottom - r.top - S * 6));
        GdipDeleteFont(f);
    }
}

/* Draws the cached ring/triangle surfaces and the two position markers. */
static void paint_wheel(GpGraphics *g, int S)
{
    RECT b = g_lay.wheel;
    int w = b.right - b.left, h = b.bottom - b.top;
    double hx, hy, tx, ty, sat, val;

    /* No backdrop fill here on purpose. This used to paint the whole box
     * opaque white before the bitmaps went down, and anything drawn earlier
     * that reached into the box silently vanished under it - which is how
     * the readouts under the swatch ended up "behind the ring". The canvas
     * is cleared at the top of paint() and both cached bitmaps keep their
     * own alpha for the hole and the corners, so there is nothing to erase. */
    if (g_pk.ringImg)
        gx_draw_image_rect_i(g, (GpImage *)g_pk.ringImg, b.left, b.top, w, h,
                             InterpolationModeNearestNeighbor);
    if (g_pk.triImg)
        gx_draw_image_rect_i(g, (GpImage *)g_pk.triImg, b.left, b.top, w, h,
                             InterpolationModeNearestNeighbor);

    /* hue marker on the ring */
    wheel_ring_point(&g_pk.geom, g_pk.wheelHue, &hx, &hy);
    {
        REAL cx = (REAL)(b.left + hx), cy = (REAL)(b.top + hy);
        REAL d = (REAL)(S * 7);
        gx_fill_ellipse(g, 0xFFFFFFFF, cx - d, cy - d, d * 2, d * 2);
        gx_stroke_ellipse(g, 0xFF1C1C1E, (REAL)(S * 2), cx - d, cy - d, d * 2, d * 2);
    }
    /* saturation/value marker inside the triangle */
    {
        double hs, hv;
        Rgb c;
        rgb_to_hsv(g_pk.cur, &hs, &sat, &val);
        (void)hs;
        wheel_tri_point(&g_pk.geom, sat, val, &tx, &ty);
        {
            REAL cx = (REAL)(b.left + tx), cy = (REAL)(b.top + ty);
            REAL d = (REAL)(S * 6);
            hsv_to_rgb(g_pk.wheelHue, sat, val, &c);
            /* the marker sits on the gradient it points at, so it needs a
             * halo to stay visible against both black and white */
            gx_fill_ellipse(g, 0xFFFFFFFF, cx - d - (REAL)S, cy - d - (REAL)S,
                            d * 2 + (REAL)(S * 2), d * 2 + (REAL)(S * 2));
            gx_fill_ellipse(g, rgb_to_argb(c), cx - d, cy - d, d * 2, d * 2);
            gx_stroke_ellipse(g, 0xFF1C1C1E, (REAL)S, cx - d, cy - d, d * 2, d * 2);
        }
    }
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc, mem;
    HBITMAP old;
    GpGraphics *g = NULL;
    RECT cr;
    int S, i;
    const Palette *pal;

    S = app_scale_px(1);
    dc = BeginPaint(h, &ps);
    if (!dc) return;
    GetClientRect(h, &cr);
    mem = CreateCompatibleDC(dc);
    if (!mem) { EndPaint(h, &ps); return; }

    if (!g_pk.memBmp || g_pk.memW != cr.right || g_pk.memH != cr.bottom) {
        if (g_pk.mem) { GdipDisposeImage((GpImage *)g_pk.mem); g_pk.mem = NULL; }
        if (g_pk.memBmp) { free_dib(g_pk.memBmp, g_pk.memBits); g_pk.memBmp = NULL; }
        g_pk.memBits = NULL;
        g_pk.memBmp = make_dib(cr.right, cr.bottom, 32, &g_pk.memBits);
        if (g_pk.memBmp)
            GdipCreateBitmapFromScan0(cr.right, cr.bottom, cr.right * 4,
                                      PixelFormat32bppARGB, g_pk.memBits, &g_pk.mem);
        g_pk.memW = cr.right; g_pk.memH = cr.bottom;
    }
    if (!g_pk.memBmp || !g_pk.mem) {
        DeleteDC(mem); EndPaint(h, &ps); return;
    }
    old = SelectObject(mem, g_pk.memBmp);
    if (GdipGetImageGraphicsContext((GpImage *)g_pk.mem, &g) == Ok) {
        gx_set_quality(g);
        gx_clear(g, 0xFFFFFFFF, (REAL)cr.right, (REAL)cr.bottom);

        build_layout(cr.right, cr.bottom);
        wheel_sync(S);

        /* tooltip for the swatch under the cursor, built once per paint */
        g_pk.tipBuf[0] = 0;
        if (g_pk.hoverSwatch >= 0) {
            const Palette *hp = palette_get(g_pk.pal);
            if (g_pk.hoverSwatch < hp->count) {
                const Swatch *sw = &hp->items[g_pk.hoverSwatch];
                _snwprintf_s(g_pk.tipBuf, 96, _TRUNCATE, L"%s  #%02X%02X%02X",
                             sw->name[0] ? sw->name : L"未命名",
                             sw->c.r, sw->c.g, sw->c.b);
            }
        }
        build_hits();

        /* big swatch */
        {
            RECT b = g_lay.big;
            gx_round_rect_bg(g, rgb_to_argb(g_pk.cur), (REAL)b.left, (REAL)b.top,
                             (REAL)(b.right - b.left), (REAL)(b.bottom - b.top),
                             (REAL)(S * 10));
            /* pick a readable label colour */
            {
                double lum = rgb_luminance(g_pk.cur);
                GpFont *f = gx_font(g_pk.mono, (REAL)(S * 17));
                if (f) {
                    gx_draw_text(g, f, lum > 0.45 ? 0xFF000000 : 0xFFFFFFFF,
                                 g_pk.values[CF_HEX], (REAL)b.left, (REAL)(b.top + S * 22),
                                 (REAL)(b.right - b.left), (REAL)(S * 26));
                    GdipDeleteFont(f);
                }
            }
            gx_stroke_rect(g, 0x18000000, 1.0f, (REAL)b.left, (REAL)b.top,
                           (REAL)(b.right - b.left), (REAL)(b.bottom - b.top));
        }

        /* rgb readout + nearest name under the swatch column */
        {
            GpFont *f = gx_font(g_pk.fam, (REAL)(S * 11));
            const wchar_t *nm = NULL;
            rgb_nearest_named(g_pk.cur, &nm);
            if (f) {
                wchar_t rgbbuf[64], namebuf[64];
                _snwprintf_s(rgbbuf, 64, _TRUNCATE, L"R %u   G %u   B %u",
                             g_pk.cur.r, g_pk.cur.g, g_pk.cur.b);
                _snwprintf_s(namebuf, 64, _TRUNCATE, L"接近 %s", nm);
                gx_draw_text(g, f, 0xFF6B7280, rgbbuf,
                             (REAL)g_lay.swatchNote.left, (REAL)g_lay.swatchNote.top,
                             (REAL)(g_lay.swatchNote.right - g_lay.swatchNote.left),
                             (REAL)(S * 16));
                gx_draw_text(g, f, 0xFF6B7280, namebuf,
                             (REAL)g_lay.swatchNote.left,
                             (REAL)(g_lay.swatchNote.top + S * 14),
                             (REAL)(g_lay.swatchNote.right - g_lay.swatchNote.left),
                             (REAL)(S * 16));
                GdipDeleteFont(f);
            }
        }

        draw_buttons(g, g_lay.eyedrop, L"屏幕取色  (D)",
                     g_pk.hover == hit_zone(ZEY_EYEDROP), 0, 1, S);
        draw_buttons(g, g_lay.custom, L"自定色…",
                     g_pk.hover == hit_zone(ZEY_CUSTOM), 0, 0, S);
        draw_buttons(g, g_lay.fav, L"★ 收藏",
                     g_pk.hover == hit_zone(ZEY_FAV), 0, 0, S);

        paint_wheel(g, S);

        /* harmony tabs + the colours they produce */
        for (i = 0; i < wheel_scheme_count(); i++) {
            RECT t = g_lay.schemeTab[i];
            int on = (i == g_pk.scheme);
            gx_round_rect_bg(g, on ? 0xFF111827 : 0xFFF3F3F5,
                             (REAL)t.left, (REAL)t.top,
                             (REAL)(t.right - t.left), (REAL)(t.bottom - t.top),
                             (REAL)(S * 6));
            {
                GpFont *f = gx_font(g_pk.fam, (REAL)(S * 11));
                if (f) {
                    gx_draw_text(g, f, on ? 0xFFFFFFFF : 0xFF4B5563,
                                 wheel_scheme_at(i)->label,
                                 (REAL)t.left, (REAL)(t.top + S * 3),
                                 (REAL)(t.right - t.left), (REAL)(t.bottom - t.top - S * 5));
                    GdipDeleteFont(f);
                }
            }
        }
        for (i = 0; i < WHEEL_SLOTS; i++) {
            RECT s = g_lay.schemeSw[i];
            Rgb c = g_pk.schemeCol[i];
            gx_fill_rect(g, rgb_to_argb(c), (REAL)s.left, (REAL)s.top,
                         (REAL)(s.right - s.left), (REAL)(s.bottom - s.top));
            gx_stroke_rect(g, rgb_equal(c, g_pk.cur) ? 0xFF0A84FF : 0x20000000,
                           rgb_equal(c, g_pk.cur) ? (REAL)(S * 2) : 1.0f,
                           (REAL)s.left, (REAL)s.top,
                           (REAL)(s.right - s.left), (REAL)(s.bottom - s.top));
        }
        /* H/S/V readout under the harmonies */
        {
            GpFont *f = gx_font(g_pk.mono, (REAL)(S * 11));
            if (f) {
                wchar_t buf[80];
                double hh, ss, vv;
                rgb_to_hsv(g_pk.cur, &hh, &ss, &vv);
                _snwprintf_s(buf, 80, _TRUNCATE, L"H %3.0f°   S %3.0f%%   V %3.0f%%",
                             hh, ss * 100.0, vv * 100.0);
                gx_draw_text(g, f, 0xFF6B7280, buf,
                             (REAL)g_lay.wheelInfo.left, (REAL)g_lay.wheelInfo.top,
                             (REAL)(g_lay.wheelInfo.right - g_lay.wheelInfo.left),
                             (REAL)(g_lay.wheelInfo.bottom - g_lay.wheelInfo.top));
                GdipDeleteFont(f);
            }
        }

        /* format tabs */
        for (i = 0; i < CF_COUNT; i++) {
            RECT t = g_lay.fmtTabs[i];
            int on = (i == CF_HEX);
            gx_round_rect_bg(g, on ? 0xFF1C1C1E : 0xFFF3F3F5,
                             (REAL)t.left, (REAL)t.top,
                             (REAL)(t.right - t.left), (REAL)(t.bottom - t.top),
                             (REAL)(S * 6));
            {
                GpFont *f = gx_font(g_pk.fam, (REAL)(S * 10));
                if (f) {
                    gx_draw_text(g, f, on ? 0xFFFFFFFF : 0xFF4B5563,
                                 color_format_label((ColorFormat)i),
                                 (REAL)t.left, (REAL)(t.top + S * 3),
                                 (REAL)(t.right - t.left), (REAL)(t.bottom - t.top - S * 5));
                    GdipDeleteFont(f);
                }
            }
        }

        /* format rows */
        for (i = 0; i < CF_COUNT; i++) {
            RECT r = g_lay.fmtRow[i];
            GpFont *fl = gx_font(g_pk.fam, (REAL)(S * 10));
            GpFont *fm = gx_font(g_pk.mono, (REAL)(S * 12));
            if (i & 1) gx_fill_rect(g, 0xFFFAFAFC, (REAL)r.left - S * 3,
                                    (REAL)r.top, (REAL)(r.right - r.left) + S * 46, (REAL)(r.bottom - r.top));
            if (fl) {
                gx_draw_text(g, fl, 0xFF6B7280, color_format_label((ColorFormat)i),
                             (REAL)r.left, (REAL)(r.top + S * 4), (REAL)(S * 40),
                             (REAL)(r.bottom - r.top - S * 6));
                GdipDeleteFont(fl);
            }
            if (fm && g_pk.values[i]) {
                gx_draw_text(g, fm, 0xFF111827, g_pk.values[i],
                             (REAL)(r.left + S * 40), (REAL)(r.top + S * 3),
                             (REAL)(r.right - r.left - S * 42), (REAL)(r.bottom - r.top - S * 5));
                GdipDeleteFont(fm);
            }
            draw_buttons(g, g_lay.fmtCopy[i], L"复制", 0, 0, 0, S);
        }
        draw_buttons(g, g_lay.copyAll, L"复制全部  Ctrl+Shift+C", 0, 0, 0, S);

        /* the theme's colour rule */
        if (palette_note(g_pk.pal)) {
            GpFont *f = gx_font(g_pk.fam, (REAL)(S * 10));
            if (f) {
                gx_draw_text(g, f, 0xFF0A84FF, palette_note(g_pk.pal),
                             (REAL)g_lay.palNote.left, (REAL)g_lay.palNote.top,
                             (REAL)(g_lay.palNote.right - g_lay.palNote.left),
                             (REAL)(g_lay.palNote.bottom - g_lay.palNote.top));
                GdipDeleteFont(f);
            }
        }

        /* palette tabs */
        for (i = 0; i < PAL_COUNT; i++) {
            RECT t = g_lay.palTab[i];
            int on = ((int)g_pk.pal == i);
            gx_round_rect_bg(g, on ? 0xFF111827 : 0xFFF3F3F5,
                             (REAL)t.left, (REAL)t.top,
                             (REAL)(t.right - t.left), (REAL)(t.bottom - t.top),
                             (REAL)(S * 6));
            {
                GpFont *f = gx_font(g_pk.fam, (REAL)(S * 10));
                if (f) {
                    gx_draw_text(g, f, on ? 0xFFFFFFFF : 0xFF4B5563,
                                 palette_get((PaletteId)i)->label,
                                 (REAL)t.left, (REAL)(t.top + S * 3),
                                 (REAL)(t.right - t.left), (REAL)(t.bottom - t.top - S * 5));
                    GdipDeleteFont(f);
                }
            }
        }

        /* swatches */
        pal = palette_get(g_pk.pal);
        for (i = 0; i < g_lay.nswatch; i++) {
            RECT s = g_lay.swatch[i];
            int si = (g_pk.swScroll * g_lay.swPerRow) + i;
            int hi;
            if (si >= pal->count) break;
            hi = (g_pk.hoverSwatch == si);
            gx_fill_rect(g, rgb_to_argb(pal->items[si].c), (REAL)s.left, (REAL)s.top,
                         (REAL)(s.right - s.left), (REAL)(s.bottom - s.top));
            gx_stroke_rect(g, rgb_equal(pal->items[si].c, g_pk.cur) ? 0xFF0A84FF : 0x20000000,
                           rgb_equal(pal->items[si].c, g_pk.cur) ? (REAL)(S * 2) : 1.0f,
                           (REAL)s.left, (REAL)s.top,
                           (REAL)(s.right - s.left), (REAL)(s.bottom - s.top));
            if (hi) gx_stroke_rect(g, 0xFF111827, (REAL)S, (REAL)s.left - 1,
                                   (REAL)s.top - 1, (REAL)(s.right - s.left) + 2,
                                   (REAL)(s.bottom - s.top) + 2);
        }
        /* scroll indicator, only while the list does not fit */
        if (g_lay.swMaxScroll > 0 && g_lay.nswatch > 0) {
            RECT b = g_lay.swBar;
            RECT first = g_lay.swatch[0];
            int gridBot = first.bottom;
            for (i = 1; i < g_lay.nswatch; i++)
                if (g_lay.swatch[i].bottom > gridBot) gridBot = g_lay.swatch[i].bottom;
            gx_round_rect_bg(g, 0x22000000, (REAL)(b.left - S * 3),
                             (REAL)(first.top - S), (REAL)(S * 6),
                             (REAL)(gridBot - first.top + S * 2), (REAL)(S * 3));
            gx_round_rect_bg(g, 0xFF9CA3AF, (REAL)b.left, (REAL)b.top,
                             (REAL)(b.right - b.left), (REAL)(b.bottom - b.top),
                             (REAL)(S * 3));
        }

        /* status line */
        {
            GpFont *f = gx_font(g_pk.fam, (REAL)(S * 11));
            int show = g_pk.status[0] != 0 &&
                       (g_pk.statusUntil == 0 || GetTickCount64() < g_pk.statusUntil ||
                        GetForegroundWindow() == g_pk.hwnd);
            if (f) {
                wchar_t line[200];
                if (show) wcsncpy(line, g_pk.status, 199);
                else _snwprintf_s(line, 200, _TRUNCATE,
                                  L"Ctrl+Alt+C 取色   ·   D 取色   ·   F 收藏   ·   1-9 复制格式   ·   方向键调色   ·   滚轮翻色板");
                gx_draw_text(g, f, 0xFF9CA3AF, line, (REAL)(S * 10),
                             (REAL)(cr.bottom - S * 22), (REAL)(cr.right - S * 20),
                             (REAL)(S * 16));
                GdipDeleteFont(f);
            }
        }

        /* hover tooltip */
        if (g_pk.hover >= 0 && g_pk.hover < g_pk.nhit && g_pk.hit[g_pk.hover].tip) {
            GpFont *f = gx_font(g_pk.fam, (REAL)(S * 11));
            if (f) {
                REAL tw = 0, th = 0;
                RECT t;
                gx_measure_text(f, g_pk.hit[g_pk.hover].tip, &tw, &th);
                t.left = g_pk.hit[g_pk.hover].r.left;
                t.top  = g_pk.hit[g_pk.hover].r.bottom + S * 4;
                t.right  = t.left + (int)lround(tw) + S * 12;
                t.bottom = t.top + (int)lround(th) + S * 8;
                if (t.right > cr.right - S * 4) t.right = cr.right - S * 4;
                if (t.bottom > cr.bottom - S * 4) t.top = g_pk.hit[g_pk.hover].r.top - (t.bottom - t.top) - S * 4;
                gx_round_rect_bg(g, 0xF01C1C1E, (REAL)t.left, (REAL)t.top,
                                 (REAL)(t.right - t.left), (REAL)(t.bottom - t.top), (REAL)(S * 5));
                gx_draw_text(g, f, 0xFFFFFFFF, g_pk.hit[g_pk.hover].tip,
                             (REAL)(t.left + S * 6), (REAL)(t.top + S * 3),
                             (REAL)(t.right - t.left - S * 12), (REAL)(t.bottom - t.top - S * 4));
                GdipDeleteFont(f);
            }
        }

        GdipDeleteGraphics(g);
    }
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static void copy_text(const wchar_t *s, const wchar_t *what)
{
    if (clip_put_text(s)) {
        wchar_t msg[160];
        _snwprintf_s(msg, 160, _TRUNCATE, L"已复制 %s：%s", what, s);
        set_status(msg);
    } else {
        set_status(L"复制失败，剪贴板被其它程序占用");
    }
}

static void copy_all_formats(void)
{
    WBuf b;
    int i;
    wbuf_init(&b);
    for (i = 0; i < CF_COUNT; i++) {
        if (!g_pk.values[i]) continue;
        wbuf_addf(&b, L"%-6s %s\n", color_format_label((ColorFormat)i), g_pk.values[i]);
    }
    if (b.buf && clip_put_text(b.buf)) set_status(L"已复制全部颜色格式");
    wbuf_free(&b);
}

/* Is the screen already taken? Asked locally and answered in general terms, on
 * purpose: a window of another process that is visible, topmost and covering the
 * whole virtual screen is something mid-capture, and a magnifier is another
 * fullscreen thing that must not land on top of it. Nothing is exchanged with
 * whoever it is - the check is one enumeration, it cannot go stale, and it
 * behaves the same whether or not that program knows this one exists.
 *
 * Enumerated rather than asked of the foreground window on purpose: whether a
 * process *can* take the foreground is a separate question that Windows
 * decides (see the capture overlay's rescue hook in src\shot\editor.c), so a
 * check that leans on it fails exactly when it is needed - a capture started
 * from a hotkey the shell never granted focus to. */
static BOOL CALLBACK screen_taken_cb(HWND w, LPARAM p)
{
    int *hit = (int *)p;
    RECT r;
    LONG ex;
    DWORD pid = 0;
    int sx, sy, sw, sh;

    if (*hit) return FALSE;
    if (!IsWindowVisible(w)) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    ex = GetWindowLongW(w, GWL_EXSTYLE);
    if (!(ex & WS_EX_TOPMOST)) return TRUE;    /* an ordinary window is not a capture */
    if (!GetWindowRect(w, &r)) return TRUE;
    sx = GetSystemMetrics(SM_XVIRTUALSCREEN); sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    sw = GetSystemMetrics(SM_CXVIRTUALSCREEN); sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (r.left <= sx && r.top <= sy && r.right >= sx + sw && r.bottom >= sy + sh)
        *hit = 1;
    return TRUE;
}

static int screen_taken(void)
{
    int hit = 0;
    EnumWindows(screen_taken_cb, (LPARAM)&hit);
    return hit;
}

static void do_eyedrop(void)
{
    Rgb c;
    /* Already up. The overlay runs a nested loop, so its own tray command, the
     * hotkey and the D key all still land here while it is waiting for a pick -
     * which is exactly the moment the user reaches for the tray. A second one
     * would freeze the screen again and stack a loupe on the first, so the
     * armed pick is left alone. */
    if (eyedrop_active()) return;
    if (screen_taken()) {
        set_status(L"屏幕正被其它程序占用");
        return;
    }
    if (eyedrop_begin(g_pk.hwnd, &c)) {
        g_pk.lastPick = c;
        set_color(c, L"已从屏幕取色");
    } else {
        set_status(L"已取消取色");
    }
}

static void do_custom(void)
{
    CHOOSECOLORW cc;
    static COLORREF cust[16];
    Rgb c;
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_pk.hwnd;
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    cc.rgbResult = RGB(g_pk.cur.r, g_pk.cur.g, g_pk.cur.b);
    if (ChooseColorW(&cc)) {
        c.r = (BYTE)GetRValue(cc.rgbResult);
        c.g = (BYTE)GetGValue(cc.rgbResult);
        c.b = (BYTE)GetBValue(cc.rgbResult);
        set_color(c, L"已设置自定义颜色");
    }
}

static void activate(int hi)
{
    Hit *h;
    const Palette *pal;
    if (hi < 0 || hi >= g_pk.nhit) return;
    h = &g_pk.hit[hi];

    switch (h->z) {
    case ZEY_BIGSWATCH: do_custom(); break;
    case ZEY_EYEDROP:   do_eyedrop(); break;
    case ZEY_CUSTOM:    do_custom(); break;
    case ZEY_FAV:
        if (fav_add(g_pk.cur)) {
            fav_save();
            set_status(L"已加入收藏");
        } else {
            set_status(L"该颜色已在收藏中");
        }
        InvalidateRect(g_pk.hwnd, NULL, FALSE);
        break;
    case ZEY_FORMAT_TAB:
        copy_text(g_pk.values[h->idx], color_format_label((ColorFormat)h->idx));
        break;
    case ZEY_COPY_ROW:
        copy_text(g_pk.values[h->idx], color_format_label((ColorFormat)h->idx));
        break;
    case ZEY_COPY_ALL:
        copy_all_formats();
        break;
    /* The wheel is applied on button-down and on every move while dragging,
     * so there is nothing left to do when the button comes back up. */
    case ZEY_WHEEL_RING:
    case ZEY_WHEEL_TRI:
        break;
    case ZEY_SCHEME_TAB:
        g_pk.scheme = h->idx;
        refresh_values();
        set_status(wheel_scheme_at(h->idx)->label);
        break;
    case ZEY_SCHEME_SW:
        set_color(g_pk.schemeCol[h->idx], wheel_scheme_at(g_pk.scheme)->label);
        break;
    case ZEY_PAL_TAB:
        g_pk.pal = (PaletteId)h->idx;
        g_pk.swScroll = 0;
        InvalidateRect(g_pk.hwnd, NULL, FALSE);
        break;
    case ZEY_SWATCH:
        pal = palette_get(g_pk.pal);
        if (h->idx < pal->count) {
            set_color(pal->items[h->idx].c, pal->items[h->idx].name);
            /* Ctrl+click copies straight away in the HEX format */
            if (GetKeyState(VK_CONTROL) & 0x8000)
                copy_text(g_pk.values[CF_HEX], L"HEX");
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* wheel interaction                                                    */
/* ------------------------------------------------------------------ */

/* Applies the pointer position to the colour, honouring the drag mode chosen
 * on button-down. Loose hit-testing is used on purpose: sliding off the ring
 * keeps turning the hue, sliding off the triangle keeps the last saturation
 * instead of sticking at the edge. */
static void wheel_apply(int px, int py)
{
    double x = px - g_lay.wheel.left;
    double y = py - g_lay.wheel.top;
    double h, s, v;
    Rgb c;
    if (g_pk.drag == WDRAG_RING) {
        if (!wheel_hue_nearest(&g_pk.geom, x, y, &h)) return;
        g_pk.wheelHue = h;
        rgb_to_hsv(g_pk.cur, &h, &s, &v);
        hsv_to_rgb(g_pk.wheelHue, s, v, &c);
        set_color(c, NULL);
    } else if (g_pk.drag == WDRAG_TRI) {
        if (!wheel_tri_sv_loose(&g_pk.geom, x, y, &s, &v)) return;
        hsv_to_rgb(g_pk.wheelHue, s, v, &c);
        set_color(c, NULL);
    }
}

static void wheel_scroll(int delta)
{
    int next = g_pk.swScroll + delta;
    if (next < 0) next = 0;
    if (next > g_lay.swMaxScroll) next = g_lay.swMaxScroll;
    if (next == g_pk.swScroll) return;
    g_pk.swScroll = next;
    InvalidateRect(g_pk.hwnd, NULL, FALSE);
}

/* Arrow keys nudge the current colour, so the wheel is reachable without a
 * mouse. Left/right turn the hue, up/down move value, +/- the saturation. */
static void wheel_nudge(int vk, int shift)
{
    double h, s, v;
    Rgb c;
    int step = shift ? 10 : 1;
    rgb_to_hsv(g_pk.cur, &h, &s, &v);
    switch (vk) {
    case VK_LEFT:  h -= step; g_pk.wheelHue = h; break;
    case VK_RIGHT: h += step; g_pk.wheelHue = h; break;
    case VK_UP:    v += step / 100.0; break;
    case VK_DOWN:  v -= step / 100.0; break;
    case VK_OEM_PLUS:  case VK_ADD:
        s += step / 100.0; break;
    case VK_OEM_MINUS: case VK_SUBTRACT:
        s -= step / 100.0; break;
    default: return;
    }
    while (h < 0.0) h += 360.0;
    while (h >= 360.0) h -= 360.0;
    if (s < 0.0) s = 0.0; else if (s > 1.0) s = 1.0;
    if (v < 0.0) v = 0.0; else if (v > 1.0) v = 1.0;
    hsv_to_rgb(h, s, v, &c);
    set_color(c, NULL);
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */

static const wchar_t *MENU[] = {
    L"屏幕取色\t40001",
    L"显示主窗口\t40002",
    L"-\t0",
    L"关于\t40003",
    L"-\t0",
    L"退出\t40004",
};

enum {
    /* Tray-menu ids, kept out of any control-id range. See the lParam guard
     * in pk_proc: only a menu command arrives with lParam == 0. */
    PMI_EYEDROP = 40001, PMI_SHOW = 40002, PMI_ABOUT = 40003, PMI_EXIT = 40004
};

static LRESULT CALLBACK pk_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* The picker is a single-window app, so the instance is a file-static.
     * (Window user-data via SetWindowLongPtr is not used here - it does not
     * round-trip reliably under this toolchain.) */
    Picker *pk = &g_pk;
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(h);
        return 0;

    case WM_SIZE: {
        RECT w;
        GetWindowRect(h, &w);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    /* Dragged onto a monitor with a different scale: adopt the rect Windows
     * suggests (it keeps the window the same physical size on screen) and
     * re-read the scale from the window, because the desktop DC still reports
     * the system DPI rather than this monitor's. Ignoring this leaves the
     * whole layout built for the old scale, and every hard-coded size in it -
     * the wheel box, the swatch grid, the tab widths - comes out wrong. */
    case WM_DPICHANGED: {
        const RECT *sug = (const RECT *)l;
        app_set_dpi_from_window(h);
        SetWindowPos(h, NULL, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int hi;
        if (g_pk.drag != WDRAG_NONE) {
            wheel_apply(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        }
        hi = hit_test(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hi != pk->hover) {
            pk->hover = hi;
            pk->hoverSwatch = (hi >= 0 && pk->hit[hi].z == ZEY_SWATCH)
                                ? pk->hit[hi].idx : -1;
            InvalidateRect(h, NULL, FALSE);
        }
        {
            TRACKMOUSEEVENT t;
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            t.dwHoverTime = 0;
            TrackMouseEvent(&t);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (pk->hover >= 0) {
            pk->hover = -1;
            pk->hoverSwatch = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        wheel_scroll(d > 0 ? -1 : (d < 0 ? 1 : 0));
        return 0;
    }

    case WM_LBUTTONDOWN:
        pk->pressed = hit_test(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        SetCapture(h);
        if (pk->pressed >= 0 && pk->hit[pk->pressed].z == ZEY_WHEEL_RING) {
            int isRing = 0;
            if (wheel_hit(GET_X_LPARAM(l), GET_Y_LPARAM(l), &isRing)) {
                g_pk.drag = isRing ? WDRAG_RING : WDRAG_TRI;
                wheel_apply(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            }
        }
        return 0;

    case WM_LBUTTONUP:
        {
            int hi = hit_test(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            ReleaseCapture();
            g_pk.drag = WDRAG_NONE;
            if (hi >= 0 && hi == pk->pressed) activate(hi);
            pk->pressed = -1;
        }
        return 0;

    case WM_CONTEXTMENU: {
        int hi = hit_test(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hi >= 0 && pk->hit[hi].z == ZEY_SWATCH &&
            pk->pal == PAL_FAVORITE && pk->hit[hi].idx < fav_count()) {
            if (MessageBoxW(h, L"从收藏中删除这个颜色？", L"删除收藏",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                fav_remove_at(pk->hit[hi].idx);
                fav_save();
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        if (hi >= 0 && pk->hit[hi].z == ZEY_SWATCH) {
            const Palette *p = palette_get(pk->pal);
            if (pk->hit[hi].idx < p->count) {
                wchar_t msg[160];
                _snwprintf_s(msg, 160, _TRUNCATE, L"复制 %s 格式的值？", p->label);
                if (MessageBoxW(h, msg, L"复制", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                    set_color(p->items[pk->hit[hi].idx].c, NULL);
                    copy_all_formats();
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_HOTKEY:
        /* a tray menu tracking on this thread must not wrap the loupe */
        if (tray_menu_up(&pk->tray)) return 0;
        if (w == HOTKEY_ID) do_eyedrop();
        return 0;

    case WM_KEYDOWN:
        switch (w) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_OEM_PLUS: case VK_OEM_MINUS:
            wheel_nudge((int)w, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        case VK_PRIOR: wheel_scroll(-1); return 0;
        case VK_NEXT:  wheel_scroll(1);  return 0;
        case VK_HOME:  g_pk.swScroll = 0; InvalidateRect(h, NULL, FALSE); return 0;
        case VK_END:
            g_pk.swScroll = g_lay.swMaxScroll;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        default: break;
        }
        break;

    case WM_CHAR: {
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        /* With Ctrl held, a letter reaches WM_CHAR as its control code -
         * Ctrl+C arrives as 0x03, never as 'C' - so the chord has to accept
         * that spelling or the shortcut only works for the keys a probe
         * posts (which carry no translation at all). */
        if (ctrl && (w == 'C' || w == 'c' || w == 3) &&
            GetKeyState(VK_SHIFT) & 0x8000) {
            copy_all_formats();
            return 0;
        }
        if (w >= '1' && w <= '9') {
            int i = (int)w - '1';
            if (i < CF_COUNT && pk->values[i])
                copy_text(pk->values[i], color_format_label((ColorFormat)i));
            return 0;
        }
        if (w == 'd' || w == 'D') { do_eyedrop(); return 0; }
        if (w == 'f' || w == 'F') { activate(hit_zone(ZEY_FAV)); return 0; }
        break;
    }

    case WM_COMMAND:
        /* lParam is 0 for a menu command, the child HWND for a notification. */
        if (l != 0) break;
        switch (LOWORD(w)) {
        case PMI_EYEDROP: do_eyedrop(); break;
        case PMI_SHOW: ShowWindow(h, SW_SHOWNORMAL); SetForegroundWindow(h); break;
        case PMI_ABOUT:
            /* The name, icon and credit line come from src\common\about.c, so
             * every program says the same thing about who wrote them. */
            app_show_about(h,
                L"MuskPicker  ·  屏幕取色与常用色盘",
                L"取色      Ctrl+Alt+C 或 D\r\n"
                L"收藏      F            自定色  点击大色块\r\n"
                L"复制      1-9 对应下方格式，Ctrl+Shift+C 全部\r\n"
                L"色盘      切换上方标签；右键色块可复制全部格式\r\n"
                L"取消收藏  右键「我的收藏」里的色块");
            break;
        case PMI_EXIT: pk->quit = 1; break;
        default: break;
        }
        return 0;

    case WM_APP + 1:
        /* Same re-entrancy guard as shot: while one menu tracks, another
         * tray callback would rebuild it out from under TrackPopupMenu. */
        if (tray_menu_up(&pk->tray)) return 0;
        if (LOWORD(l) == WM_LBUTTONUP) {
            app_logw(L"tray: eyedrop");
            do_eyedrop();
            return 0;
        }
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            HMENU mnu = tray_build_menu(&pk->tray, 6, MENU);
            POINT pt;
            UINT cmd;
            if (!mnu) return 0;
            GetCursorPos(&pt);
            app_logw(L"tray: menu");
            tray_menu_set_up(&pk->tray, 1);
            SetForegroundWindow(h);
            cmd = (UINT)TrackPopupMenu(mnu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                       pt.x, pt.y, 0, h, NULL);
            tray_menu_set_up(&pk->tray, 0);
            PostMessageW(h, WM_NULL, 0, 0);
            if (cmd) SendMessageW(h, WM_COMMAND, (WPARAM)cmd, 0);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;

    case WM_DESTROY:
        pk->quit = 1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* self-test                                                           */
/* ------------------------------------------------------------------ */

static int check(int cond, const wchar_t *what)
{
    /* Assembled with _snwprintf_s rather than handed straight to app_logw as a
     * format with arguments: this toolchain's wide printf mishandles %ls, and
     * the report is useless if it cannot be read. */
    wchar_t line[192];
    _snwprintf_s(line, 192, _TRUNCATE, L"  [%s] %s",
                 cond ? L"pass" : L"FAIL", what);
    app_logw(L"%s", line);
    return cond ? 0 : 1;
}

static int check_res(const ResSnapshot *b, const ResSnapshot *a, const wchar_t *what)
{
    int ok = (a->gdiObjects == b->gdiObjects && a->userObjects == b->userObjects &&
              a->handles == b->handles && a->allocs == b->allocs);
    if (!ok)
        app_log("        delta gdi=%+ld user=%+ld handle=%+ld heap=%+ld",
                (long)a->gdiObjects - (long)b->gdiObjects,
                (long)a->userObjects - (long)b->userObjects,
                (long)a->handles - (long)b->handles,
                a->allocs - b->allocs);
    return check(ok, what);
}

static int close_enough(double a, double b, double eps) { return fabs(a - b) <= eps; }

int picker_selftest(void)
{
    ResSnapshot base, after, pre;
    int fails = 0;
    int i;

    app_set_verbose(1);
    app_log("MuskPicker self-test");

    /* --- conversion round trips -------------------------------------- */
    {
        Rgb c; Rgb back;
        double h, s, l, v, cx, m, y, k;
        c.r = 200; c.g = 87; c.b = 43;
        rgb_to_hsl(c, &h, &s, &l);
        hsl_to_rgb(h, s, l, &back);
        fails += check(rgb_equal(c, back), L"RGB -> HSL -> RGB round trip");
        rgb_to_hsv(c, &h, &s, &v);
        hsv_to_rgb(h, s, v, &back);
        fails += check(rgb_equal(c, back), L"RGB -> HSV -> RGB round trip");
        rgb_to_cmyk(c, &cx, &m, &y, &k);
        cmyk_to_rgb(cx, m, y, k, &back);
        fails += check(close_enough(back.r, c.r, 1.5) && close_enough(back.g, c.g, 1.5) &&
                       close_enough(back.b, c.b, 1.5), L"RGB -> CMYK -> RGB round trip");
    }
    {
        Rgb c, back;
        c.r = 0; c.g = 0; c.b = 0;
        hsl_to_rgb(0, 0, 0, &back);
        fails += check(rgb_equal(c, back), L"black survives HSL");
        c.r = 255; c.g = 255; c.b = 255;
        hsl_to_rgb(0, 0, 1, &back);
        fails += check(rgb_equal(c, back), L"white survives HSL");
    }
    {
        /* known reference values */
        Rgb c; double L, A, B;
        c.r = 255; c.g = 255; c.b = 255;
        rgb_to_lab(c, &L, &A, &B);
        fails += check(close_enough(L, 100.0, 0.5) && close_enough(A, 0.0, 0.5) && close_enough(B, 0.0, 0.5),
                       L"white maps to Lab(100,0,0)");
        c.r = 0; c.g = 0; c.b = 0;
        rgb_to_lab(c, &L, &A, &B);
        fails += check(close_enough(L, 0.0, 0.5), L"black maps to Lab L=0");
        fails += check(close_enough(rgb_contrast((Rgb){255,255,255}, (Rgb){0,0,0}), 21.0, 0.1),
                       L"white/black contrast is 21:1");
    }
    {
        /* A single channel is often all a caller wants - the wheel reads the
         * hue without caring about the rest. Passing NULL for the others has
         * to be legal, and it used to be an access violation. */
        Rgb c;
        double h, s, v;
        c.r = 12; c.g = 200; c.b = 90;
        rgb_to_hsv(c, &h, NULL, NULL);
        rgb_to_hsv(c, NULL, &s, NULL);
        rgb_to_hsv(c, NULL, NULL, &v);
        fails += check(h > 0.0 && s > 0.0 && v > 0.5,
                       L"HSV out-parameters are individually optional");
        rgb_to_hsl(c, &h, NULL, NULL);
        rgb_to_hsl(c, NULL, &s, NULL);
        rgb_to_hsl(c, NULL, NULL, &v);
        fails += check(h > 0.0 && s > 0.0 && v > 0.0,
                       L"HSL out-parameters are individually optional");
        c.r = c.g = c.b = 0;
        rgb_to_hsv(c, NULL, NULL, &v);
        fails += check(close_enough(v, 0.0, 0.001), L"black stays at V=0");
        c.r = c.g = c.b = 255;
        rgb_to_hsv(c, NULL, NULL, &v);
        fails += check(close_enough(v, 1.0, 0.001), L"white stays at V=1");
    }

    /* --- formatting --------------------------------------------------- */
    {
        Rgb c;
        wchar_t *s;
        c.r = 0x3A; c.g = 0x7B; c.b = 0xD5;
        s = color_format_value(CF_HEX, c);
        fails += check(s && wcscmp(s, L"#3A7BD5") == 0, L"HEX formats uppercase with hash");
        xfree(s);
        s = color_format_value(CF_RGB, c);
        fails += check(s && wcscmp(s, L"rgb(58, 123, 213)") == 0, L"RGB formats as rgb()");
        xfree(s);
        s = color_format_value(CF_DEC, c);
        fails += check(s && wcscmp(s, L"3832789") == 0, L"DEC formats as one integer");
        xfree(s);
    }

    /* --- signed colour spaces format with a sign, not as unsigned wraps --- */
    {
        /* Green's Lab a* is around -86 and its b* around +83. A negative
         * channel read through an unsigned conversion prints as 4294967210,
         * which is what used to come out of the LAB row for most colours. */
        Rgb c;
        wchar_t *s;
        int i, ok;
        static const Rgb CASES[3] = { { 0, 255, 0 }, { 0, 0, 255 }, { 0, 161, 214 } };
        for (i = 0; i < 3; i++) {
            c = CASES[i];
            s = color_format_value(CF_LAB, c);
            ok = s && wcsncmp(s, L"lab(", 4) == 0 &&
                 wcsstr(s, L"4294967") == NULL;
            if (ok) {
                /* at least one of the two middle channels is negative for
                 * every colour in this table */
                const wchar_t *p = s + 4;
                ok = 0;
                while (*p && *p != L')') {
                    if (*p == L'-') { ok = 1; break; }
                    p++;
                }
            }
            if (!ok)
                app_logw(L"        LAB of #%02X%02X%02X -> '%s'",
                         c.r, c.g, c.b, s ? s : L"(null)");
            fails += check(ok, L"LAB prints negative channels with a minus sign");
            xfree(s);
            s = color_format_value(CF_XYZ, c);
            fails += check(s && wcsncmp(s, L"xyz(", 4) == 0 &&
                          wcsstr(s, L"4294967") == NULL,
                         L"XYZ never wraps into unsigned values");
            xfree(s);
        }
        /* white pins the axes at zero, which is neither signed nor wrapped */
        c.r = c.g = c.b = 255;
        s = color_format_value(CF_LAB, c);
        fails += check(s && wcscmp(s, L"lab(100, 0, 0)") == 0,
                       L"white formats as lab(100, 0, 0)");
        xfree(s);
    }

    /* --- parsing ------------------------------------------------------ */
    {
        Rgb c;
        fails += check(color_parse(L"#3A7BD5", &c) && c.r == 0x3A && c.g == 0x7B && c.b == 0xD5,
                       L"parses #rrggbb");
        fails += check(color_parse(L"#abc", &c) && c.r == 0xAA && c.g == 0xBB && c.b == 0xCC,
                       L"parses #rgb shorthand");
        fails += check(color_parse(L"3A7BD5", &c) && c.b == 0xD5, L"parses bare hex digits");
        fails += check(!color_parse(L"", &c), L"rejects empty input");
        fails += check(!color_parse(L"zzz", &c), L"rejects garbage");
    }

    /* --- palettes ----------------------------------------------------- */
    {
        int total = 0, k;
        for (i = 0; i < palette_count(); i++) total += palette_get((PaletteId)i)->count;
        fails += check(total > 250, L"built-in palettes hold 250+ colours");
        fails += check(palette_get(PAL_GRAY)->count == 11, L"gray ramp has 11 steps");
        fails += check(palette_get(PAL_FAVORITE)->count >= 0, L"favorites readable");
        /* every ramp step carries its number - a blank name renders as
         * "未命名" in the hover tooltip, which is not a name for a grey */
        {
            const Palette *g = palette_get(PAL_GRAY);
            int k, named = 1;
            for (k = 0; k < g->count; k++)
                if (!g->items[k].name[0]) { named = 0; break; }
            fails += check(named, L"every gray step is named (no 未命名 swatches)");
        }
    }

    /* --- theme palettes ------------------------------------------------ */
    {
        int themes = 0, named = 0, dupes = 0, j;
        for (i = PAL_THEME_FIRST; i <= PAL_THEME_LAST; i++) {
            const Palette *p = palette_get((PaletteId)i);
            const wchar_t *note = palette_note((PaletteId)i);
            if (!note || !*note) { fails += check(0, L"every theme carries a rule"); break; }
            if (!palette_is_theme((PaletteId)i)) { fails += check(0, L"palette_is_theme agrees"); break; }
            if (p->count < 8) { fails += check(0, L"theme holds at least 8 colours"); break; }
            /* a theme must not show the same swatch twice, and every swatch
             * must carry a name and a non-empty colour */
            for (j = 0; j < p->count; j++) {
                int k2;
                if (!p->items[j].name[0]) { fails += check(0, L"theme swatch is named"); break; }
                for (k2 = j + 1; k2 < p->count; k2++)
                    if (rgb_equal(p->items[j].c, p->items[k2].c)) { dupes++; break; }
            }
            themes++;
        }
        fails += check(themes == 9, L"nine theme palettes registered");
        fails += check(dupes == 0, L"theme palettes hold no duplicate swatches");
        for (i = PAL_THEME_FIRST; i <= PAL_THEME_LAST; i++)
            named += palette_get((PaletteId)i)->count;
        fails += check(named > 120, L"theme palettes expose 120+ colours");
        fails += check(palette_note(PAL_COMMON) == NULL,
                       L"non-theme palettes have no rule text");
    }

    /* --- wheel geometry ------------------------------------------------ */
    {
        WheelGeom g;
        double h, s, v, x, y;
        int k;
        wheel_geom(&g, 156, 156, 15);
        fails += check(g.rOut > g.rIn && g.rIn > g.triHalf,
                       L"ring band and triangle nest inside each other");
        fails += check(!wheel_ring_hue(&g, g.cx, g.cy, NULL),
                       L"wheel centre is not part of the ring");
        fails += check(!wheel_tri_sv(&g, g.cx, g.triTop - 5, NULL, NULL),
                       L"point above the apex is outside the triangle");
        /* every hue must survive ring_point -> ring_hue */
        {
            int bad = 0;
            for (k = 0; k < 720; k++) {
                double want = (double)k / 2.0;
                double got;
                wheel_ring_point(&g, want, &x, &y);
                if (!wheel_ring_hue(&g, x, y, &got)) { bad++; continue; }
                /* 0 and 360 are the same hue; the wrap can land on either */
                if (fabs(got - want) > 0.5 && fabs(got - want - 360.0) > 0.5 &&
                    fabs(got - want + 360.0) > 0.5) bad++;
            }
            fails += check(bad == 0, L"ring marker round-trips for all 360 hues");
        }
        /* saturation/value must survive tri_point -> tri_sv. V=1 is left out
         * on purpose: the whole cross-section collapses to the apex there, so
         * saturation stops being representable and the apex reads back as
         * white. That is the one place the mapping is intentionally lossy. */
        {
            int bad = 0;
            double sv[][2] = { {0,1}, {1,0}, {0.5,0.5}, {0.25,0.9}, {0,0.5},
                               {0.75,0.25}, {1,0.5} };
            for (k = 0; k < 7; k++) {
                double gs, gv;
                wheel_tri_point(&g, sv[k][0], sv[k][1], &x, &y);
                if (!wheel_tri_sv(&g, x, y, &gs, &gv)) { bad++; continue; }
                if (fabs(gs - sv[k][0]) > 0.01 || fabs(gv - sv[k][1]) > 0.01) bad++;
            }
            fails += check(bad == 0, L"triangle marker round-trips S/V");
        }
        /* the corners must be the three reference colours */
        fails += check(wheel_tri_sv(&g, g.cx, g.triTop, &s, &v) &&
                       s < 0.02 && v > 0.98, L"triangle apex is white");
        fails += check(wheel_tri_sv(&g, g.cx, g.triBot, &s, &v) &&
                       v < 0.02, L"triangle base is black");
        fails += check(wheel_tri_sv(&g, g.cx + g.triHalf * 0.5, g.triTop +
                                    (g.triBot - g.triTop) * 0.5, &s, &v) &&
                       s > 0.95 && fabs(v - 0.5) < 0.02,
                       L"triangle right edge is the pure hue");
        fails += check(wheel_tri_sv(&g, g.cx - g.triHalf * 0.5, g.triTop +
                                    (g.triBot - g.triTop) * 0.5, &s, &v) &&
                       s < 0.05, L"triangle left edge is unsaturated");
        /* red sits at the top of the ring and grows clockwise */
        wheel_ring_point(&g, 0.0, &x, &y);
        fails += check(y < g.cy - 1.0 && fabs(x - g.cx) < 1.0,
                       L"hue 0 (red) points straight up");
        wheel_ring_point(&g, 90.0, &x, &y);
        fails += check(x > g.cx + 1.0 && fabs(y - g.cy) < 1.0,
                       L"hue 90 (yellow-green) points right");
        /* degenerate sizes must not divide by zero */
        wheel_geom(&g, 4, 4, 1);
        fails += check(g.rIn >= 1.0, L"tiny wheel keeps a usable inner radius");
        wheel_geom(&g, 156, 156, 15);
    }

    /* --- harmonies ----------------------------------------------------- */
    {
        int n = wheel_scheme_count(), j, k2, bad = 0;
        Rgb base, out[WHEEL_SLOTS];
        double h0, s0, v0;
        base.r = 200; base.g = 87; base.b = 43;
        fails += check(n == 5, L"five colour harmonies registered");
        for (i = 0; i < n; i++) {
            const WheelScheme *sc = wheel_scheme_at(i);
            int before = bad;
            if (!sc->id || !*sc->id || !sc->label || !*sc->label) bad++;
            wheel_scheme_colors(sc, base, out);
            /* slot 0 is always the colour that is on the wheel */
            if (!rgb_equal(out[0], base)) bad++;
            /* no two slots may collapse onto the same swatch */
            for (j = 0; j < WHEEL_SLOTS; j++)
                for (k2 = j + 1; k2 < WHEEL_SLOTS; k2++)
                    if (rgb_equal(out[j], out[k2])) { bad++; j = WHEEL_SLOTS; break; }
            if (bad != before) {
                wchar_t msg[192];
                _snwprintf_s(msg, 192, _TRUNCATE,
                             L"  ! scheme '%s' -> #%02X%02X%02X #%02X%02X%02X "
                             L"#%02X%02X%02X #%02X%02X%02X #%02X%02X%02X",
                             sc->id,
                             out[0].r, out[0].g, out[0].b,
                             out[1].r, out[1].g, out[1].b,
                             out[2].r, out[2].g, out[2].b,
                             out[3].r, out[3].g, out[3].b,
                             out[4].r, out[4].g, out[4].b);
                app_logw(L"%s", msg);
            }
        }
        fails += check(bad == 0, L"each harmony yields 5 distinct real colours");
        /* the maths that makes a harmony a harmony. Hue is quantised to 8
         * bits on the way out and read back, so a couple of degrees of slack
         * is the real tolerance, not the 0.5 a float pipeline could promise. */
        rgb_to_hsv(base, &h0, &s0, &v0);
        wheel_scheme_colors(wheel_scheme_at(wheel_scheme_index_of(L"complement")), base, out);
        { double h1; rgb_to_hsv(out[1], &h1, NULL, NULL);
          fails += check(fabs(fmod(h1 - h0 + 360.0, 360.0) - 180.0) < 2.0,
                         L"complementary slot is 180 degrees across"); }
        wheel_scheme_colors(wheel_scheme_at(wheel_scheme_index_of(L"triadic")), base, out);
        {
            double h1, h2, d1, d2;
            rgb_to_hsv(out[1], &h1, NULL, NULL);
            rgb_to_hsv(out[2], &h2, NULL, NULL);
            d1 = fmod(h1 - h0 + 360.0, 360.0);
            d2 = fmod(h2 - h0 + 360.0, 360.0);
            fails += check(fabs(d1 - 120.0) < 2.0 && fabs(d2 - 240.0) < 2.0,
                           L"triadic slots sit 120/240 degrees out");
        }
        wheel_scheme_colors(wheel_scheme_at(wheel_scheme_index_of(L"split")), base, out);
        {
            double h1, h2, d1, d2;
            rgb_to_hsv(out[1], &h1, NULL, NULL);
            rgb_to_hsv(out[2], &h2, NULL, NULL);
            d1 = fmod(h1 - h0 + 360.0, 360.0);
            d2 = fmod(h2 - h0 + 360.0, 360.0);
            fails += check(fabs(d1 - 150.0) < 2.0 && fabs(d2 - 210.0) < 2.0,
                           L"split-complementary slots sit 150/210 degrees out");
        }
        wheel_scheme_colors(wheel_scheme_at(wheel_scheme_index_of(L"analogous")), base, out);
        {
            int inRange = 1;
            for (j = 0; j < WHEEL_SLOTS; j++) {
                double hh, dd;
                rgb_to_hsv(out[j], &hh, NULL, NULL);
                dd = fmod(hh - h0 + 360.0, 360.0);
                if (dd > 180.0) dd = 360.0 - dd;
                if (dd > 60.5) inRange = 0;
            }
            fails += check(inRange, L"analogous slots stay within 60 degrees");
        }
        /* grey has no hue of its own; a harmony must still come out sane */
        base.r = base.g = base.b = 128;
        for (i = 0; i < n; i++) {
            wheel_scheme_colors(wheel_scheme_at(i), base, out);
            if (!rgb_equal(out[0], base)) { fails += check(0, L"grey base survives"); break; }
        }
    }

    /* --- wheel raster -------------------------------------------------- */
    {
        /* Plain memory, no GDI+: the renderer only writes BGRA bytes.
         *
         * These assert relationships, not exact channel values. A pixel is a
         * 2x2 average of samples offset by up to half a pixel, so an exact RGB
         * expectation would be testing the sampling grid instead of the colour
         * maths. Exact values belong at the geometry level above, where no
         * rasterisation sits in the way. */
        enum { RW = 64 };
        static BYTE buf[RW * RW * 4];
        WheelGeom g;
        BGRAPX *px, *ring[4];
        int rMid, cx, cy, y1, y2, up, lo, ringR, ringG, ringB;

        #define RING_PX(dx, dy) \
            ((BGRAPX *)(buf + (((cy + (dy)) * RW) + (cx + (dx))) * 4))

        wheel_geom(&g, RW, RW, 6);
        cx = (int)g.cx; cy = (int)g.cy;
        rMid = (int)((g.rOut + g.rIn) / 2.0);

        memset(buf, 0, sizeof(buf));
        wheel_render_ring(buf, RW * 4, RW, RW, &g);

        /* Hue 0 sits at 12 o'clock and runs clockwise, so the dominant
         * channel walks red -> green -> cyan -> violet around the ring.
         * This is the assertion that pins the convention down. */
        ring[0] = RING_PX(0, -rMid);
        ring[1] = RING_PX(rMid, 0);
        ring[2] = RING_PX(0, rMid);
        ring[3] = RING_PX(-rMid, 0);
        fails += check(ring[0]->r > 200 && ring[0]->g < 90 && ring[0]->b < 90,
                       L"ring: hue 0 is red at 12 o'clock");
        fails += check(ring[1]->g > 200 && ring[1]->g > ring[1]->r && ring[1]->b < 90,
                       L"ring: hue 90 is yellow-green at 3 o'clock");
        fails += check(ring[2]->g > 170 && ring[2]->b > 170 && ring[2]->r < 90,
                       L"ring: hue 180 is cyan at 6 o'clock");
        fails += check(ring[3]->b > 170 && ring[3]->g < 90 && ring[3]->r < ring[3]->b,
                       L"ring: hue 270 is violet at 9 o'clock");
        /* the hole must stay untouched so the triangle can show through */
        px = (BGRAPX *)(buf + cy * RW * 4 + cx * 4);
        fails += check(px->r == 0 && px->a == 0,
                       L"ring leaves the inner hole untouched");

        /* The triangle darkens towards the base, and its ramp follows the
         * wheel hue instead of staying stuck on red. */
        y1 = (int)(g.triTop + (g.triBot - g.triTop) * 0.25);
        y2 = (int)(g.triTop + (g.triBot - g.triTop) * 0.75);
        memset(buf, 0, sizeof(buf));
        wheel_render_tri(buf, RW * 4, RW, RW, &g, 0.0);
        up = ((BGRAPX *)(buf + y1 * RW * 4 + cx * 4))->r;
        lo = ((BGRAPX *)(buf + y2 * RW * 4 + cx * 4))->r;
        fails += check(up > 0 && lo > 0 && up > lo + 40,
                       L"triangle darkens from apex to base");
        memset(buf, 0, sizeof(buf));
        wheel_render_tri(buf, RW * 4, RW, RW, &g, 240.0);
        px = (BGRAPX *)(buf + y1 * RW * 4 + cx * 4);
        fails += check(px->b > 140 && px->b > px->r,
                       L"triangle ramp follows the wheel hue");
        /* The fill must stop at the slanted edges. wheel_tri_sv clamps
         * saturation rather than rejecting, which is what a drag wants; using
         * it as a containment test paints the bounding box instead of the
         * triangle and buries the ring underneath. */
        px = (BGRAPX *)(buf + y1 * RW * 4 + 1 * 4);
        fails += check(px->a == 0,
                       L"triangle leaves the area beside its left edge alone");
        px = (BGRAPX *)(buf + y2 * RW * 4 + (RW - 2) * 4);
        fails += check(px->a == 0,
                       L"triangle leaves the area beside its right edge alone");
        /* and the ring is not painted over by it */
        memset(buf, 0, sizeof(buf));
        wheel_render_ring(buf, RW * 4, RW, RW, &g);
        px = RING_PX(rMid, 0);
        ringR = px->r; ringG = px->g; ringB = px->b;
        wheel_render_tri(buf, RW * 4, RW, RW, &g, 0.0);
        px = RING_PX(rMid, 0);
        fails += check(px->r == ringR && px->g == ringG && px->b == ringB,
                       L"triangle leaves the ring band untouched");
        #undef RING_PX
    }

    /* --- favourites ---------------------------------------------------- */
    {
        Rgb a, b, d;
        a.r = 10; a.g = 20; a.b = 30;
        b.r = 40; b.g = 50; b.b = 60;
        d.r = 70; d.g = 80; d.b = 90;
        fav_free();
        res_snapshot(&base);
        for (i = 0; i < 200; i++) {
            fav_add(a); fav_add(b); fav_add(d);
        }
        fails += check(fav_count() == 3, L"favorites de-duplicate");
        fails += check(fav_remove_at(1) && fav_count() == 2, L"favorite removal works");
        res_snapshot(&after);
        fails += check(after.allocs == base.allocs, L"favorite churn leaks nothing");
        fav_free();
    }

    /* --- formatting + layout churn, watching every resource counter --- */
    if (gdiplus_start()) {
        GpFontFamily *fam = NULL;
        RECT cr;
        cr.left = cr.top = 0; cr.right = app_scale_px(400); cr.bottom = app_scale_px(700);
        g_pk.memW = g_pk.memH = 0;
        GdipCreateFontFamilyFromName(L"Microsoft YaHei UI", NULL, &fam);
        g_pk.fam = fam;
        GdipCreateFontFamilyFromName(L"Consolas", NULL, &g_pk.mono);
        g_pk.hover = -1; g_pk.pressed = -1;
        g_pk.cur.r = 58; g_pk.cur.g = 123; g_pk.cur.b = 213;
        refresh_values();

        /* warm-up: GDI+ caches fonts on first use */
        for (i = 0; i < 2; i++) {
            build_layout(cr.right, cr.bottom);
            build_hits();
        }

        /* Every rectangle the layout produces has to sit inside the client
         * area it was built for. A rect that runs past the right or bottom
         * edge is invisible - it is painted, hit-tested and clicked, but the
         * blit never reaches the screen, which is exactly how the palette
         * column used to disappear instead of being clamped or wrapped. */
        {
            wchar_t diag[220];
            int bad = 0, worst = 0, k;
            diag[0] = 0;
            for (k = 0; k < 5; k++) {
                int W = app_scale_px(360 + k * 60);
                int H = app_scale_px(560 + k * 40);
                g_pk.pal = (PaletteId)(k % PAL_COUNT);
                build_layout(W, H);
                {
                    const RECT *rs[10];
                    int n = 0, ri, ai;
                    rs[n++] = &g_lay.big; rs[n++] = &g_lay.eyedrop;
                    rs[n++] = &g_lay.custom; rs[n++] = &g_lay.fav;
                    rs[n++] = &g_lay.swatchNote;
                    rs[n++] = &g_lay.wheel; rs[n++] = &g_lay.wheelInfo;
                    rs[n++] = &g_lay.copyAll; rs[n++] = &g_lay.palNote;
                    for (ri = 0; ri < n; ri++) {
                        const RECT *r = rs[ri];
                        if (r->left < 0 || r->top < 0 ||
                            r->right > W || r->bottom > H) {
                            if (bad == 0) {
                                _snwprintf_s(diag, 220, _TRUNCATE,
                                             L"rect %d,%d,%d,%d escapes %dx%d",
                                             r->left, r->top, r->right, r->bottom,
                                             W, H);
                            }
                            bad++;
                        }
                    }
                    for (ai = 0; ai < g_lay.nswatch; ai++) {
                        const RECT *r = &g_lay.swatch[ai];
                        if (r->right > W + 1) { if (r->right - W > worst) worst = r->right - W; bad++; }
                    }
                    for (ai = 0; ai < PAL_COUNT; ai++)
                        if (g_lay.palTab[ai].right > W + 1) bad++;
                    /* The readouts own the band under the swatch. The wheel
                     * starting on top of it is exactly how the nearest-name
                     * line came to vanish under the ring's backdrop. */
                    if (g_lay.wheel.top < g_lay.swatchNote.bottom) {
                        if (bad == 0)
                            _snwprintf_s(diag, 220, _TRUNCATE,
                                         L"wheel starts at %d, readouts end at %d",
                                         g_lay.wheel.top, g_lay.swatchNote.bottom);
                        bad++;
                    }
                }
            }
            fails += check(bad == 0, L"layout keeps every rect inside the client area");
            if (bad) app_logw(L"  %d rect(s) overflow; worst +%d px; %s", bad, worst, diag);
        }
        res_snapshot(&base);
        for (i = 0; i < 300; i++) {
            Rgb c;
            c.r = (BYTE)(i * 7); c.g = (BYTE)(i * 13); c.b = (BYTE)(i * 29);
            set_color(c, L"");
            g_pk.pal = (PaletteId)(i % PAL_COUNT);
            build_layout(cr.right - (i % 5) * 20, cr.bottom);
            build_hits();
            hit_test(i % 300, (i * 3) % 600);
        }
        res_snapshot(&after);
        fails += check_res(&base, &after, L"layout/format/values x300 leak nothing");

        /* Wheel churn. The two cached surfaces are long-lived app state, the
         * same kind of thing as the back buffer, so the baseline is taken once
         * they already exist - otherwise their first allocation reads as a
         * leak. From there the count must not move at all: a size change frees
         * and recreates both, a hue change only re-renders the triangle. */
        g_pk.hoverSwatch = -1;
        res_snapshot(&pre);
        build_layout(app_scale_px(400), app_scale_px(700));
        wheel_sync(app_scale_px(1));
        res_snapshot(&base);
        for (i = 0; i < 60; i++) {
            Rgb c;
            int bw = app_scale_px(320 + (i % 5) * 24);
            c.r = (BYTE)(i * 11); c.g = (BYTE)(i * 7); c.b = (BYTE)(i * 23);
            set_color(c, L"");
            build_layout(bw, app_scale_px(700));
            wheel_sync(app_scale_px(1));
            build_hits();
            hit_test(i % 300, (i * 5) % 200);
        }
        res_snapshot(&after);
        fails += check_res(&base, &after, L"wheel raster/cache churn leaks nothing");
        wheel_cache_release();
        res_snapshot(&after);
        fails += check(after.allocs == pre.allocs && after.handles == pre.handles &&
                       after.gdiObjects == pre.gdiObjects,
                       L"wheel caches release everything");

        for (i = 0; i < CF_COUNT; i++) xfree(g_pk.values[i]);
        xfree(g_pk.hit);
        g_pk.hit = NULL; g_pk.nhit = g_pk.chit = 0;
        if (g_pk.mem) { GdipDisposeImage((GpImage *)g_pk.mem); g_pk.mem = NULL; }
        if (g_pk.memBmp) { free_dib(g_pk.memBmp, g_pk.memBits); g_pk.memBmp = NULL; }
        if (g_pk.fam) { GdipDeleteFontFamily(g_pk.fam); g_pk.fam = NULL; }
        if (g_pk.mono) { GdipDeleteFontFamily(g_pk.mono); g_pk.mono = NULL; }
        gdiplus_stop();
    }

    app_logw(L"self-test %s (%d failures)", fails ? L"FAILED" : L"PASSED", fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    int argc = 0, i, hotkeys = 0;
    LPWSTR *argv;

    (void)prev; (void)show;
    app_attach_parent_console();
    app_set_dpi_aware();
    app_install_crash_log(L"picker");
    app_set_verbose(0);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--selftest") == 0) {
            if (argv) LocalFree(argv);
            return picker_selftest();
        }
        if (_wcsicmp(argv[i], L"--verbose") == 0) app_set_verbose(1);
    }
    if (argv) LocalFree(argv);

    memset(&g_pk, 0, sizeof(g_pk));
    g_pk.inst = inst;
    g_pk.hover = g_pk.pressed = -1;
    g_pk.hoverSwatch = -1;
    g_pk.pal = PAL_THEME_NATURAL;
    g_pk.scheme = 0;
    g_pk.drag = WDRAG_NONE;
    g_pk.cur.r = 58; g_pk.cur.g = 123; g_pk.cur.b = 213;
    {
        double h0, s0, v0;
        rgb_to_hsv(g_pk.cur, &h0, &s0, &v0);
        g_pk.wheelHue = h0;
    }
    g_pk.status[0] = 0;

    if (!gdiplus_start()) {
        MessageBoxW(NULL, L"无法初始化 GDI+，程序无法启动。", L"MuskPicker", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GdipCreateFontFamilyFromName(L"Microsoft YaHei UI", NULL, &g_pk.fam) != Ok)
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &g_pk.fam);
    if (GdipCreateFontFamilyFromName(L"Consolas", NULL, &g_pk.mono) != Ok)
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &g_pk.mono);
    refresh_values();
    fav_load();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = pk_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = PKG_CLASS;
    /* The logo build.ps1 embeds as res\logo-picker.ico - the same artwork the
     * .exe shows in Explorer, rather than the stock Windows application icon. */
    wc.hIcon         = app_icon_load(inst, 0);
    wc.hIconSm       = app_icon_load(inst, GetSystemMetrics(SM_CXSMICON));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    {
        int wx, wy, ww, wh;
        app_fit_window(app_scale_px(500), app_scale_px(700), &wx, &wy, &ww, &wh);
        g_pk.hwnd = CreateWindowExW(0, PKG_CLASS, L"MuskPicker  取色与色盘",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                    WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                    wx, wy, ww, wh, NULL, NULL, inst, NULL);
    }
    if (!g_pk.hwnd) return 1;
    /* Now that a window exists, re-read the scale from it: the desktop DC
     * reports the system DPI, which is not this monitor's on a mixed-DPI
     * desktop, and every size below depends on the answer. */
    app_set_dpi_from_window(g_pk.hwnd);

    tray_init_icon(&g_pk.tray, g_pk.hwnd, WM_APP + 1,
                   trayicon_from_logo(TRAY_ICON_COLOR),
                   L"MuskPicker 取色器  —  Ctrl+Alt+C");
    if (tray_add(&g_pk.tray)) {
        if (!RegisterHotKey(g_pk.hwnd, HOTKEY_ID,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C')) {
            if (!RegisterHotKey(g_pk.hwnd, HOTKEY_ID,
                                MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'C'))
                hotkeys = 1;
        }
    }

    ShowWindow(g_pk.hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_pk.hwnd);

    while (!g_pk.quit) {
        if (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            break;
        }
    }

    UnregisterHotKey(g_pk.hwnd, HOTKEY_ID);
    if (hotkeys)
        tray_balloon(&g_pk.tray, L"MuskPicker", L"全局快捷键被占用，可从托盘或窗口取色", NIIF_WARNING);
    tray_free(&g_pk.tray);
    for (i = 0; i < CF_COUNT; i++) xfree(g_pk.values[i]);
    xfree(g_pk.hit);
    if (g_pk.mem) GdipDisposeImage((GpImage *)g_pk.mem);
    if (g_pk.memBmp) free_dib(g_pk.memBmp, g_pk.memBits);
    /* The wheel surfaces belong to this GDI+ instance, so they have to go
     * before gdiplus_stop() - same rule as gx_purge_cache(). */
    wheel_cache_release();
    if (g_pk.fam) GdipDeleteFontFamily(g_pk.fam);
    if (g_pk.mono) GdipDeleteFontFamily(g_pk.mono);
    fav_save();
    DestroyWindow(g_pk.hwnd);
    UnregisterClassW(PKG_CLASS, inst);
    gdiplus_stop();
    return 0;
}
