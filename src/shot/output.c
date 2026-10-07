/* output.c - clipboard and PNG file output.
 *
 * The clipboard gets several formats at once so the image lands at full
 * quality wherever it is pasted:
 *   CF_DIB        - Office, GIMP, most native editors
 *   "PNG"         - browsers, chat clients, VS Code
 *   "HTML Format" - Word / rich text, references a temp PNG
 *   CF_UNICODETEXT - fallback, describes the capture
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"
#include <gdiplus.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Composite base + annotation layer into a fresh 32bpp bitmap.
 *
 * The result is drawn over a buffer we allocate, and that buffer is handed
 * back through *bitsOut. Both PNG encoding and the clipboard's HTML flavour
 * need the finished pixels, and GdipBitmapLockBits is not usable on this
 * build - it returns InvalidParameter for every bitmap - so reading them
 * straight out of our own memory is the only way that works. Caller xfree()s
 * *bitsOut. */
static GpBitmap *compose(ShotApp *a, BYTE **bitsOut)
{
    GpBitmap *result = NULL, *layerCopy = NULL, *bmp = NULL;
    GpGraphics *g = NULL;
    BYTE *bits;
    int w, h;

    *bitsOut = NULL;
    if (!a->haveSel || !a->baseBmp) return NULL;
    w = a->baseW; h = a->baseH;
    if (w <= 0 || h <= 0) return NULL;

    bits = (BYTE *)xcalloc((size_t)w * (size_t)h * 4u, 1);
    if (!bits) return NULL;
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  bits, &result) != Ok || !result) {
        xfree(bits);
        return NULL;
    }
    if (GdipGetImageGraphicsContext((GpImage *)result, &g) != Ok) {
        GdipDisposeImage((GpImage *)result);
        xfree(bits);
        return NULL;
    }
    GdipSetInterpolationMode(g, InterpolationModeNearestNeighbor);
    gx_clear(g, 0xFF000000, (REAL)w, (REAL)h);
    GdipSetInterpolationMode(g, InterpolationModeNearestNeighbor);
    if (GdipCreateBitmapFromHBITMAP(a->baseBmp, NULL, &bmp) == Ok) {
        GdipDrawImageRectI(g, (GpImage *)bmp, 0, 0, w, h);
        GdipDisposeImage((GpImage *)bmp);
    }
    if (a->layer) {
        if (GdipCloneBitmapAreaI(0, 0, w, h, PixelFormat32bppPARGB,
                                 (GpBitmap *)a->layer, &layerCopy) == Ok) {
            GdipDrawImageRectI(g, (GpImage *)layerCopy, 0, 0, w, h);
            GdipDisposeImage((GpImage *)layerCopy);
        }
    }
    GdipDeleteGraphics(g);
    *bitsOut = bits;
    return result;
}

/* Encodes an image that we drew over a buffer of our own. `w`/`h`/`stride`
 * describe that buffer, not the bitmap. */
static int encode_bits(const BYTE *bits, int w, int h, int stride,
                       unsigned char **png, size_t *len)
{
    *png = NULL; *len = 0;
    if (!bits || w <= 0 || h <= 0) return 0;
    return gdiplus_encode_png(bits, w, h, stride, png, len);
}

wchar_t *shot_temp_png_path(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH];
    wchar_t *name;
    if (!GetTempPathW(MAX_PATH, tmp)) return NULL;
    if (!PathCombineW(dir, tmp, L"shotclip")) return NULL;
    CreateDirectoryW(dir, NULL);
    name = wcs_format(L"%s\\%lu.png", dir, (unsigned long)GetCurrentProcessId());
    if (!name) return NULL;
    wcsncpy(path, name, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    xfree(name);
    return xwcsdup(path);
}

