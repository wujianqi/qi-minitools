/* winutil.h - shared infrastructure for every tool in src.
 *
 * Provides: DPI awareness, leak-tracked allocation, resource snapshotting,
 * small string/list helpers, GDI+ startup/shutdown.
 */
#ifndef WINUTIL_H
#define WINUTIL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7 */
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* leak-tracked allocation                                             */
/* Every heap allocation in these apps goes through these so that a      */
/* mismatch is detected immediately and at exit.                         */
/* ------------------------------------------------------------------ */
void  *xmalloc(size_t n);
void  *xcalloc(size_t n, size_t sz);
void  *xrealloc(void *p, size_t n);
void   xfree(void *p);
char  *xstrdup(const char *s);
wchar_t *xwcsdup(const wchar_t *s);

/* live allocation count; must return to the value it had at check time */
long   xalloc_live(void);
long   xalloc_total(void);
void   xalloc_reset_totals(void);

/* ------------------------------------------------------------------ */
/* resource accounting                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    DWORD gdiObjects;    /* GetGuiResources(GR_GDIOBJECTS)  */
    DWORD userObjects;   /* GetGuiResources(GR_USEROBJECTS) */
    DWORD handles;       /* GetProcessHandleCount          */
    long  allocs;        /* xalloc_live()                  */
} ResSnapshot;

void res_snapshot(ResSnapshot *out);
/* returns TRUE if nothing regressed; always logs the comparison */
int  res_check(const ResSnapshot *base, const char *label);

/* GDI object helpers with defensive checks */
HBITMAP     make_dib(int w, int h, int bpp, void **bitsOut);
HBITMAP     clone_dib_from(HDC src, int w, int h);
HBITMAP     clone_dib_object(HBITMAP src);
void        free_dib(HBITMAP bmp, void *bits);

/* ------------------------------------------------------------------ */
/* app lifecycle helpers                                               */
/* ------------------------------------------------------------------ */
int  app_set_dpi_aware(void);        /* per-monitor v2, falls back gracefully */
int  app_get_dpi(void);              /* dpi of primary / current monitor     */
double app_scale(void);              /* dpi / 96.0                           */
int  app_scale_px(int logical_px);   /* logical -> physical                  */
/* Re-read the scale from a specific window, for per-monitor DPI. Call once
 * right after the window is created; see the note in winutil.c. */
void app_set_dpi_from_window(HWND hwnd);

void app_enable_drag(HWND hwnd);     /* drag by clicking title/caption area  */
void app_middle_click(HWND hwnd, BOOL down);

int   app_is_elevated(void);
int   app_relaunch_elevated(const wchar_t *exePath);

/* draw a window's caption in the system menu colour, for frameless windows */
void  app_make_translucent(HWND hwnd, int alpha);
void  app_shadowless(HWND hwnd);

/* clipboard helpers */
int   clip_put_bitmap(HBITMAP bmp);              /* CF_DIB  */
HBITMAP clip_get_bitmap(void);                    /* CF_DIB, caller frees */
int   clip_put_text(const wchar_t *text);
/* Build a CF_DIB HGLOBAL from a 32bpp DIB. The caller must either hand it to
 * SetClipboardData (inside an already-open clipboard) or GlobalFree it. */
HGLOBAL dib_to_global(HBITMAP bmp);

/* file dialogs */
int   app_save_png_dialog(HWND owner, wchar_t *outPath, size_t outCch);

/* Picks a window position and size for a window that wants `wantW` x `wantH`
 * client pixels: the request is clamped to the monitor work area (leaving a
 * margin) and centred on it. Feeding the result to CreateWindowEx keeps a
 * window from opening larger than the screen, which is what happens if the
 * DPI scale is applied on top of a fixed size. */
void  app_fit_window(int wantW, int wantH, int *outX, int *outY,
                     int *outW, int *outH);

/* Modal single-window text prompt. `initial` is pre-filled (may be NULL).
 * `cap` is the total character capacity of initial, and also the max length
 * accepted. Returns 1 on OK with a freshly allocated string in *out. */
int   app_multiline_input(HWND owner, const wchar_t *title, const wchar_t *hint,
                          const wchar_t *initial, int cap, wchar_t **out);

/* The About box: this program's own icon, `name`, the usage `notes` the caller
 * passes, and the shared credit line - author wujianqi and a clickable
 * github.com/wujianqi/ link that opens in the default browser. Modal, and
 * centred on `owner`; pass NULL from a command-line switch, which has no window
 * to centre on. Lives in src\common\about.c. */
