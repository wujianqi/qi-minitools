/* winutil.c - shared infrastructure implementation */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "winutil.h"
#include <gdiplus.h>
#include <commctrl.h>
#include <commdlg.h>
#include <psapi.h>
#include <shlobj.h>

/* ================================================================== */
/* allocation tracker                                                   */
/* ================================================================== */

/* Each block is prefixed with a 16-byte header that keeps 16-byte
 * alignment (malloc guarantees 16 on 64-bit Windows) and stores the size. */
#define HDRSZ 16

typedef struct {
    LONG  live;
    LONG  total;
} AllocStats;

static AllocStats g_alloc = { 0, 0 };

static void alloc_delta(LONG d)
{
    InterlockedAdd(&g_alloc.live, d);
    if (d > 0) InterlockedAdd(&g_alloc.total, d);
}

void *xmalloc(size_t n)
{
    unsigned char *base;
    if (n == 0) n = 1;
    if (n > (size_t)-1 - HDRSZ) return NULL;
    base = (unsigned char *)malloc(n + HDRSZ);
    if (!base) return NULL;
    memcpy(base, &n, sizeof(size_t));
    alloc_delta(1);
    return base + HDRSZ;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p;
    size_t total;
    if (n && sz > (size_t)-1 / n) return NULL;
    total = n * sz;
    p = xmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    unsigned char *base;
    size_t old;
    if (!p) return xmalloc(n);
    if (n == 0) n = 1;
    base = (unsigned char *)p - HDRSZ;
    memcpy(&old, base, sizeof(size_t));
    if (n > (size_t)-1 - HDRSZ) return NULL;
    base = (unsigned char *)realloc(base, n + HDRSZ);
    if (!base) return NULL;   /* original block still valid */
    memcpy(base, &n, sizeof(size_t));
    return base + HDRSZ;
}

void xfree(void *p)
{
    unsigned char *base;
    if (!p) return;
    base = (unsigned char *)p - HDRSZ;
    alloc_delta(-1);
    free(base);
}

char *xstrdup(const char *s)
{
    size_t n;
    char *r;
    if (!s) return NULL;
    n = strlen(s) + 1;
    r = (char *)xmalloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

wchar_t *xwcsdup(const wchar_t *s)
{
    size_t n;
    wchar_t *r;
    if (!s) return NULL;
    n = wcslen(s) + 1;
    r = (wchar_t *)xmalloc(n * sizeof(wchar_t));
    if (r) memcpy(r, s, n * sizeof(wchar_t));
    return r;
}

long xalloc_live(void)  { return (long)g_alloc.live; }
long xalloc_total(void) { return (long)g_alloc.total; }
void xalloc_reset_totals(void) { InterlockedExchange(&g_alloc.total, 0); }

/* ================================================================== */
/* resource accounting                                                  */
/* ================================================================== */

void res_snapshot(ResSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->gdiObjects  = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    out->userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    out->handles     = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &out->handles))
        out->handles = 0;
    out->allocs = xalloc_live();
}

int res_check(const ResSnapshot *base, const char *label)
{
    ResSnapshot now;
    int ok = 1;
    res_snapshot(&now);
    if (now.gdiObjects  != base->gdiObjects)  ok = 0;
    if (now.userObjects != base->userObjects) ok = 0;
    if (now.handles     != base->handles)     ok = 0;
    if (now.allocs     != base->allocs)      ok = 0;
    app_log("[%s] %s  gdi %lu->%lu  user %lu->%lu  handle %lu->%lu  heap %ld->%ld",
            ok ? " OK " : "LEAK", label,
            (unsigned long)base->gdiObjects, (unsigned long)now.gdiObjects,
            (unsigned long)base->userObjects,(unsigned long)now.userObjects,
            (unsigned long)base->handles,    (unsigned long)now.handles,
            base->allocs, now.allocs);
    return ok;
}

/* ================================================================== */
/* DIB helpers                                                          */
/* ================================================================== */

HBITMAP make_dib(int w, int h, int bpp, void **bitsOut)
{
    BITMAPINFO bi;
    void *bits = NULL;
    HDC dc;
    HBITMAP bmp;

    if (w <= 0 || h <= 0) return NULL;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount     = (WORD)bpp;
    bi.bmiHeader.biCompression = BI_RGB;

    dc = GetDC(NULL);
    if (!dc) return NULL;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (bmp && bitsOut) *bitsOut = bits;
    return bmp;
}

void free_dib(HBITMAP bmp, void *bits)
{
    (void)bits;
    if (bmp) DeleteObject(bmp);
}

HBITMAP clone_dib_from(HDC src, int w, int h)
{
    /* Copies a w*h region from src (top-left origin) into a new top-down
     * 32bpp DIB section. Caller owns the result. */
    HBITMAP out;
    void *bits = NULL;
    HDC dc, mem;

    out = make_dib(w, h, 32, &bits);
    if (!out) return NULL;

    dc = GetDC(NULL);
    if (!dc) { DeleteObject(out); return NULL; }
    mem = CreateCompatibleDC(dc);
    ReleaseDC(NULL, dc);
    if (!mem) { DeleteObject(out); return NULL; }

    SelectObject(mem, out);
    BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY);
    DeleteDC(mem);
    return out;
}

/* ================================================================== */
/* console                                                              */
/* ================================================================== */

/* All three exes are GUI-subsystem binaries, so opening one from Explorer
 * brings up no console window at all. That is the point of it, but it also
 * means a run started from a terminal can have no standard handles, and a
 * bare `shot.exe --selftest` would look like it printed nothing. Re-attach to
 * the parent's console in exactly that case, and nowhere else.
 *
 * Two things are deliberately left alone:
 *
 *   - If the parent already handed over usable standard handles - a pipe
 *     from `2>&1`, a `> out.txt` redirect - those are better than CONOUT$
 *     and are kept. Rewiring them would send the test reports to the
 *     console screen and leave the capture pipe empty, which is the one
 *     way this could make build.ps1 -Test pass without testing anything.
 *   - If there is no parent console - Explorer, Task Scheduler, a service -
 *     AttachConsole fails and nothing is allocated. No window is created,
 *     which is the whole behaviour being asked for.
 */