/* Remove stale temp captures from previous runs. */
void shot_cleanup_temp(void)
{
    wchar_t dir[MAX_PATH], tmp[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    if (!PathCombineW(dir, tmp, L"shotclip")) return;
    if (!PathCombineW(pat, dir, L"*.png")) return;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!PathCombineW(f, dir, fd.cFileName)) continue;
        DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ------------------------------------------------------------------ */
/* CF_HTML                                                             */
/* ------------------------------------------------------------------ */

/* Builds the CF_HTML byte stream for one captured image.
 *
 * The four offsets in the header are byte offsets into the UTF-8 document,
 * measured after conversion - a path with Chinese in it is more bytes than
 * characters, so counting wide chars (or doubling them, as if every char were
 * two bytes) lands every offset in the wrong place and Word reads garbage.
 * The header itself is pure ASCII, so its byte length equals its char count,
 * and the digit fields are located by searching for their names rather than
 * by hard-coding positions that the header text would have to keep in step
 * with. */
static const wchar_t *CFHTML_HEAD =
    L"Version:0.9\r\nStartHTML:0000000100\r\nEndHTML:0000000000"
    L"\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n";
static const wchar_t *CFHTML_PRE  = L"<html><body><!--StartFragment-->";
static const wchar_t *CFHTML_POST = L"<!--EndFragment--></body></html>";
static const wchar_t *CFHTML_TAIL = L"Version:0.9\r\n";

static size_t utf8_bytes(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    return n > 0 ? (size_t)n - 1 : 0;    /* exclude the NUL, like strlen */
}

int shot_build_cfhtml(const wchar_t *imgPath, int w, int h,
                      char **bytesOut, size_t *lenOut)
{
    wchar_t *img = NULL;
    char *bytes = NULL;
    size_t headB, preB, imgB, postB, tailB, total;
    int i;

    *bytesOut = NULL; *lenOut = 0;
    if (!imgPath) return 0;
    img = wcs_format(L"<img src=\"file:///%s\" width=\"%d\" height=\"%d\">",
                     imgPath, w, h);
    if (!img) return 0;

    headB = utf8_bytes(CFHTML_HEAD);
    preB  = utf8_bytes(CFHTML_PRE);
    imgB  = utf8_bytes(img);
    postB = utf8_bytes(CFHTML_POST);
    tailB = utf8_bytes(CFHTML_TAIL);
    total = headB + preB + imgB + postB + tailB;

    bytes = (char *)xmalloc(total + 1);
    if (!bytes) { xfree(img); return 0; }
    {
        /* convert each segment into its exact slot */
        const wchar_t *segS[5] = { CFHTML_HEAD, CFHTML_PRE, img,
                                   CFHTML_POST, CFHTML_TAIL };
        const size_t   segB[5] = { headB, preB, imgB, postB, tailB };
        size_t off = 0;
        for (i = 0; i < 5; i++) {
            int written = WideCharToMultiByte(CP_UTF8, 0, segS[i],
                                              (int)wcslen(segS[i]),
                                              bytes + off, (int)segB[i],
                                              NULL, NULL);
            if (written <= 0 || (size_t)written != segB[i]) {
                xfree(bytes); xfree(img);
                return 0;
            }
            off += segB[i];
        }
        bytes[total] = 0;
    }
    xfree(img);

    /* Rewrite the four placeholder offsets. Each digit field is fixed width,
     * so patching them does not move anything downstream. The header is pure
     * ASCII, so its field names can be searched for as plain bytes. */
    {
        static const char *FIELDS[4] = {
            "StartHTML:", "EndHTML:", "StartFragment:", "EndFragment:"
        };
        size_t vals[4];
        vals[0] = headB;                                   /* StartHTML */
        vals[1] = total;                                   /* EndHTML   */
        vals[2] = headB + preB;                            /* StartFragment */
        vals[3] = headB + preB + imgB;                     /* EndFragment   */
        for (i = 0; i < 4; i++) {
            char digits[16];
            char *at = strstr(bytes, FIELDS[i]);
            if (!at) continue;
            at += strlen(FIELDS[i]);
            /* into a scratch buffer first: the %010d would otherwise write its
             * terminating NUL over the \r that follows the field, truncating
             * the header string for every strstr after this one. Ten digits,
             * because that is the width of the placeholders in CFHTML_HEAD. */
            _snprintf_s(digits, sizeof(digits), _TRUNCATE, "%010d", (int)vals[i]);
            memcpy(at, digits, 10);
        }
    }
    *bytesOut = bytes;
    *lenOut = total;
    return 1;
}

static int put_html(HWND owner, const wchar_t *imgPath, int w, int h)
{
    UINT fmtId = RegisterClipboardFormatW(L"HTML Format");
    char *bytes = NULL;
    size_t len = 0;
    HGLOBAL gh;
    void *p;
    int ok = 0;

    if (!imgPath || !fmtId) return 0;
    if (!shot_build_cfhtml(imgPath, w, h, &bytes, &len)) return 0;

    gh = GlobalAlloc(GMEM_MOVEABLE, len);
    if (gh) {
        p = GlobalLock(gh);
        if (p) { memcpy(p, bytes, len); GlobalUnlock(gh); }
        else   { GlobalFree(gh); gh = NULL; }
    }
    xfree(bytes);
    if (!gh) return 0;
    if (SetClipboardData(fmtId, gh)) ok = 1;
    else GlobalFree(gh);
    (void)owner;
    return ok;
}

/* ------------------------------------------------------------------ */

int shot_copy_to_clipboard(ShotApp *a)
{
    GpBitmap *img;
    BYTE  *bits = NULL;
    int    stride = 0;
    unsigned char *png = NULL;
    size_t pngLen = 0;
    HBITMAP dib = NULL, dup = NULL;
    wchar_t *tmpPath = NULL;
    wchar_t *info = NULL;
    int ok = 0;

    img = compose(a, &bits);
    if (!img) { xfree(bits); return 0; }
    stride = a->baseW * 4;

    if (!OpenClipboard(a->overlay)) {
        GdipDisposeImage((GpImage *)img); xfree(bits); return 0;
    }
    EmptyClipboard();

    /* CF_DIB */
    if (GdipCreateHBITMAPFromBitmap(img, &dib, 0) == Ok && dib) {
        dup = clone_dib_object(dib);
        if (dup) {
            HGLOBAL g = dib_to_global(dup);
            if (g) {
                if (SetClipboardData(CF_DIB, g)) ok = 1;
                else GlobalFree(g);
            }
            DeleteObject(dup);
        }
        DeleteObject(dib);
    }

    /* PNG (also written to disk so CF_HTML can reference it) */
    if (encode_bits(bits, a->baseW, a->baseH, stride, &png, &pngLen)) {
        UINT fmt = RegisterClipboardFormatW(L"PNG");
        if (fmt) {
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, pngLen);
            if (g) {
                void *p = GlobalLock(g);
                if (p) { memcpy(p, png, pngLen); GlobalUnlock(g); }
                else   { GlobalFree(g); g = NULL; }
                if (g) {
                    if (SetClipboardData(fmt, g)) ok = 1;
                    else GlobalFree(g);
                }
            }
        }
        tmpPath = shot_temp_png_path();
        if (tmpPath)
            gdiplus_save_png_from_bits(bits, a->baseW, a->baseH, stride, tmpPath);
    }
    xfree(png);

    /* CF_HTML */
    if (tmpPath) {
        wchar_t uri[MAX_PATH * 2];
        size_t k;
        _snwprintf_s(uri, MAX_PATH * 2, _TRUNCATE, L"%s", tmpPath);
        for (k = 0; uri[k]; k++) if (uri[k] == L'\\') uri[k] = L'/';
        if (put_html(a->overlay, uri, a->baseW, a->baseH)) ok = 1;
    }

    info = wcs_format(L"截图 %d × %d  (%d, %d)", a->baseW, a->baseH,
                      a->sel.left, a->sel.top);
    if (info) {
        size_t bytes = (wcslen(info) + 1) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (g) {
            wchar_t *p = (wchar_t *)GlobalLock(g);
            if (p) { memcpy(p, info, bytes); GlobalUnlock(g); }
            else   { GlobalFree(g); g = NULL; }
            if (g) {
                if (SetClipboardData(CF_UNICODETEXT, g)) ok = 1;
                else GlobalFree(g);
            }
        }
        xfree(info);
    }

    CloseClipboard();
    xfree(tmpPath);
    GdipDisposeImage((GpImage *)img);
    xfree(bits);
    return ok;
}