void  app_show_about(HWND owner, const wchar_t *name, const wchar_t *notes);

/* console / diagnostics */
/* Re-attach stdout/stderr to the parent's console when this process was
 * started from one and has no standard handles of its own. A no-op when
 * launched from Explorer, which is what keeps the console window away. */
void  app_attach_parent_console(void);
void  app_log(const char *fmt, ...);
/* Same, but takes a wide format and wide varargs (MinGW's printf does not
 * implement %ls, so this converts the result instead of relying on it). */
void  app_logw(const wchar_t *fmt, ...);
void  app_set_verbose(int on);
int   app_verbose(void);

/* Installs a vectored exception handler that appends a symbolized backtrace of
 * any unhandled crash to %LOCALAPPDATA%\MuskTools\crash.log. Call once, early,
 * before any window is created. `tag` identifies the app in the log. */
void  app_install_crash_log(const wchar_t *tag);

/* wide string helpers */
wchar_t *wcs_replace(const wchar_t *src, const wchar_t *find, const wchar_t *repl);
int      wcs_ieq(const wchar_t *a, const wchar_t *b);
int      wcs_icontains(const wchar_t *hay, const wchar_t *needle);
void     wcs_trim(wchar_t *s);
void     wcs_rtrim(wchar_t *s);
wchar_t *wcs_format(const wchar_t *fmt, ...);

/* growable wide string builder */
typedef struct {
    wchar_t *buf;
    size_t   len;
    size_t   cap;
} WBuf;

void wbuf_init(WBuf *b);
void wbuf_free(WBuf *b);
void wbuf_reset(WBuf *b);
void wbuf_addc(WBuf *b, wchar_t c);
void wbuf_addn(WBuf *b, const wchar_t *s, size_t n);
void wbuf_add(WBuf *b, const wchar_t *s);
void wbuf_addf(WBuf *b, const wchar_t *fmt, ...);

/* growable pointer list (owning) */
typedef struct {
    void **items;
    int    count;
    int    cap;
} PtrList;

void  ptrlist_init(PtrList *l);
void  ptrlist_free(PtrList *l, void (*freefn)(void *));
void  ptrlist_push(PtrList *l, void *p);
void *ptrlist_get(const PtrList *l, int i);
void  ptrlist_remove_at(PtrList *l, int i);
void  ptrlist_set(PtrList *l, int i, void *p);

/* ------------------------------------------------------------------ */
/* GDI+                                                                */
/* ------------------------------------------------------------------ */
int  gdiplus_start(void);
void gdiplus_stop(void);
int  gdiplus_ok(void);

#define GP_OK 0

/* Save a 32bpp DIB-section bitmap as PNG. Returns 1 on success. */
int gdiplus_save_png(HBITMAP dib, const wchar_t *path);
/* Same, but scales nothing - straight 32bpp copy. */
int gdiplus_save_png_from_bits(const void *bits, int w, int h, int stride,
                               const wchar_t *path);
/* Save a top-down 32bpp BGRA buffer as whatever `mime` names - L"image/png" or
 * L"image/jpeg" are the two every Windows install ships an encoder for.
 * `quality` is 1-100 and only read for JPEG; PNG ignores it.
 *
 * `bits` is plain memory owned by the caller rather than a DIB section's, so
 * there is no HBITMAP to hand over - the point of this one is that a caller
 * holding a raw buffer (a cropped region, a decoded image) can write it out
 * without first wrapping it in one just to reuse gdiplus_save_png.
 *
 * JPEG has no alpha channel: a buffer that carries transparency encodes to
 * black there. Callers that want a background behind those pixels must
 * flatten before calling. Returns 1 on success. */
int gdiplus_save_buffer(const void *bits, int w, int h, int stride,
                        const wchar_t *path, const wchar_t *mime, int quality);
/* Encode a 32bpp DIB into a freshly xmalloc'd PNG buffer. Caller xfree()s. */
int gdiplus_encode_png(const void *bits, int w, int h, int stride,
                       unsigned char **outBuf, size_t *outLen);

/* 32bpp BGRA helpers (our internal DIB pixel format) */
typedef struct { BYTE b, g, r, a; } BGRAPX;
#define PX_AT(BITS, STRIDE, X, Y) \
    (((BGRAPX *)((BYTE *)(BITS) + (size_t)(STRIDE) * (size_t)(Y)))[X])

#ifdef __cplusplus
}
#endif
#endif /* WINUTIL_H */