static int std_handle_is_usable(DWORD which)
{
    HANDLE h = GetStdHandle(which);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

void app_attach_parent_console(void)
{
    if (std_handle_is_usable(STD_INPUT_HANDLE)  ||
        std_handle_is_usable(STD_OUTPUT_HANDLE) ||
        std_handle_is_usable(STD_ERROR_HANDLE))
        return;

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    /* Only reached from a terminal, so the console devices exist. */
    freopen("CONIN$",  "r", stdin);
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    /* app_log flushes per line, but the self-tests are read by a parent that
     * can vanish the moment the process exits; unbuffered stderr makes
     * "written" and "visible" the same moment. */
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* ================================================================== */
/* diagnostics                                                          */
/* ================================================================== */

static int g_verbose = -1;

void app_set_verbose(int on) { g_verbose = on; }
int  app_verbose(void) { return g_verbose < 0 ? 0 : g_verbose; }

void app_log(const char *fmt, ...)
{
    va_list ap;
    char line[1024];
    size_t n;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    n = strlen(line);
    if (n + 2 < sizeof(line)) { line[n] = '\r'; line[n + 1] = '\n'; line[n + 2] = 0; }
    else { line[sizeof(line) - 3] = 0; }
    OutputDebugStringA(line);
    if (app_verbose()) { fputs(line, stderr); fflush(stderr); }
}

void app_logw(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    char *u8 = NULL;
    int bytes;

    /* Format in a single pass into a stack buffer. _vscwprintf is not usable
     * as a measuring call here: besides returning the length it also writes
     * the result to stdout, so every wide log line was duplicated into the
     * program's standard output and scrambled the test reports. */
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (buf[0] == 0) return;

    /* Use the ANSI code page: the log may carry Chinese text, and both
     * OutputDebugStringA and the console expect the system ACP. */
    bytes = WideCharToMultiByte(CP_ACP, 0, buf, -1, NULL, 0, NULL, NULL);
    if (bytes > 0) {
        u8 = (char *)xmalloc((size_t)bytes);
        if (u8) {
            WideCharToMultiByte(CP_ACP, 0, buf, -1, u8, bytes, NULL, NULL);
            app_log("%s", u8);
            xfree(u8);
        }
    }
}

/* ================================================================== */
/* crash log                                                           */
/* ================================================================== */

/* A vectored handler is the only thing that still runs when the guard page
 * trips, so it is used instead of a plain SetUnhandledExceptionFilter. Symbol
 * resolution is warmed up at install time because doing it from inside the
 * handler is both slow and liable to fault again. */

#include <dbghelp.h>

static PVOID            g_crashVeh = NULL;
static PVOID            g_crashBase = NULL;
static HANDLE           g_crashFile = INVALID_HANDLE_VALUE;
static const wchar_t   *g_crashTagW = L"app";

/* MinGW's dbghelp.h declares SYMBOL_INFOW as just { WCHAR Name[1]; }, but the
 * API writes up to MaxNameLen wide chars past the header, so the real layout
 * is the two ULONGs followed by the name. Declare it here and size the buffer
 * to match. */
typedef struct {
    ULONG SizeOf;
    ULONG MaxNameLen;
    WCHAR Name[1];
} CrashSymW;

#define CRASH_SYM_MAX 512
static BYTE g_crashSym[sizeof(CrashSymW) + CRASH_SYM_MAX * sizeof(WCHAR)];

static void crash_emit(const char *s)
{
    DWORD n, written = 0;
    /* Deliberately no OutputDebugStringA here: it raises DBG_PRINTEXCEPTION_C
     * internally, which comes straight back through our own VEH and would
     * have the crash logger report itself. */
    if (g_crashFile == INVALID_HANDLE_VALUE) return;
    n = (DWORD)strlen(s);
    if (!n) return;
    WriteFile(g_crashFile, s, n, &written, NULL);
}

static void crash_emitf(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof(b) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    /* Terminate right after the text, not at a fixed offset near the end of
     * the buffer: the string's own NUL would otherwise cut the CRLF off and
     * every line would run together. */
    if (n > (int)sizeof(b) - 3) n = (int)sizeof(b) - 3;
    b[n]     = '\r';
    b[n + 1] = '\n';
    b[n + 2] = 0;
    crash_emit(b);
}

/* Resolves into the static buffer; never writes past it. */
static int crash_symbol(DWORD64 addr, DWORD64 *disp)
{
    CrashSymW *si = (CrashSymW *)g_crashSym;
    typedef BOOL (WINAPI *fnSA)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
    static fnSA sa = NULL;
    HMODULE dbg;

    if (!sa) {
        dbg = GetModuleHandleW(L"dbghelp.dll");
        if (!dbg) return 0;
        sa = (fnSA)(void *)GetProcAddress(dbg, "SymFromAddrW");
        if (!sa) return 0;
    }
    si->SizeOf = sizeof(CrashSymW);
    si->MaxNameLen = CRASH_SYM_MAX - 1;
    si->Name[0] = 0;
    if (!sa(GetCurrentProcess(), addr, disp, (PSYMBOL_INFOW)si)) return 0;
    si->Name[CRASH_SYM_MAX - 1] = 0;
    return 1;
}

static void crash_frame(const char *label, DWORD64 addr)
{
    DWORD64 disp = 0;
    /* Emit the raw address first: if symbolisation faults we still have it. */
    crash_emitf("     %s 0x%012llX", label, (unsigned long long)addr);
    if (crash_symbol(addr, &disp))
        crash_emitf("     %s ^ %ls +0x%llX", label, ((CrashSymW *)g_crashSym)->Name,
                    (unsigned long long)(addr - disp));
}

static LONG CALLBACK crash_veh_proc(EXCEPTION_POINTERS *ep)
{
    /* Only report the first fault in a chain; the rest are fallout. */
    static volatile LONG seen;
    static void *frames[64];
    ULONG code;
    USHORT n;
    WORD i;
    char lbl[8];

    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    code = ep->ExceptionRecord->ExceptionCode;

    /* Notifications, not faults. A VEH sees these from perfectly healthy
     * processes all the time (any component calling OutputDebugString raises
     * DBG_PRINTEXCEPTION_C, and JIT debuggers use the breakpoint codes), so
     * logging them would bury a real crash under noise. */
    if (code == 0x40010006u ||            /* DBG_PRINTEXCEPTION_C     */
        code == 0x40010007u ||            /* DBG_PRINTEXCEPTION_C_VC  */
        code == 0x406D1388u ||            /* thread naming            */
        code == 0x00000002u)              /* INT_DEBUG                */
        return EXCEPTION_CONTINUE_SEARCH;

    /* An attached debugger owns breakpoint reporting. */
    if (code == 0x80000003u && IsDebuggerPresent())
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedCompareExchange(&seen, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    crash_emitf("---- %ls crash: pid=%lu tid=%lu code=0x%08lX",
                g_crashTagW ? g_crashTagW : L"?",
                (unsigned long)GetCurrentProcessId(),
                (unsigned long)GetCurrentThreadId(),
                (unsigned long)code);
    crash_frame("at   ", (DWORD64)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        crash_emitf("     param0=0x%llX param1=0x%llX",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);

    n = CaptureStackBackTrace(0, (DWORD)(sizeof(frames) / sizeof(frames[0])), frames, NULL);
    for (i = 0; i < n && i < (WORD)(sizeof(frames) / sizeof(frames[0])); i++) {
        lbl[0] = '#';
        lbl[1] = (char)('0' + (i / 10) % 10);
        lbl[2] = (char)('0' + i % 10);
        lbl[3] = ' ';
        lbl[4] = 0;
        crash_frame(lbl, (DWORD64)(ULONG_PTR)frames[i]);
    }
    crash_emit("---- end crash record\r\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_open_file(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir)))
        return;
    {
        size_t n = wcslen(dir);
        if (n + 16 >= MAX_PATH) return;
        wcscpy(dir + n, L"\\MuskTools");
    }
    CreateDirectoryW(dir, NULL);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\crash.log", dir);
    g_crashFile = CreateFileW(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

void app_install_crash_log(const wchar_t *tag)
{
    HMODULE dbg;
    if (tag) g_crashTagW = tag;
    g_crashBase = (PVOID)GetModuleHandleW(NULL);

    dbg = LoadLibraryW(L"dbghelp.dll");
    if (dbg) {
        typedef BOOL (WINAPI *fnSI)(HANDLE, PCWSTR, DWORD);
        typedef BOOL (WINAPI *fnSA)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
        typedef DWORD (WINAPI *fnSO)(DWORD);
        fnSI si = (fnSI)(void *)GetProcAddress(dbg, "SymInitializeW");
        fnSA sa = (fnSA)(void *)GetProcAddress(dbg, "SymFromAddrW");
        fnSO so = (fnSO)(void *)GetProcAddress(dbg, "SymSetOptions");
        if (so) so(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (si) si(GetCurrentProcess(), NULL, 1);
        if (sa) {
            /* force the module table in now so the handler stays cheap */
            DWORD64 disp = 0;
            crash_symbol((DWORD64)(ULONG_PTR)g_crashBase, &disp);
        }
    }

    crash_open_file();
    if (g_crashFile == INVALID_HANDLE_VALUE) return;
    g_crashVeh = AddVectoredExceptionHandler(1, crash_veh_proc);
}

/* ================================================================== */
/* window placement                                                    */
/* ================================================================== */

void app_fit_window(int wantW, int wantH, int *outX, int *outY,
                    int *outW, int *outH)
{
    RECT wa;
    HMONITOR mon;
    MONITORINFO mi;
    POINT origin;
    int availW, availH, w, h, x, y, margin;

    origin.x = 0; origin.y = 0;
    mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        wa = mi.rcWork;
    } else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    else {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    /* Room is limited to 92% of the work area so the window never touches the
     * taskbar or the screen edge, and a sane minimum is kept for tiny screens. */
    availW = (int)((wa.right - wa.left) * 92 / 100);
    availH = (int)((wa.bottom - wa.top) * 92 / 100);
    margin = app_scale_px(8);
    if (availW < 360) availW = 360;
    if (availH < 300) availH = 300;

    w = wantW > 0 ? wantW : availW;
    h = wantH > 0 ? wantH : availH;
    if (w > availW) w = availW;
    if (h > availH) h = availH;

    x = wa.left + (wa.right - wa.left - w) / 2;
    y = wa.top  + (wa.bottom - wa.top - h) / 2;
    if (y < wa.top + margin) y = wa.top + margin;

    if (outX) *outX = x;
    if (outY) *outY = y;
    if (outW) *outW = w;
    if (outH) *outH = h;
}

/* ================================================================== */
/* DPI                                                                  */
/* ================================================================== */

typedef BOOL (WINAPI *fnSetCtx)(HANDLE);
typedef HANDLE (WINAPI *fnGetCtx)(void);
typedef int   (WINAPI *fnGetAware)(HANDLE);

/* DPI_AWARENESS / DPI_AWARENESS_CONTEXT values */
#define AWARE_UNAWARE  0
#define AWARE_SYSTEM   1
#define AWARE_PERMON   2

static int current_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        fnGetCtx getCtx = (fnGetCtx)(void *)GetProcAddress(user32,
                                  "GetThreadDpiAwarenessContext");
        fnGetAware getAware = (fnGetAware)(void *)GetProcAddress(user32,
                                     "GetAwarenessFromDpiAwarenessContext");
        if (getCtx && getAware) return getAware(getCtx());
    }
    {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            typedef HRESULT (WINAPI *fnPA)(HANDLE, int *);
            fnPA pa = (fnPA)(void *)GetProcAddress(shcore, "GetProcessDpiAwareness");
            int a = AWARE_UNAWARE;
            if (pa && SUCCEEDED(pa(GetCurrentProcess(), &a))) {
                FreeLibrary(shcore);
                return a;
            }
            FreeLibrary(shcore);
        }
    }
    return -1;      /* unknown */
}

int app_set_dpi_aware(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    int have = current_dpi_awareness();

    /* A process inherits its DPI awareness from whoever launched it. If the
     * launcher was already per-monitor aware we must not touch it: falling
     * through to SetProcessDPIAware() here would DOWNGRADE the process, and a
     * window created under a downgraded context is later re-managed by the
     * shell at a different scale. Escalate only, never downgrade. */
    if (have == AWARE_PERMON) return 2;

    if (user32) {
        fnSetCtx f = (fnSetCtx)(void *)GetProcAddress(user32,
                                    "SetProcessDpiAwarenessContext");
        if (f) {
            /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
            if (f((HANDLE)-4)) return 2;
            if (f((HANDLE)-3)) return 2;   /* PER_MONITOR_AWARE */
        }
    }
    {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            typedef HRESULT (WINAPI *fnSA)(int);
            fnSA sa = (fnSA)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
            if (sa && SUCCEEDED(sa(AWARE_PERMON))) { FreeLibrary(shcore); return 2; }
        }
        if (shcore) FreeLibrary(shcore);
    }
    if (have == AWARE_SYSTEM) return 1;
    return SetProcessDPIAware() ? 1 : 0;
}

static int g_dpi = 96;

static void refresh_dpi(void)
{
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(NULL, dc);
    if (d <= 0) d = 96;
    g_dpi = d;
}

int app_get_dpi(void)
{
    if (g_dpi == 96) refresh_dpi();
    return g_dpi;
}

/* GetDC(NULL) and GetDpiForSystem() both report the *system* DPI, which on a
 * mixed-DPI desktop is not the DPI of the monitor a window actually sits on -
 * a per-monitor-v2 process on a 125% monitor still sees 96 from both. The
 * window itself is the only authoritative source there, so re-read the scale
 * from it as soon as one exists. Call this after CreateWindowEx; without it
 * every layout in these tools is built for 96 DPI and comes out 20% small.
 *
 * Older Windows has no GetDpiForWindow, and neither has a way to ask per
 * monitor from here, so those builds keep the desktop-DC value. */
void app_set_dpi_from_window(HWND hwnd)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *fnGDFW)(HWND);
    fnGDFW f;
    UINT d;

    if (!hwnd || !user32) return;
    f = (fnGDFW)(void *)GetProcAddress(user32, "GetDpiForWindow");
    if (!f) return;
    d = f(hwnd);
    if (d >= 72 && d <= 576) g_dpi = (int)d;
}

double app_scale(void) { return (double)app_get_dpi() / 96.0; }

int app_scale_px(int logical_px)
{
    return (int)lround(logical_px * app_scale());
}

void app_enable_drag(HWND hwnd)
{
    if (hwnd) SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void app_middle_click(HWND hwnd, BOOL down)
{
    if (!hwnd) return;
    SendMessageW(hwnd,
        down ? WM_MBUTTONDOWN : WM_MBUTTONUP,
        down ? MK_MBUTTON : 0, 0);
}

/* ================================================================== */
/* elevation                                                            */
/* ================================================================== */

int app_is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    int r = 0;
    memset(&el, 0, sizeof(el));
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), NULL)) r = el.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

