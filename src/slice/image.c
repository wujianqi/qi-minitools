/* image.c - getting pixels in and out of the slicer.
 *
 * The one rule everything else here follows: pixels are a top-down 32bpp BGRA
 * buffer, and row 0 of that buffer is the top row of the picture. A DIB
 * section built by make_dib() with a negative height is already in that order,
 * and GDI+ is told the same, so the canvas, the crop and the encoder all agree
 * about which way is up without any of them converting.
 */
#include "slice.h"

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

int img_is_supported(const wchar_t *path)
{
    const wchar_t *dot;
    if (!path || !*path) return 0;
    dot = wcsrchr(path, L'.');
    if (!dot) return 0;
    dot++;
    if (wcs_ieq(dot, L"png")) return 1;
    if (wcs_ieq(dot, L"jpg") || wcs_ieq(dot, L"jpeg")) return 1;
    return 0;
}

wchar_t *img_stem(const wchar_t *path)
{
    const wchar_t *b, *dot;
    WBuf out;
    if (!path || !*path) return xwcsdup(L"slice");
    b = path + wcslen(path);
    while (b > path && b[-1] != L'\\' && b[-1] != L'/') b--;
    dot = wcsrchr(b, L'.');
    if (!dot) return xwcsdup(b);
    wbuf_init(&out);
    wbuf_addn(&out, b, (size_t)(dot - b));
    if (out.len == 0) { wbuf_free(&out); return xwcsdup(L"slice"); }
    return xwcsdup(out.buf);
}

wchar_t *img_default_dir(const wchar_t *path)
{
    wchar_t *stem, *dir;
    const wchar_t *slash;
    WBuf b;
    size_t cut = 0;

    wbuf_init(&b);
    if (path && *path) {
        slash = path + wcslen(path);
        while (slash > path && slash[-1] != L'\\' && slash[-1] != L'/') slash--;
        /* The loop stops with `slash` on the first character *after* the
         * separator, so the folder is slash - path characters long - the
         * separator included. Adding one more dragged the first letter of the
         * file name along and produced "C:\pics\hholiday_slices". */
        if (*slash) cut = (size_t)(slash - path);
        wbuf_addn(&b, path, cut);
    }
    stem = img_stem(path);
    wbuf_add(&b, stem);
    wbuf_add(&b, L"_slices");
    xfree(stem);
    dir = xwcsdup(b.buf);
    wbuf_free(&b);
    return dir;
}

int img_mkdir_p(const wchar_t *path)
{
    wchar_t buf[MAX_PATH];
    wchar_t *p;
    DWORD a;
    int start = 3;

    if (!path || !*path) return 0;
    wcsncpy(buf, path, MAX_PATH - 1);
    buf[MAX_PATH - 1] = 0;

    a = GetFileAttributesW(buf);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return 1;

    /* Trailing separators are trimmed first: the scan below would otherwise
     * walk straight past the end creating "C:\out\" and only then get to the
     * real leaf. */
    p = buf + wcslen(buf);
    while (p > buf && (p[-1] == L'\\' || p[-1] == L'/')) *--p = 0;
    if (!*buf) return 0;

    /* A UNC path's \\server\share prefix cannot be created and must not be
     * chopped at the first backslash, so the scan starts past the share. */
    if (buf[0] == L'\\' && buf[1] == L'\\') {
        const wchar_t *q = buf + 2;
        int segs = 0;
        while (*q && segs < 2) { if (*q == L'\\') segs++; q++; }
        start = (int)(q - buf);
    }

    for (p = buf + start; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t save = *p;
            *p = 0;
            CreateDirectoryW(buf, NULL);
            *p = save;
        }
    }
    if (!CreateDirectoryW(buf, NULL)) {
        /* A parallel run of the app, or a second export into the same
         * folder, lands here - the directory existing is success. */
        a = GetFileAttributesW(buf);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return 1;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static int scan_alpha(const void *bits, int w, int h, int stride)
{
    const BYTE *p = (const BYTE *)bits;
    int x, y;
    for (y = 0; y < h; y++) {
        const BGRAPX *row = (const BGRAPX *)(p + (size_t)y * stride);
        for (x = 0; x < w; x++)
            if (row[x].a != 255) return 1;
    }
    return 0;
}

void img_free(SourceImage *s)
{
    if (!s) return;
    if (s->gp) { GdipDisposeImage((GpImage *)s->gp); s->gp = NULL; }
    if (s->dib) { free_dib(s->dib, s->bits); s->dib = NULL; }
    s->bits = NULL;
    s->w = s->h = s->stride = 0;
    s->path[0] = 0;
}

int img_load(SourceImage *s, const wchar_t *path)
{
    GpImage *loaded = NULL;
    GpBitmap *flat = NULL, *view = NULL;
    GpGraphics *g = NULL;
    BitmapData bd;
    void *bits = NULL;
    HBITMAP dib = NULL;
    UINT uw = 0, uh = 0;
    int w, h, y, ok = 0;

    if (!s || !path || !*path) return 0;
    memset(s, 0, sizeof(*s));
    if (!gdiplus_ok()) return 0;

    if (GdipLoadImageFromFile(path, &loaded) != Ok || !loaded) return 0;
    if (GdipGetImageWidth(loaded, &uw) != Ok) goto done;
    if (GdipGetImageHeight(loaded, &uh) != Ok) goto done;
    w = (int)uw;
    h = (int)uh;
    if (w < 1 || h < 1 || w > IMG_MAX_DIM || h > IMG_MAX_DIM ||
        (long long)w * (long long)h > IMG_MAX_PIXELS)
        goto done;

    /* Normalise whatever the decoder produced into 32bpp ARGB. SourceCopy
     * rather than SourceOver: a transparent PNG must stay transparent, and
     * compositing onto the cleared surface would flatten the alpha instead. */
    if (GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppARGB, NULL, &flat) != Ok)
        goto done;
    if (GdipGetImageGraphicsContext((GpImage *)flat, &g) != Ok) goto done;
    GdipSetCompositingMode(g, CompositingModeSourceCopy);
    GdipSetInterpolationMode(g, InterpolationModeNearestNeighbor);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
    if (GdipDrawImageRectI(g, loaded, 0, 0, w, h) != Ok) goto done;
    GdipDeleteGraphics(g);
    g = NULL;

    /* Copy out of GDI+'s buffer into a DIB section we own. LockBits hands back
     * a stride that is padded, and the locked memory must not be handed to a
     * second GpBitmap or used after the unlock - so the rows are copied once
     * and the only thing built over them afterwards is our own view. */
    memset(&bd, 0, sizeof(bd));
    if (GdipBitmapLockBits(flat, NULL, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
        goto done;
    if (!bd.Scan0) { GdipBitmapUnlockBits(flat, &bd); goto done; }
    dib = make_dib(w, h, 32, &bits);
    if (!dib) { GdipBitmapUnlockBits(flat, &bd); goto done; }
    for (y = 0; y < h; y++)
        memcpy((BYTE *)bits + (size_t)y * w * 4,
               (const BYTE *)bd.Scan0 + (size_t)y * bd.Stride,
               (size_t)w * 4);
    GdipBitmapUnlockBits(flat, &bd);

    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  (BYTE *)bits, &view) != Ok) {
        free_dib(dib, bits);
        dib = NULL;
        goto done;
    }

    s->dib = dib;
    s->bits = bits;
    s->gp = view;
    s->stride = w * 4;
    s->w = w;
    s->h = h;
    s->hasAlpha = scan_alpha(bits, w, h, w * 4);
    wcsncpy(s->path, path, MAX_PATH - 1);
    s->path[MAX_PATH - 1] = 0;
    ok = 1;

done:
    if (g) GdipDeleteGraphics(g);
    if (flat) GdipDisposeImage((GpImage *)flat);
    if (loaded) GdipDisposeImage(loaded);
    if (!ok && s->gp) { GdipDisposeImage((GpImage *)s->gp); s->gp = NULL; }
    if (!ok && s->dib) { free_dib(s->dib, s->bits); s->dib = NULL; s->bits = NULL; }
    return ok;
}

