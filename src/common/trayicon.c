/* trayicon.c - turns logo.svg into a multi-resolution HICON for the tray.
 *
 * The shape comes straight from logo.svg (the two <path> elements, copied
 * verbatim; newlines inside a path's `d` are just separators). The artwork
 * itself is deliberately NOT parsed out of the file at runtime: the three
 * executables are built to be copied anywhere on their own, so everything they
 * draw has to live inside the .exe.
 *
 * So this file carries a small SVG path interpreter, feeds the result to GDI+
 * as a GpPath, and fills it in a single colour. That keeps logo.svg the single
 * source of truth for the shape while letting the colour be a parameter.
 *
 * Supported path commands: M m L l H h V v C c S s Q q T t Z z. Elliptical
 * arcs (A a) are not implemented - logo.svg does not use them, and a
 * mis-rendered arc would be worse than an honest bail-out.
 */
#include "trayicon.h"

/* ------------------------------------------------------------------ */
/* logo.svg, verbatim                                                 */
/* ------------------------------------------------------------------ */

/* The board outline, plus the swirl cut out of it. */
static const char LOGO_PATH_BOARD[] =
    "M23.543,0.973c1.43-0.174,2.876-0.123,4.315-0.142C43.831,0.83,59.806,0.834,75.781,0.828\n"
    "C82.817,1,89.686,4.559,93.858,10.233c2.996,3.893,4.544,8.829,4.483,13.731c-0.013,17.547-0.015,35.095-0.021,52.642\n"
    "c-0.015,6.968-3.702,13.695-9.289,17.781c-3.893,2.947-8.75,4.629-13.639,4.614c-15.088-0.032-30.176,0.004-45.266-0.009\n"
    "c-2.011-0.045-4.027,0.07-6.036-0.055c-4.376-0.396-8.682-1.983-12.139-4.719c-3.534-2.67-6.316-6.359-7.801-10.539\n"
    "c-0.811-2.262-1.185-4.665-1.285-7.059C2.867,59.504,2.86,42.392,2.862,25.277c-0.03-2.476,0.075-4.993,0.768-7.385\n"
    "c1.096-4.138,3.427-7.935,6.584-10.823C13.823,3.634,18.595,1.496,23.543,0.973z M26.326,9.221\n"
    "c-1.289,1.527-1.664,3.563-1.938,5.483c-0.313,2.698-0.304,5.423-0.117,8.129c0.332,3.34,0.721,6.687,1.542,9.948\n"
    "c1.657,7.331,4.825,14.284,9.086,20.464c-3.642,2.712-6.912,6.083-8.861,10.231c-1.544,3.068-2.323,6.499-2.427,9.923\n"
    "c-0.062,2.944,0.774,5.882,2.272,8.408c1.823,3.017,4.631,5.359,7.748,6.953c3.993,2.038,8.486,2.832,12.92,3.136\n"
    "c4.982,0.253,10.033,0.183,14.907-0.979c3.581-0.783,7.006-2.33,9.893-4.593c1.91-1.572,3.534-3.5,4.659-5.706\n"
    "c1.468-2.906,1.936-6.272,1.476-9.486c-0.538-4.168-2.142-8.195-4.629-11.584c-1.759-2.459-4.061-4.465-6.525-6.187\n"
    "c1.945-3.021,3.814-6.108,5.293-9.391c3.581-7.855,5.451-16.479,5.516-25.11c-0.117-2.795-0.196-5.697-1.393-8.282\n"
    "c-0.609-1.198-1.583-2.4-2.978-2.659c-1.349-0.311-2.742,0.14-3.889,0.843c-1.636,1.021-3.023,2.393-4.246,3.87\n"
    "c-3.138,3.742-5.463,8.121-7.053,12.727c-1.342,3.949-2.255,8.044-2.77,12.182c-0.434,3.959-0.721,7.976-0.202,11.941\n"
    "c-2.636-0.525-5.372-0.492-8.014-0.021c0.313-1.942,0.321-3.915,0.275-5.878c-0.17-5.41-0.964-10.816-2.506-16.009\n"
    "c-1.932-6.51-5.17-12.812-10.25-17.424c-1.298-1.117-2.8-2.204-4.57-2.31C28.328,7.729,27.086,8.266,26.326,9.221z";