int shot_save_to_file(ShotApp *a, const wchar_t *path)
{
    BYTE *bits = NULL;
    GpBitmap *img;
    int ok = 0;

    img = compose(a, &bits);
    if (!img || !path) {
        if (img) GdipDisposeImage((GpImage *)img);
        xfree(bits);
        return 0;
    }
    if (a->baseW > 0 && a->baseH > 0)
        ok = gdiplus_save_png_from_bits(bits, a->baseW, a->baseH,
                                        a->baseW * 4, path);
    GdipDisposeImage((GpImage *)img);
    xfree(bits);
    return ok;
}

wchar_t *shot_default_filename(void)
{
    wchar_t path[MAX_PATH];
    wchar_t dir[MAX_PATH];
    SYSTEMTIME st;
    wchar_t name[64];

    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, dir)))
        wcsncpy(dir, L".", MAX_PATH);
    dir[MAX_PATH - 1] = 0;
    GetLocalTime(&st);
    _snwprintf_s(name, 64, _TRUNCATE,
                 L"截图_%04d%02d%02d_%02d%02d%02d.png",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    if (!PathCombineW(path, dir, name)) wcsncpy(path, name, MAX_PATH);
    return xwcsdup(path);
}

int shot_finish_and_copy(ShotApp *a)
{
    int r;
    if (!a->haveSel) return 0;
    if (a->baseW < 3 || a->baseH < 3) return 0;
    r = shot_copy_to_clipboard(a);
    shot_cancel(a);
    return r;
}