int app_relaunch_elevated(const wchar_t *exePath)
{
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = L"--elevated";
    sei.lpDirectory = NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 0;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return 1;
}

/* ================================================================== */
/* clipboard                                                            */
/* ================================================================== */

HGLOBAL dib_to_global(HBITMAP bmp)
{
    BITMAP bm;
    HDC dc;
    HGLOBAL gh = NULL;
    BITMAPINFO bi;
    size_t stride, size;
    BYTE *src, *dst;

    if (!bmp) return NULL;
    dc = GetDC(NULL);
    if (!dc) return NULL;
    if (GetObjectW(bmp, sizeof(bm), &bm) != sizeof(bm)) { ReleaseDC(NULL, dc); return NULL; }
    if (bm.bmBitsPixel != 32 || !bm.bmBits) { ReleaseDC(NULL, dc); return NULL; }

    stride = (size_t)bm.bmWidthBytes;
    size   = stride * (size_t)bm.bmHeight;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize       = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth      = bm.bmWidth;
    bi.bmiHeader.biHeight     = -bm.bmHeight;  /* top-down, matches our DIBs */
    bi.bmiHeader.biPlanes     = 1;
    bi.bmiHeader.biBitCount   = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    gh = GlobalAlloc(GMEM_MOVEABLE, sizeof(bi) + size);
    if (!gh) { ReleaseDC(NULL, dc); return NULL; }
    dst = (BYTE *)GlobalLock(gh);
    if (!dst) { GlobalFree(gh); ReleaseDC(NULL, dc); return NULL; }
    memcpy(dst, &bi, sizeof(bi));
    src = (BYTE *)bm.bmBits;
    memcpy(dst + sizeof(bi), src, size);
    GlobalUnlock(gh);
    ReleaseDC(NULL, dc);
    return gh;
}