/* The separate dot at the lower right. */
static const char LOGO_PATH_DOT[] =
    "M61.906,64.11c1.066-0.226,2.232-0.038,3.127,0.602c1.211,0.796,1.802,2.361,1.557,3.77\n"
    "c-0.343,1.691-1.878,3.104-3.627,3.159c-2.011,0.068-3.87-1.702-3.929-3.71C58.904,66.188,60.204,64.478,61.906,64.11z";

static const char *const LOGO_PATHS[] = { LOGO_PATH_BOARD, LOGO_PATH_DOT };
#define LOGO_PATH_N ((int)(sizeof(LOGO_PATHS) / sizeof(LOGO_PATHS[0])))

/* logo.svg's viewBox is 0 0 100 100, so path units map onto the icon box by
 * dividing by this. */
#define LOGO_UNITS 100.0f

/* ------------------------------------------------------------------ */
/* minimal SVG path reader                                            */
/* ------------------------------------------------------------------ */

typedef struct { const char *p; } Svg;

/* Advances to the next value or command. Whitespace and commas are separators;
 * anything else that cannot start either is skipped so one stray token does not
 * derail the rest of the path. */
static void svg_sep(Svg *s)
{
    for (;;) {
        unsigned char c = (unsigned char)*s->p;
        if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            s->p++;
            continue;
        }
        if (!c) return;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return;
        s->p++;
    }
}

static int svg_num(Svg *s, double *out)
{
    char *end = NULL;
    double v;
    svg_sep(s);
    if (!*s->p) return 0;
    v = strtod(s->p, &end);
    if (end == s->p) return 0;   /* not a number - leave the cursor alone */
    s->p = end;
    *out = v;
    return 1;
}

static int svg_cmd(Svg *s, char *out)
{
    unsigned char c;
    svg_sep(s);
    c = (unsigned char)*s->p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    *out = (char)c;
    s->p++;
    return 1;
}

static REAL gx2r(double v) { return (REAL)v; }

/* Number of values each command consumes per repetition. 0 for Z. */
static int svg_argc(char cmd)
{
    switch (cmd) {
    case 'M': case 'm': case 'L': case 'l': case 'T': case 't': return 2;
    case 'H': case 'h': case 'V': case 'v':                          return 1;
    case 'C': case 'c':                                              return 6;
    case 'S': case 's': case 'Q': case 'q':                          return 4;
    case 'Z': case 'z':                                              return 0;
    default:                                                         return -1;
    }
}

/* Appends one path's `d` string to `path`. Returns 0 on an unsupported
 * command, leaving whatever was built so far in place. */