/* ------------------------------------------------------------------ */
/* saving one cell                                                     */
/* ------------------------------------------------------------------ */

int img_save_rect(const SourceImage *s, RECT r, const wchar_t *path,
                  int asJpeg, int quality)
{
    GpBitmap *view = NULL, *out = NULL;
    GpGraphics *g = NULL;
    GpSolidFill *bg = NULL;
    void *bits = NULL, *obits = NULL;
    HBITMAP dib = NULL, odib = NULL;
    int w, h, y, ok = 0;

    if (!s || !s->bits || !s->gp || !path || !*path) return 0;

    if (r.left   < 0)                 r.left   = 0;
    if (r.top    < 0)                 r.top    = 0;
    if (r.right  > s->w)              r.right  = s->w;
    if (r.bottom > s->h)              r.bottom = s->h;
    w = r.right - r.left;
    h = r.bottom - r.top;
    if (w < 1 || h < 1) return 0;

    dib = make_dib(w, h, 32, &bits);
    if (!dib) return 0;
    for (y = 0; y < h; y++)
        memcpy((BYTE *)bits + (size_t)y * w * 4,
               (const BYTE *)s->bits + (size_t)(r.top + y) * s->stride
                                  + (size_t)r.left * 4,
               (size_t)w * 4);

    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  (BYTE *)bits, &view) != Ok) {
        free_dib(dib, bits);
        return 0;
    }

    if (!asJpeg) {
        ok = gdiplus_save_buffer(bits, w, h, w * 4, path, L"image/png", quality);
        goto done;
    }

    /* JPEG has no alpha channel. A transparent PNG cell would encode to solid
     * black there, so the cell is re-composited onto white first - which is
     * what a user asking for JPEG almost always means by "no background". */
    odib = make_dib(w, h, 32, &obits);
    if (!odib) goto done;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  (BYTE *)obits, &out) != Ok)
        goto done;
    if (GdipGetImageGraphicsContext((GpImage *)out, &g) != Ok) goto done;
    if (GdipCreateSolidFill(0xFFFFFFFF, &bg) != Ok) goto done;
    if (GdipFillRectangle(g, bg, 0, 0, (REAL)w, (REAL)h) != Ok) goto done;
    GdipSetCompositingMode(g, CompositingModeSourceOver);
    if (GdipDrawImageRectI(g, (GpImage *)view, 0, 0, w, h) != Ok) goto done;
    ok = gdiplus_save_buffer(obits, w, h, w * 4, path, L"image/jpeg", quality);

done:
    if (bg) GdipDeleteBrush((GpBrush *)bg);
    if (g) GdipDeleteGraphics(g);
    if (out) GdipDisposeImage((GpImage *)out);
    if (odib) { free_dib(odib, obits); }
    if (view) GdipDisposeImage((GpImage *)view);
    if (dib) { free_dib(dib, bits); }
    return ok;
}