HBITMAP clone_dib_object(HBITMAP src)
{
    /* Copies any 32bpp DIB-section bitmap into a fresh top-down 32bpp DIB. */
    HBITMAP out;
    void *bits = NULL;
    HDC dc, mem;
    HBITMAP old;
    BITMAP bm;

    if (!src) return NULL;
    if (GetObjectW(src, sizeof(bm), &bm) != sizeof(bm)) return NULL;
    if (bm.bmBitsPixel != 32 || !bm.bmBits) return NULL;

    out = make_dib(bm.bmWidth, bm.bmHeight, 32, &bits);
    if (!out) return NULL;
    dc = GetDC(NULL);
    if (!dc) { DeleteObject(out); return NULL; }
    mem = CreateCompatibleDC(dc);
    if (!mem) { ReleaseDC(NULL, dc); DeleteObject(out); return NULL; }
    {
        HBITMAP saved;
        old = SelectObject(mem, out);
        saved = SelectObject(mem, src);   /* src must be selected to blit from */
        BitBlt(mem, 0, 0, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
        SelectObject(mem, saved);
        SelectObject(mem, old);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, dc);
    return out;
}

int clip_put_bitmap(HBITMAP bmp)
{
    HGLOBAL gh = dib_to_global(bmp);
    if (!gh) return 0;
    if (!OpenClipboard(NULL)) { GlobalFree(gh); return 0; }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, gh)) { CloseClipboard(); GlobalFree(gh); return 0; }
    CloseClipboard();
    return 1;   /* ownership transferred to the clipboard */
}