static int svg_run(GpPath *path, const char *d)
{
    Svg s;
    char cmd = 0;
    double cx = 0, cy = 0;        /* current point                      */
    double sx = 0, sy = 0;        /* start of the current subpath      */
    double kx = 0, ky = 0;        /* last cubic control point (for S)  */
    double qx = 0, qy = 0;        /* last quadratic control point (for T) */
    int haveCubic = 0, haveQuad = 0, inFigure = 0;
    double a[6];

    s.p = d;

    for (;;) {
        int need, i, argc;

        /* A command letter starts a new run; otherwise the previous one
         * repeats (SVG lets "L 1 2 3 4" mean two line-tos). */
        if (svg_cmd(&s, &cmd)) {
            argc = svg_argc(cmd);
            if (argc < 0) {
                app_log("trayicon: unsupported SVG path command '%c'", cmd);
                return 0;
            }
            need = argc;
            if (!argc) {
                /* Z: close the figure and jump back to its start */
                if (inFigure) GdipClosePathFigure(path);
                inFigure = 0;
                cx = sx; cy = sy;
                haveCubic = haveQuad = 0;
                continue;
            }
        } else if (!cmd) {
            break;                    /* nothing left to read */
        } else {
            argc = svg_argc(cmd);
            /* No command letter, so the previous one repeats - unless that
             * previous one was a closepath, which takes no arguments and
             * therefore cannot repeat. That is the normal end of a path. */
            if (argc == 0) break;
            if (argc < 0) {
                app_log("trayicon: unsupported SVG path command '%c'", cmd);
                return 0;
            }
            need = argc;
        }

        for (i = 0; i < need; i++) {
            if (!svg_num(&s, &a[i])) {
                app_log("trayicon: SVG path command '%c' is missing an argument", cmd);
                return 0;
            }
        }

        {
            int rel = (cmd >= 'a' && cmd <= 'z');
            double ox = rel ? cx : 0.0, oy = rel ? cy : 0.0;
            double nx, ny;

            switch (cmd) {
            case 'M': case 'm':
                if (inFigure) GdipStartPathFigure(path);   /* implicit: GDI+
                                                             * starts one on the
                                                             * first point added */
                nx = ox + a[0]; ny = oy + a[1];
                GdipStartPathFigure(path);
                cx = sx = nx; cy = sy = ny;
                inFigure = 1;
                haveCubic = haveQuad = 0;
                /* Extra coordinate pairs after a moveto are implicit linetos. */
                cmd = rel ? 'l' : 'L';
                break;

            case 'L': case 'l':
                nx = ox + a[0]; ny = oy + a[1];
                GdipAddPathLine(path, gx2r(cx), gx2r(cy), gx2r(nx), gx2r(ny));
                cx = nx; cy = ny;
                haveCubic = haveQuad = 0;
                break;

            case 'H': case 'h':
                nx = ox + a[0];
                GdipAddPathLine(path, gx2r(cx), gx2r(cy), gx2r(nx), gx2r(cy));
                cx = nx;
                haveCubic = haveQuad = 0;
                break;

            case 'V': case 'v':
                ny = oy + a[0];
                GdipAddPathLine(path, gx2r(cx), gx2r(cy), gx2r(cx), gx2r(ny));
                cy = ny;
                haveCubic = haveQuad = 0;
                break;

            case 'C': case 'c': {
                double c1x = ox + a[0], c1y = oy + a[1];
                double c2x = ox + a[2], c2y = oy + a[3];
                nx = ox + a[4]; ny = oy + a[5];
                GdipAddPathBezier(path, gx2r(cx), gx2r(cy),
                                       gx2r(c1x), gx2r(c1y), gx2r(c2x), gx2r(c2y),
                                       gx2r(nx), gx2r(ny));
                kx = c2x; ky = c2y; haveCubic = 1; haveQuad = 0;
                cx = nx; cy = ny;
                break;
            }

            case 'S': case 's': {
                /* the first control point mirrors the previous one */
                double c1x = haveCubic ? (2 * cx - kx) : cx;
                double c1y = haveCubic ? (2 * cy - ky) : cy;
                double c2x = ox + a[0], c2y = oy + a[1];
                nx = ox + a[2]; ny = oy + a[3];
                GdipAddPathBezier(path, gx2r(cx), gx2r(cy),
                                       gx2r(c1x), gx2r(c1y), gx2r(c2x), gx2r(c2y),
                                       gx2r(nx), gx2r(ny));
                kx = c2x; ky = c2y; haveCubic = 1; haveQuad = 0;
                cx = nx; cy = ny;
                break;
            }

            case 'Q': case 'q': {
                /* GDI+ has no quadratic segment, so raise the degree: a
                 * quadratic through P0,Q,P1 equals a cubic whose controls sit
                 * two thirds of the way from each end towards Q. */
                double qcx = ox + a[0], qcy = oy + a[1];
                double c1x = cx + 2.0 / 3.0 * (qcx - cx);
                double c1y = cy + 2.0 / 3.0 * (qcy - cy);
                double c2x, c2y;
                nx = ox + a[2]; ny = oy + a[3];
                c2x = nx + 2.0 / 3.0 * (qcx - nx);
                c2y = ny + 2.0 / 3.0 * (qcy - ny);
                GdipAddPathBezier(path, gx2r(cx), gx2r(cy),
                                       gx2r(c1x), gx2r(c1y), gx2r(c2x), gx2r(c2y),
                                       gx2r(nx), gx2r(ny));
                qx = qcx; qy = qcy; haveQuad = 1; haveCubic = 0;
                cx = nx; cy = ny;
                break;
            }

            case 'T': case 't': {
                double qcx = haveQuad ? (2 * cx - qx) : cx;
                double qcy = haveQuad ? (2 * cy - qy) : cy;
                double c1x = cx + 2.0 / 3.0 * (qcx - cx);
                double c1y = cy + 2.0 / 3.0 * (qcy - cy);
                double c2x, c2y;
                nx = ox + a[0]; ny = oy + a[1];
                c2x = nx + 2.0 / 3.0 * (qcx - nx);
                c2y = ny + 2.0 / 3.0 * (qcy - ny);
                GdipAddPathBezier(path, gx2r(cx), gx2r(cy),
                                       gx2r(c1x), gx2r(c1y), gx2r(c2x), gx2r(c2y),
                                       gx2r(nx), gx2r(ny));
                qx = qcx; qy = qcy; haveQuad = 1; haveCubic = 0;
                cx = nx; cy = ny;
                break;
            }

            default:
                app_log("trayicon: unhandled SVG path command '%c'", cmd);
                return 0;
            }
        }
    }
    return 1;
}