HBITMAP clip_get_bitmap(void)
{
    HANDLE h;
    BITMAPINFO *bi;
    size_t off, size, stride;
    HDC dc, mem;
    HBITMAP out = NULL;
    void *bits = NULL;
    const BYTE *src;

    if (!OpenClipboard(NULL)) return NULL;
    h = GetClipboardData(CF_DIB);
    if (!h) { CloseClipboard(); return NULL; }
    bi = (BITMAPINFO *)GlobalLock(h);
    if (!bi) { CloseClipboard(); return NULL; }

    off  = bi->bmiHeader.biSize;
    if (bi->bmiHeader.biSize == sizeof(BITMAPINFOHEADER) &&
        bi->bmiHeader.biClrUsed)
        off += (size_t)bi->bmiHeader.biClrUsed * 4;

    stride = (((size_t)abs(bi->bmiHeader.biWidth) * bi->bmiHeader.biBitCount + 31) / 32) * 4;
    size   = stride * (size_t)abs(bi->bmiHeader.biHeight);
    src    = (const BYTE *)bi + off;

    out = make_dib(abs(bi->bmiHeader.biWidth), abs(bi->bmiHeader.biHeight), 32, &bits);
    if (out && bits) {
        int y, n = abs(bi->bmiHeader.biHeight);
        int flip = bi->bmiHeader.biHeight < 0;
        for (y = 0; y < n; y++) {
            const BYTE *s = src + stride * (size_t)(flip ? y : (n - 1 - y));
            memcpy((BYTE *)bits + stride * (size_t)y, s, stride);
        }
    }
    GlobalUnlock(h);
    CloseClipboard();
    if (out && !bits) { DeleteObject(out); out = NULL; }
    return out;
}

int clip_put_text(const wchar_t *text)
{
    size_t bytes;
    HGLOBAL gh;
    wchar_t *p;
    if (!text) return 0;
    bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    gh = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!gh) return 0;
    p = (wchar_t *)GlobalLock(gh);
    if (!p) { GlobalFree(gh); return 0; }
    memcpy(p, text, bytes);
    GlobalUnlock(gh);
    if (!OpenClipboard(NULL)) { GlobalFree(gh); return 0; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, gh)) { CloseClipboard(); GlobalFree(gh); return 0; }
    CloseClipboard();
    return 1;
}

/* ================================================================== */
/* modal text prompt                                                   */
/* ================================================================== */

typedef struct {
    HWND     hwnd, edit;
    wchar_t *out;
    int      cap;
} PromptDlg;

static const wchar_t *PROMPT_CLASS = L"MuskPromptDlgCls";

static PromptDlg g_pd;      /* modal: one instance at a time */

static LRESULT CALLBACK prompt_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    PromptDlg *pd = &g_pd;
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == 1 || LOWORD(w) == 2) {
            if (pd && LOWORD(w) == 1) {
                int n = GetWindowTextLengthW(pd->edit) + 1;
                if (n > 1) {
                    pd->out = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
                    if (pd->out) GetWindowTextW(pd->edit, pd->out, n);
                }
            }
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int app_multiline_input(HWND owner, const wchar_t *title, const wchar_t *hint,
                        const wchar_t *initial, int cap, wchar_t **out)
{
    static const wchar_t *cls = NULL;
    WNDCLASSEXW wc;
    PromptDlg *pd = &g_pd;
    MSG msg;
    int S = app_scale_px(1);
    int W = app_scale_px(460), H = app_scale_px(300);
    RECT r;

    if (out) *out = NULL;
    cls = PROMPT_CLASS;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = prompt_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = cls;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;

    memset(pd, 0, sizeof(*pd));
    pd->cap = cap;
    GetWindowRect(owner, &r);
    pd->hwnd = CreateWindowExW(WS_EX_TOPMOST, cls, title ? title : L"输入",
                              WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
                              r.left + 60, r.top + 60, W, H,
                              owner, NULL, wc.hInstance, NULL);
    if (!pd->hwnd) return 0;

    if (hint)
        CreateWindowExW(0, L"STATIC", hint, WS_CHILD | WS_VISIBLE,
                        S * 12, S * 10, W - S * 24, S * 18,
                        pd->hwnd, (HMENU)(INT_PTR)102, wc.hInstance, NULL);
    pd->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial ? initial : L"",
                              WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                              ES_WANTRETURN | ES_AUTOHSCROLL,
                              S * 12, S * 32, W - S * 24, H - S * 78,
                              pd->hwnd, (HMENU)(INT_PTR)101, wc.hInstance, NULL);
    {
        HFONT f = CreateFontW(-S * 13, 0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
        if (f) SendMessageW(pd->edit, WM_SETFONT, (WPARAM)f, TRUE);
    }
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    W - S * 110, H - S * 40, S * 46, S * 26, pd->hwnd,
                    (HMENU)(INT_PTR)2, wc.hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    W - S * 58, H - S * 40, S * 46, S * 26, pd->hwnd,
                    (HMENU)(INT_PTR)1, wc.hInstance, NULL);

    SetFocus(pd->edit);
    SendMessageW(pd->edit, EM_SETSEL, 0, -1);
    SetForegroundWindow(pd->hwnd);

    while (IsWindow(pd->hwnd)) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) break;
        if (msg.message == WM_QUIT) break;
        if (!IsDialogMessageW(pd->hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(pd->hwnd)) DestroyWindow(pd->hwnd);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(cls, wc.hInstance);
    if (out) *out = pd->out;
    return pd->out != NULL;
}

/* ================================================================== */
/* PNG save dialog                                                      */
/* ================================================================== */

/* Minimal modern-style save dialog is overkill; we build a classic
 * GetSaveFileNameW dialog with a PNG filter. */
int app_save_png_dialog(HWND owner, wchar_t *outPath, size_t outCch)
{
    static const wchar_t *filter =
        L"PNG 图片 (*.png)\0*.png\0\0";
    wchar_t file[512];
    OPENFILENAMEW ofn;
    int r;

    if (!outPath || outCch == 0) return 0;
    file[0] = 0;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = (DWORD)(sizeof(file) / sizeof(wchar_t));
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle  = L"保存截图";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    r = GetSaveFileNameW(&ofn);
    if (!r) return 0;
    wcsncpy(outPath, file, outCch - 1);
    outPath[outCch - 1] = 0;
    return 1;
}

/* ================================================================== */
/* wide string helpers                                                  */
/* ================================================================== */

int wcs_ieq(const wchar_t *a, const wchar_t *b)
{
    if (!a || !b) return a == b;
    return _wcsicmp(a, b) == 0;
}

int wcs_icontains(const wchar_t *hay, const wchar_t *needle)
{
    size_t nl;
    if (!hay || !needle) return 0;
    nl = wcslen(needle);
    if (!nl) return 1;
    if (wcslen(hay) < nl) return 0;
    for (; *hay; hay++) {
        if (_wcsnicmp(hay, needle, nl) == 0) return 1;
    }
    return 0;
}

void wcs_trim(wchar_t *s)
{
    wchar_t *p = s;
    size_t n;
    if (!s) return;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n'))
        s[--n] = 0;
}

void wcs_rtrim(wchar_t *s)
{
    size_t n;
    if (!s) return;
    n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L';')) s[--n] = 0;
}

wchar_t *wcs_replace(const wchar_t *src, const wchar_t *find, const wchar_t *repl)
{
    WBuf b;
    const wchar_t *p = src;
    size_t fl;
    if (!src || !find || !repl) return xwcsdup(src);
    fl = wcslen(find);
    wbuf_init(&b);
    if (!fl) { wbuf_add(&b, src); return b.buf; }
    while (*p) {
        if (wcs_ieq(p, find) || wcsncmp(p, find, fl) == 0) {
            wbuf_add(&b, repl);
            p += fl;
        } else {
            wbuf_addc(&b, *p++);
        }
    }
    return b.buf;
}

wchar_t *wcs_format(const wchar_t *fmt, ...)
{
    va_list ap;
    int n;
    wchar_t *buf;
    va_start(ap, fmt);
    n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n < 0) return xwcsdup(L"");
    buf = (wchar_t *)xmalloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!buf) return NULL;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, (size_t)n + 1, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

/* ================================================================== */
/* WBuf                                                                 */
/* ================================================================== */

void wbuf_init(WBuf *b) { b->buf = NULL; b->len = 0; b->cap = 0; }

void wbuf_free(WBuf *b)
{
    if (!b) return;
    xfree(b->buf);
    b->buf = NULL; b->len = 0; b->cap = 0;
}

void wbuf_reset(WBuf *b) { b->len = 0; if (b->buf) b->buf[0] = 0; }

static int wbuf_reserve(WBuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;
    wchar_t *nb;
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    nb = (wchar_t *)xrealloc(b->buf, cap * sizeof(wchar_t));
    if (!nb) return 0;
    b->buf = nb; b->cap = cap;
    return 1;
}

void wbuf_addc(WBuf *b, wchar_t c)
{
    if (!wbuf_reserve(b, 1)) return;
    b->buf[b->len++] = c;
    b->buf[b->len] = 0;
}

void wbuf_addn(WBuf *b, const wchar_t *s, size_t n)
{
    if (!s || !n) return;
    if (!wbuf_reserve(b, n)) return;
    memcpy(b->buf + b->len, s, n * sizeof(wchar_t));
    b->len += n;
    b->buf[b->len] = 0;
}

void wbuf_add(WBuf *b, const wchar_t *s)
{
    if (s) wbuf_addn(b, s, wcslen(s));
}

void wbuf_addf(WBuf *b, const wchar_t *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vscwprintf(fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (!wbuf_reserve(b, (size_t)n)) return;
    va_start(ap, fmt);
    _vsnwprintf_s(b->buf + b->len, (size_t)n + 1, _TRUNCATE, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    b->buf[b->len] = 0;
}

/* ================================================================== */
/* PtrList                                                              */
/* ================================================================== */

void ptrlist_init(PtrList *l) { l->items = NULL; l->count = 0; l->cap = 0; }

void ptrlist_free(PtrList *l, void (*freefn)(void *))
{
    int i;
    if (!l) return;
    for (i = 0; i < l->count; i++)
        if (l->items[i] && freefn) freefn(l->items[i]);
    xfree(l->items);
    l->items = NULL; l->count = 0; l->cap = 0;
}

void ptrlist_push(PtrList *l, void *p)
{
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        void **ni = (void **)xrealloc(l->items, (size_t)cap * sizeof(void *));
        if (!ni) return;
        l->items = ni; l->cap = cap;
    }
    l->items[l->count++] = p;
}

void *ptrlist_get(const PtrList *l, int i)
{
    if (i < 0 || i >= l->count) return NULL;
    return l->items[i];
}

void ptrlist_set(PtrList *l, int i, void *p)
{
    if (i < 0 || i >= l->count) return;
    l->items[i] = p;
}

void ptrlist_remove_at(PtrList *l, int i)
{
    if (i < 0 || i >= l->count) return;
    memmove(&l->items[i], &l->items[i + 1], (size_t)(l->count - i - 1) * sizeof(void *));
    l->count--;
}

/* ================================================================== */
/* GDI+                                                                 */
/* ================================================================== */

static ULONG_PTR g_gpToken = 0;
static int       g_gpStarted = 0;

int gdiplus_start(void)
{
    GdiplusStartupInput in;
    GdiplusStartupOutput out;
    if (g_gpStarted) return 1;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_gpToken, &in, &out) != Ok) {
        app_log("GdiplusStartup failed");
        return 0;
    }
    g_gpStarted = 1;
    return 1;
}

/* uikit.c keeps a scratch surface for text measurement. It is created through
 * GDI+, so it has to go before GdiplusShutdown - a shutdown with live GDI+
 * objects leaves that cache pointing at freed memory, and the next startup
 * then crashes the first time a label is measured. */
void gx_purge_cache(void);

void gdiplus_stop(void)
{
    if (!g_gpStarted) return;
    gx_purge_cache();
    GdiplusShutdown(g_gpToken);
    g_gpToken = 0;
    g_gpStarted = 0;
}

int gdiplus_ok(void) { return g_gpStarted; }

static int get_encoder_clsid(const wchar_t *mime, CLSID *out)
{
    UINT  len = 0, bytes = 0;
    ImageCodecInfo *info = NULL;
    int i, r = 0;
    if (!mime || !out) return 0;
    if (GdipGetImageEncodersSize(&len, &bytes) != Ok || bytes == 0) return 0;
    info = (ImageCodecInfo *)LocalAlloc(LMEM_FIXED, bytes);
    if (!info) return 0;
    if (GdipGetImageEncoders(len, bytes, info) == Ok) {
        /* `len` is the encoder count (matches both MSVC and MinGW layouts) */
        for (i = 0; i < (int)len; i++) {
            if (info[i].MimeType && wcscmp(info[i].MimeType, mime) == 0) {
                *out = info[i].Clsid;
                r = 1;
                break;
            }
        }
    }
    LocalFree(info);
    return r;
}