/* Builds the whole logo as one GpPath. Caller frees with GdipDeletePath. */
static GpPath *logo_path(void)
{
    GpPath *p = NULL;
    int i;
    /* Empty path, alternate fill: the swirl is a subpath nested inside the
     * board, so even-odd is what cuts the hole out of it. GdipCreatePath is
     * the empty-path entry point - GdipCreatePath2 rejects NULL points. */
    if (GdipCreatePath(FillModeAlternate, &p) != Ok || !p) return NULL;
    for (i = 0; i < LOGO_PATH_N; i++) {
        if (!svg_run(p, LOGO_PATHS[i])) {
            GdipDeletePath(p);
            return NULL;
        }
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* icon assembly                                                       */
/* ------------------------------------------------------------------ */

/* How the tray icon is built, and why.
 *
 * The obvious approach - assemble a multi-resolution .ico in memory and hand
 * it to CreateIconFromResourceEx - does not work. The .ico this file produced
 * was valid (LoadImageW accepts the exact same bytes straight from a file),
 * but CreateIconFromResourceEx returned NULL for every combination of
 * dwVersion, nDesiredResource and cx/cy, and CreateIconFromResource failed the
 * same way, both without setting a last error. Verified against Win11: the
 * in-memory .ico entry point simply refuses these images here.
 *
 * So the icon is built the way GDI has always done it: a 32bpp colour DIB plus
 * a 1bpp AND mask, handed to CreateIconIndirect. Both bitmaps come straight
 * from logo_render's pixels - no temp file, no .ico bytes, nothing on disk.
 *
 * One size, not six. The tray is 16x16 at 100% DPI and 32x32 at 200%, so a
 * single 32x32 icon is exact at 200% and downsampled cleanly at 100% - which
 * is the opposite trade from the .ico route, where the sizes the shell does not
 * get to pick are wasted work anyway. */
#define TRAY_ICON_PX 32

/* Renders the logo at `size` px in `color` into a freshly xmalloc'd 32bpp BGRA
 * buffer (top-down, straight alpha, ready for the ICO). Returns bytes written. */
static size_t logo_render(GpPath *path, ARGB color, int size, BYTE **outBits)
{
    BYTE   *bits;
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    size_t  n = (size_t)size * (size_t)size * 4u;
    size_t  i;

    *outBits = NULL;
    bits = (BYTE *)xcalloc(n, 1);
    if (!bits) return 0;

    if (GdipCreateBitmapFromScan0(size, size, size * 4, PixelFormat32bppARGB,
                                  bits, &bmp) != Ok || !bmp) {
        xfree(bits);
        return 0;
    }
    if (GdipGetImageGraphicsContext((GpImage *)bmp, &g) != Ok || !g) {
        GdipDisposeImage((GpImage *)bmp);
        xfree(bits);
        return 0;
    }

    gx_set_quality(g);
    /* Scale the 100-unit artwork onto the icon box. PixelOffsetModeHalf (from
     * gx_set_quality) is what keeps a one-pixel stroke from disappearing. */
    GdipScaleWorldTransform(g, (REAL)size / LOGO_UNITS, (REAL)size / LOGO_UNITS,
                            MatrixOrderPrepend);
    gx_fill_path(g, color, path, FillModeAlternate);
    GdipDeleteGraphics(g);
    GdipDisposeImage((GpImage *)bmp);

    *outBits = bits;
    for (i = 0; i < n; i += 4) {
        /* GDI+ leaves premultiplied ARGB; an icon wants straight BGRA. */
        unsigned a = bits[i + 3];
        unsigned r = bits[i + 2], gch = bits[i + 1], b = bits[i];
        if (a == 0) { r = gch = b = 0; }
        else if (a < 255) {
            r = (r * 255u + a / 2) / a; if (r > 255) r = 255;
            gch = (gch * 255u + a / 2) / a; if (gch > 255) gch = 255;
            b = (b * 255u + a / 2) / a; if (b > 255) b = 255;
        }
        bits[i + 0] = (BYTE)b;
        bits[i + 1] = (BYTE)gch;
        bits[i + 2] = (BYTE)r;
        bits[i + 3] = (BYTE)a;
    }
    return n;
}

/* Builds a top-down DIB section of `px` x `px` at `bpp` bits. On success
 * *bitsOut points at the pixel rows and the caller owns the HBITMAP. */
static HBITMAP make_topdown_dib(int px, int bpp, void **bitsOut)
{
    BITMAPINFO bi;
    HDC dc;
    HBITMAP bmp = NULL;
    void *bits = NULL;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = px;
    bi.bmiHeader.biHeight      = -px;      /* negative = top-down rows */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = (WORD)bpp;
    bi.bmiHeader.biCompression = BI_RGB;

    dc = CreateCompatibleDC(NULL);
    if (!dc) return NULL;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    DeleteDC(dc);
    if (bmp && !bits) { DeleteObject(bmp); return NULL; }
    *bitsOut = bits;
    return bmp;
}

HICON trayicon_from_logo(ARGB color)
{
    const int px = TRAY_ICON_PX;
    const size_t colorStride = (size_t)px * 4u;
    const size_t colorBytes  = colorStride * (size_t)px;
    const size_t maskStride  = (((size_t)px + 31u) / 32u) * 4u;  /* 1bpp, 4-byte aligned */
    GpPath *path;
    BYTE   *rendered = NULL;      /* logo_render's own buffer, xmalloc'd */
    BYTE   *colorBits = NULL;
    BYTE   *maskBits = NULL;
    HBITMAP colorBmp = NULL, maskBmp = NULL;
    ICONINFO ii;
    HICON icon = NULL;
    int y;

    if (!gdiplus_start()) return NULL;
    path = logo_path();
    if (!path) {
        app_log("trayicon: could not build the logo path");
        return NULL;
    }
    if (!logo_render(path, color, px, &rendered) || !rendered) {
        GdipDeletePath(path);
        xfree(rendered);
        app_log("trayicon: render at %d px failed", px);
        return NULL;
    }
    GdipDeletePath(path);

    /* logo_render already produces top-down straight-alpha BGRA, which is the
     * colour DIB's own layout, so the rows are copied straight across. */
    colorBmp = make_topdown_dib(px, 32, (void **)&colorBits);
    if (!colorBmp) { xfree(rendered); return NULL; }
    memcpy(colorBits, rendered, colorBytes);
    xfree(rendered);

    /* The AND mask is 1bpp, and in it 1 = opaque / 0 = transparent - the
     * inverse of what most people expect, but it is what ICONINFO.hbmMask
     * means. Taking the shape from the alpha channel keeps the two bitmaps in
     * agreement, which is what stops a halo appearing around the artwork. */
    maskBmp = make_topdown_dib(px, 1, (void **)&maskBits);
    if (!maskBmp) { DeleteObject(colorBmp); return NULL; }
    memset(maskBits, 0, maskStride * (size_t)px);
    for (y = 0; y < px; y++) {
        BYTE       *mrow = maskBits + (size_t)y * maskStride;
        const BYTE *crow = colorBits + (size_t)y * colorStride;
        int x;
        for (x = 0; x < px; x++) {
            /* 1bpp rows are packed MSB first: pixel x is bit (7 - x%8) of
             * byte (x / 8). */
            if (crow[(size_t)x * 4u + 3u] != 0)
                mrow[x >> 3] |= (BYTE)(0x80u >> (x & 7));
        }
    }

    memset(&ii, 0, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask  = maskBmp;
    icon = CreateIconIndirect(&ii);
    if (!icon) app_log("trayicon: CreateIconIndirect failed (win32 %lu)",
                       (unsigned long)GetLastError());

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

/* The inverse of the unpremultiply in logo_render: a GDI+ bitmap created over
 * caller memory in PixelFormat32bppARGB is read as *premultiplied*, so handing
 * it logo_render's straight-alpha pixels would wash out every antialiased edge
 * (a half-covered purple pixel is stored as its full-strength colour, which
 * GDI+ then treats as "premultiplied" and brightens). The .ico path wants
 * straight alpha; the GDI+/PNG path wants premultiplied. */
static void premultiply(BYTE *bits, size_t n)
{
    size_t i;
    for (i = 0; i < n; i += 4) {
        unsigned a = bits[i + 3];
        if (a == 0 || a == 255) continue;
        bits[i + 0] = (BYTE)((bits[i + 0] * a + 127u) / 255u);
        bits[i + 1] = (BYTE)((bits[i + 1] * a + 127u) / 255u);
        bits[i + 2] = (BYTE)((bits[i + 2] * a + 127u) / 255u);
    }
}

/* Writes the logo at `size` px to `path` as a PNG, for the places an .ico is
 * not what you want (taskbar pins, shortcut icons, docs). Returns 1 on
 * success. */
int trayicon_export_png(const wchar_t *path, ARGB color, int size)
{
    GpPath *p;
    BYTE   *bits = NULL;
    size_t  n;
    int     ok;

    if (!path || size < 8 || size > 1024) {
        app_log("trayicon: bad PNG request (size=%d)", size);
        return 0;
    }
    if (!gdiplus_start()) { app_log("trayicon: GDI+ unavailable"); return 0; }
    p = logo_path();
    if (!p) { app_log("trayicon: logo path unavailable"); return 0; }
    n = logo_render(p, color, size, &bits);
    GdipDeletePath(p);
    if (!n || !bits) { app_log("trayicon: render at %d px failed", size); return 0; }
    premultiply(bits, n);
    /* logo_render hands back top-down rows, which is the layout the PNG
     * encoder wants, so no flip is needed here. */
    ok = gdiplus_save_png_from_bits(bits, size, size, size * 4, path);
    if (!ok) app_log("trayicon: could not write the PNG");
    xfree(bits);
    return ok;
}

/* ------------------------------------------------------------------ */
/* the icon embedded in the executables                                 */
/* ------------------------------------------------------------------ */

/* The window class holds its icons for the life of the process, so there is
 * nothing to free - and LoadImageW, unlike LoadIconW, hands back a handle the
 * caller owns. Loading per call would leak two handles every time a class is
 * registered, and the self-tests register theirs more than once to measure
 * exactly that kind of leak. Two slots, loaded once: the large icon (cx 0) and
 * the small one. */
static HICON g_appIcon[2];

HICON app_icon_load(HINSTANCE inst, int cx)
{
    const int slot = cx ? 1 : 0;
    HICON icon;

    if (g_appIcon[slot]) return g_appIcon[slot];
    icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(APP_ICON_RES), IMAGE_ICON,
                             cx, cx, cx ? 0 : LR_DEFAULTSIZE);
    if (!icon) {
        app_log("app icon: resource %d not embedded, using the system icon",
                APP_ICON_RES);
        icon = LoadIconW(inst, (LPCWSTR)(ULONG_PTR)(UINT)32512);
    }
    /* Cached even on the fallback path, so the "no resource" warning is logged
     * once rather than on every registration. */
    g_appIcon[slot] = icon;
    return icon;
}

/* One .ico is a directory followed by the images themselves:
 *
 *   ICONDIR            reserved, type (1 = icon), image count
 *   ICONDIRENTRY  x n  width, height, palette, planes, bit count, size, offset
 *   image data    x n  either a DIB or a whole PNG
 *
 * The sizes are the ones Windows actually asks for: the taskbar and Alt+Tab
 * want 16/24/32, the Explorer's large view and a pinned shortcut want 48 up to
 * 256. Anything the shell does not ask for is dead weight in the .ico each exe
 * embeds, so the list stops there.
 *
 * 16..48 are stored as DIBs - a BITMAPINFOHEADER with biHeight doubled to
 * cover the AND mask, which is what every shell has read since NT. 64 and up
 * go out as PNG, because a 256px DIB would add a megabyte of uncompressed
 * pixels to an executable that is otherwise 270KB. */
static const int ICO_SIZES[] = { 16, 24, 32, 48, 64, 128, 256 };
#define ICO_N ((int)(sizeof(ICO_SIZES) / sizeof(ICO_SIZES[0])))
#define ICO_DIR_BYTES (6u + 16u * (unsigned)ICO_N)

/* Packs logo_render's top-down BGRA pixels into a DIB-shaped .ico entry: the
 * 40-byte header, the colour rows bottom-up, then the 1bpp AND mask (1 =
 * opaque, rows padded to a 4-byte boundary). Returns the entry, xmalloc'd, and
 * its length in *outLen. */
static BYTE *ico_dib_entry(const BYTE *topdown, int px, DWORD *outLen)
{
    const DWORD headerSize = 40u;                       /* BITMAPINFOHEADER */
    const DWORD xorBytes   = (DWORD)px * (DWORD)px * 4u;
    const DWORD maskStride = (((DWORD)px + 31u) / 32u) * 4u;
    const size_t total = headerSize + xorBytes + maskStride * (DWORD)px;
    BITMAPINFOHEADER bih;
    BYTE *entry;
    BYTE *mask;
    int   y, x;

    entry = (BYTE *)xcalloc(total, 1);
    if (!entry) return NULL;

    memset(&bih, 0, sizeof(bih));
    bih.biSize        = headerSize;
    bih.biWidth       = px;
    bih.biHeight      = px * 2;   /* colour rows plus the mask rows */
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biCompression = BI_RGB;
    memcpy(entry, &bih, headerSize);

    /* A DIB's first row is the artwork's bottom row, so both loops walk y
     * backwards against the top-down source. */
    for (y = 0; y < px; y++) {
        const BYTE *src = topdown + (size_t)(px - 1 - y) * (size_t)px * 4u;
        memcpy(entry + headerSize + (size_t)y * (size_t)px * 4u, src,
               (size_t)px * 4u);
    }
    mask = entry + headerSize + xorBytes;
    for (y = 0; y < px; y++) {
        BYTE       *mrow = mask + (size_t)y * maskStride;
        const BYTE *crow = topdown + (size_t)(px - 1 - y) * (size_t)px * 4u;
        for (x = 0; x < px; x++) {
            if (crow[(size_t)x * 4u + 3u] != 0)
                mrow[x >> 3] |= (BYTE)(0x80u >> (x & 7));
        }
    }
    *outLen = (DWORD)total;
    return entry;
}

static int ico_write_all(HANDLE f, const void *data, DWORD len)
{
    DWORD written = 0;
    return WriteFile(f, data, len, &written, NULL) && written == len;
}

int trayicon_write_ico(const wchar_t *path, ARGB color)
{
    struct { BYTE *data; DWORD len; } img[ICO_N];
    BYTE    dir[6 + 16 * ICO_N];
    GpPath *p = NULL;
    HANDLE  f = INVALID_HANDLE_VALUE;
    DWORD   offset = ICO_DIR_BYTES;
    int     i, ok = 0;

    if (!path) return 0;
    for (i = 0; i < ICO_N; i++) { img[i].data = NULL; img[i].len = 0; }

    if (!gdiplus_start()) { app_log("trayicon: GDI+ unavailable"); return 0; }
    p = logo_path();
    if (!p) { app_log("trayicon: could not build the logo path"); return 0; }

    for (i = 0; i < ICO_N; i++) {
        const int size = ICO_SIZES[i];
        BYTE *bits = NULL;
        size_t n = logo_render(p, color, size, &bits);
        if (!n || !bits) {
            app_log("trayicon: render at %d px failed", size);
            goto done;
        }
        if (size < 64) {
            img[i].data = ico_dib_entry(bits, size, &img[i].len);
        } else {
            unsigned char *png = NULL;
            size_t pngLen = 0;
            /* PNG wants premultiplied alpha; see premultiply() above. */
            premultiply(bits, n);
            if (!gdiplus_encode_png(bits, size, size, size * 4, &png, &pngLen)) {
                app_log("trayicon: PNG encode at %d px failed", size);
                xfree(bits);
                goto done;
            }
            img[i].data = png;
            img[i].len  = (DWORD)pngLen;
        }
        xfree(bits);
        if (!img[i].data) goto done;
    }

    memset(dir, 0, sizeof(dir));
    dir[2] = 1;                          /* type: icon */
    dir[4] = (BYTE)ICO_N;
    for (i = 0; i < ICO_N; i++) {
        BYTE *e = dir + 6 + 16 * i;
        e[0] = (BYTE)(ICO_SIZES[i] >= 256 ? 0 : ICO_SIZES[i]);  /* 0 = 256 */
        e[1] = e[0];                                            /* square   */
        e[2] = 0;                                              /* no palette */
        e[3] = 0;
        e[4] = 1;  e[5] = 0;                                   /* planes    */
        e[6] = 32; e[7] = 0;                                   /* bit count */
        memcpy(e + 8,  &img[i].len, 4);
        memcpy(e + 12, &offset,   4);
        offset += img[i].len;
    }

    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        app_log("trayicon: could not open %ls for writing", path);
        goto done;
    }
    if (!ico_write_all(f, dir, sizeof(dir))) goto done;
    for (i = 0; i < ICO_N; i++) {
        if (!ico_write_all(f, img[i].data, img[i].len)) goto done;
    }
    ok = 1;

done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    for (i = 0; i < ICO_N; i++) xfree(img[i].data);
    if (p) GdipDeletePath(p);
    if (!ok) app_log("trayicon: could not write the .ico");
    return ok;
}