static int get_png_encoder_clsid(CLSID *out)
{
    return get_encoder_clsid(L"image/png", out);
}

/* ------------------------------------------------------------------ */
/* In-memory IStream.                                                   */
/* MinGW ships no IStream_Seek()/IStream_Read() helpers, so we build the */
/* COM vtable by hand. The IStream vtable layout has been frozen since  */
/* Windows 3.1 and is identical across every shipped version of Windows. */
/* ------------------------------------------------------------------ */

typedef HRESULT (STDMETHODCALLTYPE *MS_QueryInterface)(IStream *, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *MS_AddRef)(IStream *);
typedef HRESULT (STDMETHODCALLTYPE *MS_Release)(IStream *);
typedef HRESULT (STDMETHODCALLTYPE *MS_Read)(IStream *, void *, ULONG, ULONG *);
typedef HRESULT (STDMETHODCALLTYPE *MS_Write)(IStream *, const void *, ULONG, ULONG *);
typedef HRESULT (STDMETHODCALLTYPE *MS_Seek)(IStream *, LARGE_INTEGER, DWORD, ULARGE_INTEGER *);
typedef HRESULT (STDMETHODCALLTYPE *MS_SetSize)(IStream *, ULARGE_INTEGER);
typedef HRESULT (STDMETHODCALLTYPE *MS_CopyTo)(IStream *, IStream *, ULARGE_INTEGER,
                                               ULARGE_INTEGER *, ULARGE_INTEGER *);
typedef HRESULT (STDMETHODCALLTYPE *MS_Commit)(IStream *, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *MS_Revert)(IStream *);
typedef HRESULT (STDMETHODCALLTYPE *MS_Stat)(IStream *, STATSTG *, DWORD);

typedef struct {
    MS_QueryInterface QueryInterface;
    MS_AddRef        AddRef;
    MS_Release       Release;
    MS_Read          Read;
    MS_Write         Write;
    MS_Seek          Seek;
    MS_SetSize       SetSize;
    MS_CopyTo        CopyTo;
    MS_Commit        Commit;
    MS_Revert        Revert;
    MS_Stat          Stat;
} MemStreamVtbl;

typedef struct {
    IStream           base;     /* must be first: vtable pointer */
    const MemStreamVtbl *vtbl;
    unsigned char    *data;
    size_t            cap;
    size_t            size;
    size_t            pos;
    LONG              refs;
} MemStream;

static HRESULT STDMETHODCALLTYPE ms_AddRef(IStream *s);

static HRESULT STDMETHODCALLTYPE ms_QueryInterface(IStream *s, REFIID riid, void **ppv)
{
    /* GDI+ queries the object for IID_IStream before writing to it, so we must
     * answer properly instead of failing - otherwise it dereferences NULL. */
    static const GUID IID_IUnknown_ = {
        0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
    };
    static const GUID IID_IStream_ = {
        0x0000000C, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
    };
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown_) || IsEqualIID(riid, &IID_IStream_)) {
        *ppv = s;
        ms_AddRef(s);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE ms_AddRef(IStream *s)
{
    MemStream *m = (MemStream *)s;
    return InterlockedIncrement(&m->refs);
}

static HRESULT STDMETHODCALLTYPE ms_Release(IStream *s)
{
    MemStream *m = (MemStream *)s;
    if (InterlockedDecrement(&m->refs) == 0) {
        xfree(m->data);
        m->data = NULL;
        xfree(m);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ms_Read(IStream *s, void *pv, ULONG cb, ULONG *pcb)
{
    MemStream *m = (MemStream *)s;
    size_t avail;
    if (!m || !pv) return E_POINTER;
    avail = m->size - m->pos;
    if (cb > avail) cb = (ULONG)avail;
    if (cb) memcpy(pv, m->data + m->pos, cb);
    m->pos += cb;
    if (pcb) *pcb = cb;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ms_Write(IStream *s, const void *pv, ULONG cb, ULONG *pcb)
{
    MemStream *m = (MemStream *)s;
    if (!m || (!pv && cb)) return E_POINTER;
    if (m->pos + cb > m->cap) {
        size_t cap = m->cap ? m->cap : 65536;
        unsigned char *nd;
        while (cap < m->pos + cb) cap *= 2;
        nd = (unsigned char *)xrealloc(m->data, cap);
        if (!nd) return E_OUTOFMEMORY;
        m->data = nd;
        m->cap  = cap;
    }
    if (cb) memcpy(m->data + m->pos, pv, cb);
    m->pos += cb;
    if (m->pos > m->size) m->size = m->pos;
    if (pcb) *pcb = cb;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ms_Seek(IStream *s, LARGE_INTEGER move,
                                         DWORD origin, ULARGE_INTEGER *pnew)
{
    MemStream *m = (MemStream *)s;
    LONGLONG base, target;
    if (!m) return E_POINTER;
    switch (origin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = (LONGLONG)m->pos; break;
    case STREAM_SEEK_END: base = (LONGLONG)m->size; break;
    default: return STG_E_INVALIDFUNCTION;
    }
    if (move.QuadPart > 0) {
        if (base > (LONGLONG)((ULONGLONG)-1 >> 1) - move.QuadPart)
            return STG_E_INVALIDFUNCTION;
        target = base + move.QuadPart;
    } else {
        if (base < -move.QuadPart) return STG_E_INVALIDFUNCTION;
        target = base + move.QuadPart;
    }
    m->pos = (size_t)target;
    if (pnew) pnew->QuadPart = (ULONGLONG)m->pos;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ms_SetSize(IStream *s, ULARGE_INTEGER newsize)
{
    (void)s; (void)newsize;
    return STG_E_INVALIDFUNCTION;   /* read-only stream */
}

static HRESULT STDMETHODCALLTYPE ms_CopyTo(IStream *s, IStream *dest,
                                           ULARGE_INTEGER cb,
                                           ULARGE_INTEGER *pread,
                                           ULARGE_INTEGER *pwritten)
{
    MemStream *m = (MemStream *)s;
    size_t n;
    ULONG got = 0;
    if (!m || !dest) return E_POINTER;
    n = (size_t)cb.QuadPart;
    if (n > m->size - m->pos) n = m->size - m->pos;
    if (n) {
        HRESULT hr = ms_Write(dest, m->data + m->pos, (ULONG)n, &got);
        if (FAILED(hr)) return hr;
        m->pos += got;
    }
    if (pread) pread->QuadPart = n;
    if (pwritten) pwritten->QuadPart = got;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ms_Commit(IStream *s, DWORD flags)
{
    (void)s; (void)flags;
    return S_OK;    /* memory stream: nothing to flush */
}

static HRESULT STDMETHODCALLTYPE ms_Revert(IStream *s)
{
    MemStream *m = (MemStream *)s;
    if (m) { m->pos = 0; }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ms_Stat(IStream *s, STATSTG *st, DWORD flags)
{
    MemStream *m = (MemStream *)s;
    if (!m || !st) return E_POINTER;
    memset(st, 0, sizeof(*st));
    st->type           = STGTY_STREAM;
    st->cbSize.QuadPart = (LONGLONG)m->size;
    st->grfMode        = STGM_READ;
    (void)flags;
    return S_OK;
}

static const MemStreamVtbl g_memStreamVtbl = {
    ms_QueryInterface, ms_AddRef, ms_Release, ms_Read, ms_Write,
    ms_Seek, ms_SetSize, ms_CopyTo, ms_Commit, ms_Revert, ms_Stat
};

static IStream *memstream_create(void)
{
    MemStream *m = (MemStream *)xcalloc(1, sizeof(MemStream));
    if (!m) return NULL;
    /* The first member IS the COM vtable pointer - it must be set, not the
     * convenience `vtbl` alias further down the struct. The two vtable struct
     * types differ only in the AddRef/Release return type (HRESULT vs ULONG);
     * they are ABI-identical on Windows, so the cast is safe. */
    m->base.lpVtbl = (IStreamVtbl *)(void *)&g_memStreamVtbl;
    m->vtbl = &g_memStreamVtbl;
    m->refs = 1;
    return (IStream *)m;
}

/* Takes ownership of `take` on success. */
static int memstream_take(IStream *s, unsigned char **take, size_t *takeLen)
{
    MemStream *m = (MemStream *)s;
    if (!m) return 0;
    *take   = m->data;
    *takeLen = m->size;
    m->data = NULL;
    m->size = m->cap = m->pos = 0;
    ms_Release(s);
    return 1;
}

int gdiplus_encode_png(const void *bits, int w, int h, int stride,
                       unsigned char **outBuf, size_t *outLen)
{
    CLSID clsid;
    GpBitmap *bmp = NULL;
    IStream *strm = NULL;
    int ok = 0;

    if (!bits || w <= 0 || h <= 0 || !outBuf || !outLen) return 0;
    *outBuf = NULL; *outLen = 0;
    if (!gdiplus_ok()) return 0;
    if (!get_png_encoder_clsid(&clsid)) return 0;

    /* GDI+ rejects a zero `scan0` when raw bits are supplied, so always hand
     * it the real stride even though 32bpp DIBs are always width*4. */
    if (GdipCreateBitmapFromScan0(w, h, stride ? (int)stride : w * 4,
                                  PixelFormat32bppARGB,
                                  (BYTE *)bits, &bmp) != Ok)
        return 0;

    strm = memstream_create();
    if (!strm) goto done;

    if (GdipSaveImageToStream(bmp, strm, &clsid, NULL) != Ok) goto done;

    ok = memstream_take(strm, outBuf, outLen);
    strm = NULL;   /* released by memstream_take on success */

done:
    if (bmp)  GdipDisposeImage((GpImage *)bmp);
    if (strm) ms_Release(strm);
    return ok;
}

int gdiplus_save_png_from_bits(const void *bits, int w, int h, int stride,
                               const wchar_t *path)
{
    unsigned char *buf = NULL;
    size_t len = 0;
    HANDLE f;
    DWORD written = 0;
    if (!gdiplus_encode_png(bits, w, h, stride, &buf, &len)) return 0;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { xfree(buf); return 0; }
    if (!WriteFile(f, buf, (DWORD)len, &written, NULL) || written != len) {
        CloseHandle(f); xfree(buf); return 0;
    }
    CloseHandle(f);
    xfree(buf);
    return 1;
}

int gdiplus_save_png(HBITMAP dib, const wchar_t *path)
{
    BITMAP bm;
    if (!dib) return 0;
    if (GetObjectW(dib, sizeof(bm), &bm) != sizeof(bm)) return 0;
    if (!bm.bmBits) return 0;
    return gdiplus_save_png_from_bits(bm.bmBits, bm.bmWidth, bm.bmHeight,
                                      bm.bmWidthBytes, path);
}

int gdiplus_save_buffer(const void *bits, int w, int h, int stride,
                        const wchar_t *path, const wchar_t *mime, int quality)
{
    CLSID  clsid;
    GpBitmap *bmp = NULL;
    EncoderParameters ep;
    ULONG  q = 0;
    int    jpeg;

    if (!bits || w <= 0 || h <= 0 || !path || !*path || !mime) return 0;
    if (!gdiplus_ok()) return 0;
    if (!get_encoder_clsid(mime, &clsid)) return 0;

    jpeg = (wcscmp(mime, L"image/jpeg") == 0);
    memset(&ep, 0, sizeof(ep));
    if (jpeg) {
        if (quality < 1) quality = 1;
        if (quality > 100) quality = 100;
        q = (ULONG)quality;
        /* `Parameter` is declared as a one-element array, so setting index 0
         * in place is correct - no separate allocation, and no uninitialised
         * tail for GDI+ to read past Count. */
        ep.Count = 1;
        ep.Parameter[0].Guid = EncoderQuality;
        ep.Parameter[0].NumberOfValues = 1;
        ep.Parameter[0].Type = EncoderParameterValueTypeLong;
        ep.Parameter[0].Value = &q;
    }

    if (GdipCreateBitmapFromScan0(w, h, stride ? (int)stride : w * 4,
                                  PixelFormat32bppARGB,
                                  (BYTE *)bits, &bmp) != Ok)
        return 0;
    if (GdipSaveImageToFile((GpImage *)bmp, path, &clsid,
                            jpeg ? &ep : NULL) != Ok) {
        GdipDisposeImage((GpImage *)bmp);
        return 0;
    }
    GdipDisposeImage((GpImage *)bmp);
    return 1;
}
