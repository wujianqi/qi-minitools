/* slice.c - the window: canvas, slice tree, control panel, tray, command line.
 *
 * The canvas is painted by this file with GDI+ into a back buffer, the same way
 * the picker does it, because a slice border needs a 1px line that stays
 * readable over a white sky and a black silhouette in the same picture. The
 * right-hand panel is made of ordinary Win32 controls instead: the prefix and
 * size fields need real text editing and a real IME, which is not something
 * to reimplement by hand on top of WM_CHAR.
 *
 * Four gestures on the canvas, told apart by button and by where the press
 * landed: a click picks the cell under it, a left drag draws a slice line
 * through everything it crosses, a press that lands on a border drags that grid
 * line instead, and a right drag (or Ctrl+left drag) rubber-bands a selection
 * over several cells for 合并. There is no tool palette to keep switching
 * between - the gesture that makes sense is the one the pointer is already on.
 */
#include "slice.h"
#include <shlobj.h>
#include <shobjidl.h>

#define SL_CLASS   L"MuskSliceMain"
#define HOTKEY_ID  1

/* Smallest client area that still holds the whole control panel. Enforced by
 * WM_GETMINMAXINFO, so the user cannot drag the frame smaller than its own
 * contents; the self-test checks the layout at and above these. */
#define SL_MIN_CLIENT_W 560
#define SL_MIN_CLIENT_H 520

/* This program's own half of the About box. The name, the icon and the credit
 * line are added by app_show_about() (src\common\about.c), so all four tools
 * credit the same author without four copies of the text to keep in step.
 * Declared up here because the panel's 关于 button asks for them long before
 * the command-line switch near wWinMain does. */
static const wchar_t *SLICE_ABOUT_NAME  = L"MuskSlice  ·  图片分割器";
/* Kept to one line per gesture on purpose: the About box has a fixed text
 * area, and anything past about ten lines is cut off at the bottom rather than
 * scrolled - a list whose last entries cannot be read is worse than a shorter
 * one. The panel hint carries the detail. */
static const wchar_t *SLICE_ABOUT_NOTES =
    L"选格      在图上点一下\r\n"
    L"画线      顺着拖动方向画：横拖出横线，竖拖出竖线\r\n"
    L"拖网格线  光标放到格线上会变成 ↔ ↕，按住左右/上下拖，整条线一起动；\r\n"
    L"          方向键逐像素微调，按住 Alt 拖动则一律画切割线\r\n"
    L"框选      右键拖出选区（Ctrl+左键也行），自动吸附格子边缘\r\n"
    L"合并      按 M 或点「合并」并成一格；Shift 点第二格，Ctrl+A 全选\r\n"
    L"细分      选中格后点「横分」「竖分」或按 S\r\n"
    L"删除      选中格后按 Delete，该格并回上一级\r\n"
    L"整图布局  按行均分 / 按列均分 / 按尺寸切 / 重置\r\n"
    L"导出      Ctrl+E\r\n"
    L"批处理    slice.exe --split 图.png --move v 100 160 --merge 0,0,100,200";

/* How far the pointer must travel before a press counts as a drag rather
 * than a click. Below this the hand shakes and every click would draw a line. */
#define SL_DRAG_SLOP 4

/* How close to a slice border, in logical pixels, the pointer has to be before
 * it is treated as being on that border and the border becomes draggable.
 *
 * This number decides whether the feature is usable. The left button means
 * three different things - pick a cell, draw a cut, move a grid line - and the
 * only thing telling them apart is where the press landed. At 4px it was a
 * pixel-hunt: the pointer has to be right on a 1px line, and when it misses,
 * the drag silently becomes a cut and the user concludes the feature does not
 * work at all. At 8px it is a normal resize border, still far enough from the
 * middle of a cell that "drag here to cut" is not ambushed - and Alt forces a
 * cut anywhere, so nobody is ever stuck. */
#define SL_GRAB_PX   8

/* Which canvas gesture is running. A press is undecided until the pointer
 * travels, and a press that lands on a grid line is a fourth gesture rather
 * than a click-or-cut: it moves the line instead. The rubber band runs on the
 * right button, which is free - the left button already means "pick a cell" and
 * "draw a cut", and merging is impossible without picking more than one cell,
 * so it cannot be left to a modifier key nobody will find. */
enum { GESTURE_NONE = 0, GESTURE_CLICK, GESTURE_CUT, GESTURE_LINE, GESTURE_BAND };

/* control ids - all inside 100..199 so they cannot collide with the tray menu
 * ids, which arrive at WM_COMMAND with lParam == 0 (see sl_proc) */
enum {
    IDC_OPEN = 100, IDC_BROWSE, IDC_EXPORT,
    IDC_ROWS, IDC_ROWS_MINUS, IDC_ROWS_PLUS,
    IDC_COLS, IDC_COLS_MINUS, IDC_COLS_PLUS,
    IDC_CELL, IDC_FIXED,
    IDC_EQ_ROWS, IDC_EQ_COLS, IDC_RESET,
    IDC_DIVN, IDC_DIV_H, IDC_DIV_V, IDC_DIV_ALL, IDC_DEL, IDC_MERGE,
    IDC_FORMAT, IDC_QUALITY, IDC_PREFIX, IDC_OUTDIR, IDC_ABOUT,
    IDC_LAST = 199
};

typedef struct {
    RECT panel, canvas, status;
    RECT open, browse, export;
    RECT rowsLbl, rowsMinus, rowsEdit, rowsPlus;
    RECT colsLbl, colsMinus, colsEdit, colsPlus;
    RECT cellLbl, cellEdit, fixed;
    RECT eqRows, eqCols, reset;
    RECT selNote;
    RECT divLbl, divEdit, divH, divV;
    RECT divAll, delSel, mergeSel;
    RECT fmtLbl, fmt, qualLbl, qual, prefixLbl, prefix, dirLbl, dir;
    RECT about;
    RECT hint;
} Layout;

typedef struct {
    HINSTANCE   inst;
    HWND        hwnd;
    int         quit;
    Tray        tray;

    SourceImage src;
    SliceTree   tree;

    double      scale;
    RECT        view;       /* the image's rect in client coords */

    Slice      *sel;        /* the picked region, leaf or not, or NULL */
    Slice      *hover;      /* the leaf under the cursor */

    /* The box 合并 acts on. A plain click leaves it on the picked cell; a
     * Shift-click spans it between the two cells; a rubber band snaps it to the
     * cells it covers. All three are plain rectangles rather than region
     * pointers on purpose - the merge and the moved line both rebuild the tree,
     * and anything pointing into the old tree would be left dangling by them.
     * Empty (right <= left) means nothing is picked. */
    RECT        selRect;
    RECT        selFrom;    /* the cell the box was measured from */
    int         selWide;    /* the box covers more than one cell */

    /* the line being dragged, in image coordinates; live only while drawing */
    int         down;       /* the button is held; click-or-drag undecided */
    int         gesture;    /* which canvas gesture is running */
    int         drawing;
    int         lineAxis;   /* 0 = vertical, 1 = horizontal */
    int         linePos;
    int         lineLo, lineHi;
    int         downX, downY;

    /* The grid line under the pointer, and the one being dragged. axis 0 is a
     * vertical line at x = pos, 1 a horizontal one at y = pos; lo/hi bound the
     * run of collinear edges the grab belongs to, which is a whole grid line
     * rather than the single segment under the cursor. `grabOff` keeps the
     * pointer from teleporting the line to the pixel centre on the first move,
     * and haveLast remembers the line last touched so the arrow keys can nudge
     * it after the pointer has wandered off. */
    int         hoverAxis, hoverPos, hoverLo, hoverHi, hoverGrab, haveHover;
    POINT       hoverPt;   /* where the pointer is, for the hover handle */
    int         grabAxis, grabPos, grabLo, grabHi, grabOff;
    int         dragTo, dragLo, dragHi;
    int         lastAxis, lastPos, lastLo, lastHi, haveLast;

    /* The rubber band. Held in screen coordinates because it is a gesture being
     * drawn rather than a thing in the picture - it snaps to cell edges only
     * when the button comes up, so it tracks the pointer exactly while it is
     * still in the user's hand. */
    int         bandDown;
    POINT       bandFrom, bandTo;

    int         rowsN, colsN, cellPx, divN;
    int         asJpeg, quality;
    wchar_t     prefix[128];
    wchar_t     outDir[MAX_PATH];

    wchar_t     msg[192];
    int         msgIsError;

    HFONT       uiFont;
    int         uiFontPx;

    HWND        ctl[IDC_LAST + 1];
    /* the panel rectangle the controls were last placed for; the controls are
     * only moved when it changes, because MoveWindow on an unchanged rect
     * still makes an EDIT forget its scroll offset and jump the caret */
    RECT        ctlPlaced;

    HBITMAP     memBmp;
    void       *memBits;
    GpBitmap  *mem;
    int         memW, memH;

    /* checkerboard behind a transparent PNG, built as a real bitmap rather
     * than tiled with a texture brush - one blit per paint, and the pattern
     * stays square on screen whatever the zoom is */
    HBITMAP     chkBmp;
    void       *chkBits;
    GpBitmap  *chkGp;
    int         chkW, chkH, chkCell;

    GpFontFamily *fam, *mono;
} Slicer;

static Slicer g_sl;
static Layout g_lay;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_msg(const wchar_t *s, int isError)
{
    wcsncpy(g_sl.msg, s ? s : L"", 191);
    g_sl.msg[191] = 0;
    g_sl.msgIsError = isError;
    if (g_sl.hwnd) InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

static int read_int(HWND h, int fallback)
{
    wchar_t buf[32];
    int v;
    if (!h) return fallback;
    GetWindowTextW(h, buf, 32);
    buf[31] = 0;
    wcs_trim(buf);
    if (!buf[0]) return fallback;
    v = _wtoi(buf);
    return v;
}

static void write_int(HWND h, int v)
{
    wchar_t buf[32];
    if (!h) return;
    _snwprintf_s(buf, 32, _TRUNCATE, L"%d", v);
    SetWindowTextW(h, buf);
}

/* Joins a directory and a file name with exactly one separator.
 *
 * The output folder reaches the app three ways - from the folder picker, typed
 * into the edit box, or defaulted to <source>_slices - and only one of them
 * guarantees a trailing backslash. Concatenating without this put
 * "photo_slices_01.png" in the user's pictures folder. */
static void dir_join(wchar_t *out, size_t cch, const wchar_t *dir, const wchar_t *name)
{
    size_t n = 0;
    if (!out || cch == 0) return;
    out[0] = 0;
    if (dir) {
        n = wcslen(dir);
        if (n >= cch - 2) n = cch - 2;
        wcsncpy(out, dir, n);
        out[n] = 0;
    }
    if (n > 0 && out[n - 1] != L'\\' && out[n - 1] != L'/') {
        if (n + 1 < cch) { out[n] = L'\\'; out[n + 1] = 0; }
    }
    if (name) wcsncpy(out + wcslen(out), name, cch - wcslen(out) - 1);
}

/* The 1-based number the canvas badge shows for a region, or 0 if it is not
 * one of the exported leaves (an already-divided region, or a stale pointer). */
static int leaf_number(const Slice *s)
{
    Slice **lv;
    int n = 0, i, found = 0;
    if (!s) return 0;
    lv = st_leaves(&g_sl.tree, &n);
    if (lv) {
        for (i = 0; i < n; i++) {
            if (lv[i] == s) { found = i + 1; break; }
        }
        xfree(lv);
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* transparency checkerboard                                          */
/* ------------------------------------------------------------------ */

static void checker_sync(int w, int h, int cell)
{
    int x, y, c;

    if (w < 1 || h < 1) return;
    if (cell < 4) cell = 4;
    if (cell > 64) cell = 64;
    if (g_sl.chkBmp && g_sl.chkW == w && g_sl.chkH == h && g_sl.chkCell == cell) return;

    if (g_sl.chkGp) { GdipDisposeImage((GpImage *)g_sl.chkGp); g_sl.chkGp = NULL; }
    if (g_sl.chkBmp) { free_dib(g_sl.chkBmp, g_sl.chkBits); g_sl.chkBmp = NULL; }
    g_sl.chkBits = NULL;
    g_sl.chkW = g_sl.chkH = g_sl.chkCell = 0;

    g_sl.chkBmp = make_dib(w, h, 32, &g_sl.chkBits);
    if (!g_sl.chkBmp) return;
    for (y = 0; y < h; y++) {
        BGRAPX *row = (BGRAPX *)((BYTE *)g_sl.chkBits + (size_t)y * w * 4);
        for (x = 0; x < w; x++) {
            c = (((x / cell) + (y / cell)) & 1) ? 0xFFE3E5E9 : 0xFFFFFFFF;
            row[x].b = (BYTE)(c & 0xFF);
            row[x].g = (BYTE)((c >> 8) & 0xFF);
            row[x].r = (BYTE)((c >> 16) & 0xFF);
            row[x].a = 255;
        }
    }
    if (GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  (BYTE *)g_sl.chkBits, &g_sl.chkGp) != Ok)
        g_sl.chkGp = NULL;
    g_sl.chkW = w;
    g_sl.chkH = h;
    g_sl.chkCell = cell;
}

static void checker_release(void)
{
    if (g_sl.chkGp) { GdipDisposeImage((GpImage *)g_sl.chkGp); g_sl.chkGp = NULL; }
    if (g_sl.chkBmp) { free_dib(g_sl.chkBmp, g_sl.chkBits); g_sl.chkBmp = NULL; }
    g_sl.chkBits = NULL;
    g_sl.chkW = g_sl.chkH = g_sl.chkCell = 0;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

/* Places one panel rectangle: offsets are relative to the current row's left
 * edge and top, so a row is a single y and a stack of x positions. */
#define LR(id, y0, x0, x1, y1) SetRect(&g_lay.id, px + (x0), y + (y0), px + (x1), y + (y1))

static void place_controls(void)
{
    static const struct { int id; const RECT *r; } map[] = {
        { IDC_OPEN,      &g_lay.open },
        { IDC_BROWSE,    &g_lay.browse },
        { IDC_EXPORT,    &g_lay.export },
        { IDC_ROWS,      &g_lay.rowsEdit },
        { IDC_ROWS_MINUS,&g_lay.rowsMinus },
        { IDC_ROWS_PLUS, &g_lay.rowsPlus },
        { IDC_COLS,      &g_lay.colsEdit },
        { IDC_COLS_MINUS,&g_lay.colsMinus },
        { IDC_COLS_PLUS, &g_lay.colsPlus },
        { IDC_CELL,      &g_lay.cellEdit },
        { IDC_FIXED,     &g_lay.fixed },
        { IDC_EQ_ROWS,   &g_lay.eqRows },
        { IDC_EQ_COLS,   &g_lay.eqCols },
        { IDC_RESET,     &g_lay.reset },
        { IDC_DIVN,      &g_lay.divEdit },
        { IDC_DIV_H,     &g_lay.divH },
        { IDC_DIV_V,     &g_lay.divV },
        { IDC_DIV_ALL,   &g_lay.divAll },
        { IDC_DEL,       &g_lay.delSel },
        { IDC_MERGE,     &g_lay.mergeSel },
        { IDC_FORMAT,    &g_lay.fmt },
        { IDC_QUALITY,   &g_lay.qual },
        { IDC_PREFIX,    &g_lay.prefix },
        { IDC_OUTDIR,    &g_lay.dir },
        { IDC_ABOUT,     &g_lay.about }
    };
    size_t k;
    for (k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
        HWND hw = g_sl.ctl[map[k].id];
        if (!hw) continue;
        MoveWindow(hw, map[k].r->left, map[k].r->top,
                   map[k].r->right - map[k].r->left,
                   map[k].r->bottom - map[k].r->top, TRUE);
    }
}

static void build_layout(int W, int H)
{
    int S = app_scale_px(1);
    int panelW = S * 282;
    int statusH = S * 28;
    int px, pw, y, h, third;

    memset(&g_lay, 0, sizeof(g_lay));
    if (panelW > W * 2 / 3) panelW = W * 2 / 3;

    SetRect(&g_lay.panel,  W - panelW, 0, W, H - statusH);
    SetRect(&g_lay.canvas, 0, 0, W - panelW, H - statusH);
    SetRect(&g_lay.status, 0, H - statusH, W, H);

    px = g_lay.panel.left + S * 14;
    pw = panelW - S * 28;
    third = (pw - S * 16) / 3;

    y = S * 14;
    /* 打开图片 keeps the room it needs and 关于 takes the right-hand end of the
     * same row. The panel is the only place this window has for chrome, and the
     * top row is the one a reader looks at first - a 关于 buried under the
     * output directory is a 关于 nobody opens. */
    LR(open,  0, 0,           pw - S * 64, S * 30);
    LR(about, 0, pw - S * 60, pw,         S * 30);

    /* rows and columns, two stepper groups side by side. Each group is laid out
     * inside its own half: a label, then - [value] +. The offsets are measured
     * against the half's left edge, because a group sized off the panel's own
     * origin runs the second one off the right-hand side. */
    y += S * 30 + S * 14;
    h = S * 24;
    {
        int step = (pw - S * 6) / 2;
        int x2  = step + S * 6;
        if (step >= S * 118) {
            LR(rowsLbl,   S * 4, 0,           S * 20,  h);
            LR(rowsMinus, 0,     S * 22,      S * 44,  h);
            LR(rowsEdit,  0,     S * 48,      S * 92,  h);
            LR(rowsPlus,  0,     S * 96,      S * 118, h);
            LR(colsLbl,   S * 4, x2,          x2 + S * 20,  h);
            LR(colsMinus, 0,     x2 + S * 22, x2 + S * 44,  h);
            LR(colsEdit,  0,     x2 + S * 48, x2 + S * 92,  h);
            LR(colsPlus,  0,     x2 + S * 96, x2 + S * 118, h);
        } else {
            LR(rowsLbl,   S * 4, 0,      S * 20,  h);
            LR(rowsMinus, 0,     S * 22, S * 44,  h);
            LR(rowsEdit,  0,     S * 48, S * 92,  h);
            LR(rowsPlus,  0,     S * 96, S * 118, h);
            y += h + S * 6;
            LR(colsLbl,   S * 4, 0,      S * 20,  h);
            LR(colsMinus, 0,     S * 22, S * 44,  h);
            LR(colsEdit,  0,     S * 48, S * 92,  h);
            LR(colsPlus,  0,     S * 96, S * 118, h);
        }
    }

    /* fixed cell size, applied to both axes at once */
    y += h + S * 10;
    LR(cellLbl,  S * 4, 0,      S * 52,  h);
    LR(cellEdit, 0,     S * 56, S * 112, h);
    LR(fixed,    0,     S * 118, pw,    h);

    /* the three whole-picture layouts */
    y += h + S * 12;
    h = S * 26;
    LR(eqRows, 0, 0,                   third,             h);
    LR(eqCols, 0, third + S * 8,       third * 2 + S * 8, h);
    LR(reset,  0, third * 2 + S * 16,  pw,               h);

    /* the picked-region tools, under their own heading */
    y += h + S * 14;
    LR(selNote, 0, 0, pw, S * 20);

    y += S * 20;
    h = S * 24;
    LR(divLbl,  S * 4, 0,      S * 40,  h);
    LR(divEdit, 0,     S * 44, S * 88,  h);
    LR(divH,    0,     S * 94, S * 170, h);
    LR(divV,    0,     S * 176, pw,     h);

    y += h + S * 8;
    h = S * 26;
    {
        /* Three on one row rather than four rows of two. Merging belongs beside
         * deleting - both act on what is picked and both hand the pixels back -
         * and a fourth row would push the export button, which is the one
         * control that must never fall off the bottom of the panel. */
        int t3 = (pw - S * 16) / 3;
        LR(divAll,   0, 0,                     t3,             h);
        LR(delSel,  0, t3 + S * 8,             t3 * 2 + S * 8,  h);
        LR(mergeSel, 0, t3 * 2 + S * 16,        pw,             h);
    }

    /* output */
    y += h + S * 18;
    LR(fmtLbl,  S * 5, 0,      S * 38,  h);
    LR(fmt,     0,     S * 42, S * 134, h);
    LR(qualLbl, S * 5, S * 142, S * 180, h);
    LR(qual,    0,     S * 184, pw,     h);

    y += h + S * 10;
    LR(prefixLbl, S * 5, 0,      S * 38, h);
    LR(prefix,    0,     S * 42, pw,     h);

    y += h + S * 10;
    LR(dirLbl, S * 5, 0,           S * 38,      h);
    LR(browse, 0,     pw - S * 70, pw,          h);
    LR(dir,    0,     0,           pw - S * 76, h);

    y += h + S * 18;
    LR(export, 0, 0, pw, S * 34);

    /* The hint is the only decorative thing in the panel, so it is the one row
     * that gets dropped when the window is short. Sliding it off the bottom
     * edge would leave a control painted where nothing can click it. */
    y += S * 34 + S * 12;
    if (y + S * 42 <= g_lay.panel.bottom) LR(hint, 0, 0, pw, S * 42);

    if (!EqualRect(&g_sl.ctlPlaced, &g_lay.panel)) {
        g_sl.ctlPlaced = g_lay.panel;
        place_controls();
    }
}

static void build_view(void)
{
    RECT c = g_lay.canvas;
    int cw = c.right - c.left, ch = c.bottom - c.top;
    double s;

    SetRect(&g_sl.view, 0, 0, 0, 0);
    g_sl.scale = 1.0;
    if (!g_sl.src.gp || cw <= 0 || ch <= 0) return;

    s = (double)cw / (double)g_sl.src.w;
    if ((double)ch / (double)g_sl.src.h < s) s = (double)ch / (double)g_sl.src.h;
    /* No lower clamp: a small icon is scaled up too, so it can actually be
     * worked on, and the image is drawn nearest-neighbour above 1:1 so those
     * pixels stay square instead of turning to mush. */
    if (s > 8.0) s = 8.0;
    if (s <= 0.0) return;
    g_sl.scale = s;
    g_sl.view.left   = c.left + (cw - (int)(g_sl.src.w * s + 0.5)) / 2;
    g_sl.view.top    = c.top  + (ch - (int)(g_sl.src.h * s + 0.5)) / 2;
    g_sl.view.right  = g_sl.view.left + (int)(g_sl.src.w * s + 0.5);
    g_sl.view.bottom = g_sl.view.top  + (int)(g_sl.src.h * s + 0.5);
}
/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static void paint_label(GpGraphics *g, const RECT *r, const wchar_t *s, ARGB c)
{
    GpFont *f = gx_font(g_sl.fam, (REAL)app_scale_px(12));
    if (!f || !r->right || !s || !*s) return;
    gx_draw_text(g, f, c, s, (REAL)r->left, (REAL)r->top,
                 (REAL)(r->right - r->left), (REAL)(r->bottom - r->top));
    GdipDeleteFont(f);
}

static void paint_panel(GpGraphics *g)
{
    int S = app_scale_px(1);
    RECT p = g_lay.panel;
    wchar_t note[96];

    gx_fill_rect(g, 0xFFF6F6F8, (REAL)p.left, (REAL)p.top,
                 (REAL)(p.right - p.left), (REAL)(p.bottom - p.top));
    gx_stroke_line(g, 0xFFE2E2E6, 1.0f, (REAL)p.left, (REAL)p.top,
                   (REAL)p.left, (REAL)p.bottom);

    paint_label(g, &g_lay.rowsLbl,  L"行", 0xFF4B5563);
    paint_label(g, &g_lay.colsLbl,  L"列", 0xFF4B5563);
    paint_label(g, &g_lay.cellLbl,  L"单元格", 0xFF4B5563);
    paint_label(g, &g_lay.fmtLbl,   L"格式", 0xFF4B5563);
    paint_label(g, &g_lay.qualLbl,  L"质量", 0xFF4B5563);
    paint_label(g, &g_lay.prefixLbl, L"前缀", 0xFF4B5563);
    paint_label(g, &g_lay.dirLbl,   L"目录", 0xFF4B5563);
    paint_label(g, &g_lay.divLbl,   L"细分", 0xFF4B5563);

    /* The heading names what the tools below it act on, so it is never stale:
     * it says which region is picked, or that everything will be. With a box
     * spanning several cells it says so instead, because that is the case where
     * the number on the canvas badge and the number 合并 acts on disagree, and
     * the user needs to know which one they are about to change. */
    if (g_sl.sel) {
        int num = leaf_number(g_sl.sel);
        if (g_sl.selWide && g_sl.selRect.right > g_sl.selRect.left) {
            int aligned = 0;
            int inBox = st_rect_cells(&g_sl.tree, g_sl.selRect, &aligned);
            _snwprintf_s(note, 96, _TRUNCATE,
                         L"选区 %d 格  (%d x %d)%s",
                         inBox,
                         g_sl.selRect.right - g_sl.selRect.left,
                         g_sl.selRect.bottom - g_sl.selRect.top,
                         aligned ? L"" : L"  未对齐");
        } else if (num) {
            _snwprintf_s(note, 96, _TRUNCATE, L"选中格  #%d  (%d x %d)",
                         num,
                         g_sl.sel->r.right - g_sl.sel->r.left,
                         g_sl.sel->r.bottom - g_sl.sel->r.top);
        } else {
            _snwprintf_s(note, 96, _TRUNCATE, L"选中格  (%d x %d)",
                         g_sl.sel->r.right - g_sl.sel->r.left,
                         g_sl.sel->r.bottom - g_sl.sel->r.top);
        }
    } else {
        wcsncpy(note, L"选中格  未选择（细分将作用于全部）", 95);
        note[95] = 0;
    }
    paint_label(g, &g_lay.selNote, note, g_sl.sel ? 0xFF7C3AED : 0xFF9AA0A6);

    paint_label(g, &g_lay.hint,
                L"格线可拖动（按住 Alt 画线）；右键拖出选区，按 M 合并。", 0xFF9AA0A6);
    (void)S;
}

static void paint_cell_badge(GpGraphics *g, REAL x, REAL y, int index, int S)
{
    wchar_t num[12];
    GpFont *f;
    REAL d = (REAL)(S * 15);
    _snwprintf_s(num, 12, _TRUNCATE, L"%d", index);
    gx_round_rect_bg(g, 0xB0202026, x, y, d, d, (REAL)(S * 4));
    f = gx_font(g_sl.mono, (REAL)(S * 9));
    if (f) {
        gx_draw_text(g, f, 0xFFFFFFFF, num, x + (REAL)(S * 2), y + (REAL)(S * 3), d, d);
        GdipDeleteFont(f);
    }
}

/* One slice border, drawn as a dark line under a light one. A single light
 * line vanishes into a white sky and a single dark one into a silhouette, so
 * every border gets both. */
static void paint_border(GpGraphics *g, REAL x, REAL y, REAL ww, REAL hh)
{
    gx_stroke_rect(g, 0x66000000, 1.0f, x + 0.5f, y + 0.5f, ww - 1.0f, hh - 1.0f);
    gx_stroke_rect(g, 0xE6FFFFFF, 1.0f, x + 0.5f, y + 0.5f, ww - 1.0f, hh - 1.0f);
}

/* The box the 合并 button acts on.
 *
 * It is drawn as a wash plus a dashed edge rather than as another border: on top
 * of the slice borders it has to be told apart from them at a glance, and a
 * second solid line is exactly what a border already looks like. The wash also
 * leaves the individual cells inside the box readable, which matters because the
 * point of the box is to say "these go together". */
static void paint_sel_box(GpGraphics *g, int S)
{
    const RECT *r = &g_sl.selRect;
    double sc = g_sl.scale;
    REAL x0, y0, ww, hh;

    if (g_sl.scale <= 0 || r->right <= r->left || r->bottom <= r->top) return;
    x0 = (REAL)g_sl.view.left + (REAL)r->left * sc;
    y0 = (REAL)g_sl.view.top + (REAL)r->top * sc;
    ww = (REAL)(r->right - r->left) * sc;
    hh = (REAL)(r->bottom - r->top) * sc;
    if (ww < 2.0f) ww = 2.0f;
    if (hh < 2.0f) hh = 2.0f;
    gx_fill_rect(g, 0x267C3AED, x0, y0, ww, hh);
    gx_stroke_dashed_rect(g, 0xFF7C3AEDu, (REAL)(S * 2), x0 + 1.0f, y0 + 1.0f,
                          ww - 2.0f, hh - 2.0f);
}

/* The rubber band, while it is being dragged. Dashed, and a different colour
 * from the committed selection box, because the two occupy the same place and
 * the band is only ever the provisional version of it. */
static void paint_band(GpGraphics *g, int S)
{
    REAL x0, y0, ww, hh;
    if (!g_sl.bandDown) return;
    x0 = (REAL)((g_sl.bandFrom.x < g_sl.bandTo.x) ? g_sl.bandFrom.x : g_sl.bandTo.x);
    y0 = (REAL)((g_sl.bandFrom.y < g_sl.bandTo.y) ? g_sl.bandFrom.y : g_sl.bandTo.y);
    ww = (REAL)abs(g_sl.bandTo.x - g_sl.bandFrom.x);
    hh = (REAL)abs(g_sl.bandTo.y - g_sl.bandFrom.y);
    gx_fill_rect(g, 0x1F2563EB, x0, y0, ww, hh);
    gx_stroke_dashed_rect(g, 0xFF2563EBu, (REAL)(S), x0 + 0.5f, y0 + 0.5f,
                          ww - 1.0f, hh - 1.0f);
}

/* A grid line, drawn along the whole run of collinear edges it is made of, with
 * a grip at the middle.
 *
 * The grip is the whole reason the borders look draggable. A border on its own
 * could just as well be a fixed division, and finding out otherwise by trying is
 * a worse way to learn it than being shown. The line is drawn dark under a
 * coloured one for the same reason the cell borders are: it has to stay legible
 * over both a white sky and a black silhouette. */
static void paint_line_grip(GpGraphics *g, int S, int axis, int pos,
                            int lo, int hi, int active, int ax, int ay)
{
    double sc = g_sl.scale;
    REAL vx = (REAL)g_sl.view.left, vy = (REAL)g_sl.view.top;
    ARGB line = active ? 0xFF0EA5E9u : 0xCC9AA3B2u;
    REAL w  = active ? (REAL)(S * 4) : (REAL)(S * 3);
    REAL gw = (REAL)(S * 12), gh = (REAL)(S * 9);
    REAL cx, cy;

    if (g_sl.scale <= 0 || hi <= lo) return;
    if (axis == 0) {
        REAL x = vx + (REAL)pos * sc;
        REAL y0 = vy + (REAL)lo * sc, y1 = vy + (REAL)hi * sc;
        cx = (REAL)ax; cy = (REAL)ay;
        /* Clamped to the run, so the handle cannot wander off the line it
         * belongs to when the pointer is past either end of it. */
        if (cy < y0) cy = y0;
        if (cy > y1) cy = y1;
        gx_stroke_line(g, 0x77000000u, w + 1.0f, x, y0, x, y1);
        gx_stroke_line(g, line, w, x, y0, x, y1);
    } else {
        REAL y = vy + (REAL)pos * sc;
        REAL x0 = vx + (REAL)lo * sc, x1 = vx + (REAL)hi * sc;
        cx = (REAL)ax; cy = (REAL)ay;
        if (cx < x0) cx = x0;
        if (cx > x1) cx = x1;
        gx_stroke_line(g, 0x77000000u, w + 1.0f, x0, y, x1, y);
        gx_stroke_line(g, line, w, x0, y, x1, y);
    }
    /* The handle sits under the pointer, not at the middle of the run. A
     * handle half the picture away from the cursor is not read as "this is the
     * thing you grab" - it reads as a decoration on some other line, and the
     * user has no way to tell where the grab zone actually is. Under the
     * pointer it doubles as a cursor readout: it shows which spot of the line
     * the press would take hold of. */
    gx_round_rect_bg(g, active ? 0xFF0EA5E9u : 0xE6FFFFFFu,
                     cx - gw / 2, cy - gh / 2, gw, gh, (REAL)(S * 3));
    {
        GpFont *f = gx_font(g_sl.mono, (REAL)(S * 8));
        if (f) {
            gx_draw_text(g, f, active ? 0xFFFFFFFFu : 0xFF0EA5E9u,
                         axis == 0 ? L"\u2194" : L"\u2195",
                         cx - gw / 2, cy - gh / 2, gw, gh);
            GdipDeleteFont(f);
        }
    }
}

static void paint_tree(GpGraphics *g, int S)
{
    Slice **lv;
    int n = 0, i;
    double sc = g_sl.scale;
    REAL vx = (REAL)g_sl.view.left, vy = (REAL)g_sl.view.top;

    lv = st_leaves(&g_sl.tree, &n);
    if (lv) {
        for (i = 0; i < n; i++) {
            const RECT *r = &lv[i]->r;
            REAL x0 = vx + (REAL)r->left * sc;
            REAL y0 = vy + (REAL)r->top * sc;
            REAL ww = (REAL)(r->right - r->left) * sc;
            REAL hh = (REAL)(r->bottom - r->top) * sc;
            ARGB wash;

            if (ww < 1.0f) ww = 1.0f;
            if (hh < 1.0f) hh = 1.0f;
            /* A wash over every slice plus its border: either alone is
             * invisible on part of the picture, together they make the tiling
             * readable whatever is underneath. */
            if (lv[i] == g_sl.hover)      wash = 0x2E7C3AED;
            else if (lv[i] == g_sl.sel)   wash = 0x3A7C3AED;
            else                          wash = 0x16000000;
            gx_fill_rect(g, wash, x0, y0, ww, hh);
            paint_border(g, x0, y0, ww, hh);
            if (ww > (REAL)(S * 28) && hh > (REAL)(S * 20))
                paint_cell_badge(g, x0 + (REAL)(S * 3), y0 + (REAL)(S * 3), i + 1, S);
        }
        xfree(lv);
    }

    /* The picked region gets its own outline, drawn after every border so it
     * sits on top. A region that has already been divided is not a leaf and so
     * has no border of its own, but it still has to be shown: it is what the
     * next 细分 click will act on, and leaving it unmarked would make the
     * repeated subdividing look like it had stopped working. */
    if (g_sl.sel) {
        const RECT *r = &g_sl.sel->r;
        REAL x0 = vx + (REAL)r->left * sc;
        REAL y0 = vy + (REAL)r->top * sc;
        REAL ww = (REAL)(r->right - r->left) * sc;
        REAL hh = (REAL)(r->bottom - r->top) * sc;
        if (ww >= 2.0f && hh >= 2.0f) {
            gx_stroke_rect(g, 0xFF000000u, (REAL)(S * 3), x0 + 1.0f, y0 + 1.0f,
                           ww - 2.0f, hh - 2.0f);
            gx_stroke_rect(g, 0xFF7C3AEDu, (REAL)(S * 2), x0 + 1.0f, y0 + 1.0f,
                           ww - 2.0f, hh - 2.0f);
        }
    }

    /* Then the box 合并 acts on, under the picked cell's outline so the outline
     * still reads as "this one cell". */
    paint_sel_box(g, S);

    /* Then the grid line being dragged, which is the only thing on the canvas
     * that can still be changing, and above the borders so its landing spot is
     * never hidden under one. */
    if (g_sl.gesture == GESTURE_LINE) {
        paint_line_grip(g, S, g_sl.grabAxis, g_sl.dragTo, g_sl.dragLo, g_sl.dragHi,
                        1, g_sl.hoverPt.x, g_sl.hoverPt.y);
        /* The line it is leaving, dimmed rather than hidden, so both ends of the
         * distance being covered are visible. */
        paint_line_grip(g, S, g_sl.grabAxis, g_sl.grabPos, g_sl.grabLo, g_sl.grabHi,
                        0, g_sl.hoverPt.x, g_sl.hoverPt.y);
    } else if (g_sl.haveHover) {
        paint_line_grip(g, S, g_sl.hoverAxis, g_sl.hoverPos, g_sl.hoverLo, g_sl.hoverHi,
                        0, g_sl.hoverPt.x, g_sl.hoverPt.y);
    }

    /* the line currently under the pointer, before it becomes a cut */
    if (g_sl.drawing) {
        ARGB c = 0xFFFF2DAAu;
        if (g_sl.lineAxis == 0) {
            REAL x  = vx + (REAL)g_sl.linePos * sc;
            REAL y0 = vy + (REAL)g_sl.lineLo * sc;
            REAL y1 = vy + (REAL)g_sl.lineHi * sc;
            gx_stroke_line(g, 0xFF000000u, (REAL)(S * 3) + 1.0f, x, y0, x, y1);
            gx_stroke_line(g, c, (REAL)(S * 3), x, y0, x, y1);
        } else {
            REAL y  = vy + (REAL)g_sl.linePos * sc;
            REAL x0 = vx + (REAL)g_sl.lineLo * sc;
            REAL x1 = vx + (REAL)g_sl.lineHi * sc;
            gx_stroke_line(g, 0xFF000000u, (REAL)(S * 3) + 1.0f, x0, y, x1, y);
            gx_stroke_line(g, c, (REAL)(S * 3), x0, y, x1, y);
        }
    }

    /* The rubber band goes on last of all: it is what the pointer is doing
     * right now, and nothing about it should be hidden under the picture. */
    paint_band(g, S);
}

static void paint_canvas(GpGraphics *g, int S)
{
    RECT c = g_lay.canvas;
    int cw = c.right - c.left, ch = c.bottom - c.top;
    int vw = g_sl.view.right - g_sl.view.left;
    int vh = g_sl.view.bottom - g_sl.view.top;
    GpFont *f;

    gx_fill_rect(g, 0xFFDCDEE3, (REAL)c.left, (REAL)c.top, (REAL)cw, (REAL)ch);
    if (!g_sl.src.gp) {
        f = gx_font(g_sl.fam, (REAL)(S * 14));
        if (f) {
            gx_draw_text(g, f, 0xFF9AA0A6, L"打开一张 JPG 或 PNG 开始分割",
                         (REAL)c.left, (REAL)(c.top + ch / 2 - (REAL)(S * 10)),
                         (REAL)cw, (REAL)(S * 20));
            GdipDeleteFont(f);
        }
        return;
    }
    if (vw < 1 || vh < 1) return;

    /* shadow first, so it never lands on the picture's own pixels */
    gx_fill_rect(g, 0x1F000000, (REAL)(g_sl.view.left + S * 2),
                 (REAL)(g_sl.view.top + S * 3), (REAL)vw, (REAL)vh);

    /* A transparent PNG over the grey canvas would just look grey. The
     * checkerboard is what tells the user those pixels really are see-through,
     * and it is only worth building when the image has alpha to show. */
    if (g_sl.src.hasAlpha) {
        checker_sync(vw, vh, (int)(S * 8 * g_sl.scale));
        if (g_sl.chkGp)
            gx_draw_image_rect_i(g, (GpImage *)g_sl.chkGp,
                                 g_sl.view.left, g_sl.view.top, vw, vh,
                                 InterpolationModeNearestNeighbor);
    }

    gx_draw_image_rect_i(g, (GpImage *)g_sl.src.gp,
                         g_sl.view.left, g_sl.view.top, vw, vh,
                         (g_sl.scale > 1.05) ? InterpolationModeNearestNeighbor
                                             : InterpolationModeHighQualityBicubic);

    paint_tree(g, S);
    gx_stroke_rect(g, 0x40000000, 1.0f, (REAL)(g_sl.view.left - 0.5f),
                   (REAL)(g_sl.view.top - 0.5f), (REAL)(vw + 1), (REAL)(vh + 1));
}

static void paint_status(GpGraphics *g, int S)
{
    RECT s = g_lay.status;
    GpFont *f;
    wchar_t left[192];
    REAL w = 0;

    gx_fill_rect(g, 0xFFF6F6F8, (REAL)s.left, (REAL)s.top,
                 (REAL)(s.right - s.left), (REAL)(s.bottom - s.top));
    gx_stroke_line(g, 0xFFE2E2E6, 1.0f, (REAL)s.left, (REAL)s.top,
                   (REAL)s.right, (REAL)s.top);

    if (g_sl.src.gp) {
        int cells = st_count(&g_sl.tree);
        /* While a line is being dragged the status bar is the only place the
         * numbers can be: the pointer is a hundred pixels away from the line it
         * is moving, so the canvas cannot say where it is about to land, and
         * these are the two numbers the user is aiming at. */
        if (g_sl.gesture == GESTURE_LINE) {
            int d = g_sl.dragTo - g_sl.grabPos;
            _snwprintf_s(left, 192, _TRUNCATE,
                         L"%s  %d -> %d  (%+d 像素)    %d 个切片",
                         g_sl.grabAxis == 0 ? L"竖线 x" : L"横线 y",
                         g_sl.grabPos, g_sl.dragTo, d, cells);
        } else if (g_sl.sel) {
            int num = leaf_number(g_sl.sel);
            if (g_sl.selWide && g_sl.selRect.right > g_sl.selRect.left)
                _snwprintf_s(left, 192, _TRUNCATE,
                             L"%d x %d    %d 个切片    选区 %d x %d    缩放 %d%%",
                             g_sl.src.w, g_sl.src.h, cells,
                             g_sl.selRect.right - g_sl.selRect.left,
                             g_sl.selRect.bottom - g_sl.selRect.top,
                             (int)(g_sl.scale * 100.0 + 0.5));
            else
                _snwprintf_s(left, 192, _TRUNCATE,
                             L"%d x %d    %d 个切片    %s #%d  %d x %d 像素    缩放 %d%%",
                             g_sl.src.w, g_sl.src.h, cells,
                             num ? L"选中" : L"区域", num,
                             g_sl.sel->r.right - g_sl.sel->r.left,
                             g_sl.sel->r.bottom - g_sl.sel->r.top,
                             (int)(g_sl.scale * 100.0 + 0.5));
        } else {
            _snwprintf_s(left, 192, _TRUNCATE,
                         L"%d x %d    %d 个切片    缩放 %d%%",
                         g_sl.src.w, g_sl.src.h, cells,
                         (int)(g_sl.scale * 100.0 + 0.5));
        }
    } else {
        wcsncpy(left, L"未打开图片", 191);
        left[191] = 0;
    }

    f = gx_font(g_sl.fam, (REAL)(S * 11));
    if (f) {
        gx_draw_text(g, f, 0xFF6B7280, left, (REAL)(s.left + S * 12),
                     (REAL)(s.top + S * 6), (REAL)((s.right - s.left) * 0.58f),
                     (REAL)(S * 16));
        if (g_sl.msg[0] && gx_measure_text(f, g_sl.msg, &w, NULL))
            gx_draw_text(g, f, g_sl.msgIsError ? 0xFFDC2626 : 0xFF7C3AED,
                         g_sl.msg, (REAL)(s.right - w - S * 14),
                         (REAL)(s.top + S * 6), w + (REAL)(S * 4), (REAL)(S * 16));
        GdipDeleteFont(f);
    }
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc, mem;
    HBITMAP old;
    GpGraphics *g = NULL;
    RECT cr;
    int S;

    S = app_scale_px(1);
    dc = BeginPaint(h, &ps);
    if (!dc) return;
    GetClientRect(h, &cr);
    mem = CreateCompatibleDC(dc);
    if (!mem) { EndPaint(h, &ps); return; }

    if (!g_sl.memBmp || g_sl.memW != cr.right || g_sl.memH != cr.bottom) {
        if (g_sl.mem) { GdipDisposeImage((GpImage *)g_sl.mem); g_sl.mem = NULL; }
        if (g_sl.memBmp) { free_dib(g_sl.memBmp, g_sl.memBits); g_sl.memBmp = NULL; }
        g_sl.memBits = NULL;
        g_sl.memBmp = make_dib(cr.right, cr.bottom, 32, &g_sl.memBits);
        if (g_sl.memBmp)
            GdipCreateBitmapFromScan0(cr.right, cr.bottom, cr.right * 4,
                                      PixelFormat32bppARGB, g_sl.memBits, &g_sl.mem);
        g_sl.memW = cr.right;
        g_sl.memH = cr.bottom;
    }
    if (!g_sl.memBmp || !g_sl.mem) { DeleteDC(mem); EndPaint(h, &ps); return; }

    old = SelectObject(mem, g_sl.memBmp);
    if (GdipGetImageGraphicsContext((GpImage *)g_sl.mem, &g) == Ok) {
        gx_set_quality(g);
        gx_clear(g, 0xFFF6F6F8, (REAL)cr.right, (REAL)cr.bottom);
        build_layout(cr.right, cr.bottom);
        build_view();
        paint_canvas(g, S);
        paint_panel(g);
        paint_status(g, S);
        GdipDeleteGraphics(g);
    }
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    EndPaint(h, &ps);
}
/* ------------------------------------------------------------------ */
/* canvas interaction                                                  */
/* ------------------------------------------------------------------ */

static int in_view(int sx, int sy)
{
    return (g_sl.src.gp && g_sl.scale > 0 &&
            sx >= g_sl.view.left && sx <= g_sl.view.right &&
            sy >= g_sl.view.top && sy <= g_sl.view.bottom);
}

static int screen_to_img_x(int sx)
{
    return (int)(((double)(sx - g_sl.view.left) / g_sl.scale) + 0.5);
}

static int screen_to_img_y(int sy)
{
    return (int)(((double)(sy - g_sl.view.top) / g_sl.scale) + 0.5);
}

/* Which screen coordinate a vertical line is measured against in x and a
 * horizontal one in y. Spelled out rather than picked inline because the line
 * code needs it four or five times and one of them getting it backwards would
 * look exactly like a bug in the hit test. */
static int ptr_at(int sx, int sy, int axis)
{
    return (axis == 0) ? sx : sy;
}

static void update_hover(int sx, int sy)
{
    Slice *h = NULL;
    if (in_view(sx, sy))
        h = st_leaf_at_point(&g_sl.tree, screen_to_img_x(sx), screen_to_img_y(sy));
    if (h != g_sl.hover) {
        g_sl.hover = h;
        InvalidateRect(g_sl.hwnd, NULL, FALSE);
    }
}

/* Recomputes the line being dragged from the pointer. The line runs *along* the
 * drag: dragged sideways it becomes a horizontal line, dragged downwards a
 * vertical one, so a long sweep cuts a long run and a short one cuts only a
 * short run. The dominant direction picks the axis, so a diagonal drag still
 * produces one clean line, and the line sits where the cursor is rather than
 * at the drag's midpoint - the cursor is where the user is aiming. */
static void update_line(int sx, int sy)
{
    int dx = sx - g_sl.downX, dy = sy - g_sl.downY;
    int ax, ay, bx, by, span;

    if (g_sl.scale <= 0) return;
    ax = screen_to_img_x(g_sl.downX);
    ay = screen_to_img_y(g_sl.downY);
    bx = screen_to_img_x(sx);
    by = screen_to_img_y(sy);

    if (abs(dx) >= abs(dy)) {
        g_sl.lineAxis = 1;
        g_sl.linePos  = by;
        g_sl.lineLo   = (ax < bx) ? ax : bx;
        g_sl.lineHi   = (ax < bx) ? bx + 1 : ax + 1;
        span = g_sl.src.w;
    } else {
        g_sl.lineAxis = 0;
        g_sl.linePos  = bx;
        g_sl.lineLo   = (ay < by) ? ay : by;
        g_sl.lineHi   = (ay < by) ? by + 1 : ay + 1;
        span = g_sl.src.h;
    }
    /* The span runs along the line - x for a horizontal one, y for a vertical
     * one - so each is clamped against the extent it is measured in. */
    if (g_sl.lineLo < 0) g_sl.lineLo = 0;
    if (g_sl.lineHi > span) g_sl.lineHi = span;
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* The slice border closest to a screen point, if one is within `reach` pixels.
 * Pure - it is handed the cell, the view rect, the scale and the picture size
 * rather than reading them - so the self-test can pin the arithmetic down
 * without a window, which is the only way this can be checked at all: a miss
 * here does not throw or misbehave visibly, it silently falls back to drawing a
 * cut, and the user sees a drag that does the wrong thing.
 *
 * axis 0 is a vertical line and its position is an x, axis 1 a horizontal one
 * and its position is a y, matching the model everywhere else. */
static int nearest_edge(const RECT *cell, const RECT *view, double scale,
                        int imgW, int imgH, int sx, int sy, int reach,
                        int *outAxis, int *outPos, int *outGrab)
{
    /* `found` and `axis` are deliberately separate: axis 0 is a legal answer
     * for a vertical line, so a single variable cannot carry both "did we hit
     * anything" and "which axis it was". */
    int best = reach + 1, k, found = 0, axis = 0, pos = 0, off = 0;

    for (k = 0; k < 4; k++) {
        int vert = (k < 2);
        int base = vert ? view->left : view->top;
        int size = vert ? imgW : imgH;
        int edge, at, d;

        edge = vert ? ((k & 1) ? cell->right : cell->left)
                    : ((k & 1) ? cell->bottom : cell->top);
        /* The picture's own border has no cell on the far side of it to trade
         * pixels with, so it is not a line anyone can move. */
        if (edge <= 0 || edge >= size) continue;
        at = (int)((double)base + (double)edge * scale + 0.5);
        d = (vert ? sx : sy) - at;
        if (d < 0) d = -d;
        /* `>=` on the distance keeps the earlier border, which is what makes a
         * corner resolve to the vertical line every time instead of flickering.
         * It does not weaken the reach: best starts at reach+1, so a press
         * exactly on the limit still counts as a hit. */
        if (d > reach || d >= best) continue;
        best  = d;
        found = 1;
        axis  = vert ? 0 : 1;
        pos   = edge;
        off   = (vert ? sx : sy) - at;
    }
    if (!found) return 0;
    if (outAxis) *outAxis = axis;
    if (outPos)  *outPos  = pos;
    if (outGrab) *outGrab = off;
    return 1;
}

/* Is the pointer close enough to one of the cell's own borders to grab it?
 *
 * Only the cell under the pointer is looked at, which keeps this cheap enough
 * to run from WM_SETCURSOR: walking down to the deepest cell is a few
 * comparisons, not a scan of the tree. [outLo, outHi] come back as that cell's
 * extent across the line, which seeds the run - st_line_target grows it to the
 * whole grid line from there. Returns 0 when there is nothing to grab. */
static int hit_line(int sx, int sy, int *outAxis, int *outPos,
                    int *outLo, int *outHi, int *outGrab)
{
    Slice *s;
    int reach = app_scale_px(SL_GRAB_PX);
    int axis = 0;

    if (!in_view(sx, sy)) return 0;
    /* Alt is the way out: it says "I meant to cut here" whatever the pointer is
     * near, which is what makes widening the reach below safe. Without it, a
     * cell small enough for its borders to crowd the middle would have no
     * comfortable place left to start a cut. */
    if (GetKeyState(VK_MENU) & 0x8000) return 0;
    s = st_leaf_at_point(&g_sl.tree, screen_to_img_x(sx), screen_to_img_y(sy));
    if (!s) return 0;

    /* A cell narrower than a few reach widths has no room between its borders,
     * so the reach grows to stay grabbable - but only up to double, because
     * past that the borders would start eating the middle of the cell and
     * "drag here to cut" would stop working on a fine grid. */
    {
        double cw = (double)(s->r.right - s->r.left) * g_sl.scale;
        double ch = (double)(s->r.bottom - s->r.top) * g_sl.scale;
        int small = (int)((cw < ch ? cw : ch) / 3);
        if (small > reach) reach = (small > reach * 2) ? reach * 2 : small;
    }

    /* Vertical borders first, and left before right, so a pointer sitting on a
     * corner always resolves to the same line. nearest_edge keeps that order,
     * and its `>` on the tie is deliberate. */
    if (!nearest_edge(&s->r, &g_sl.view, g_sl.scale, g_sl.src.w, g_sl.src.h,
                      sx, sy, reach, &axis, outPos, outGrab))
        return 0;
    if (outAxis) *outAxis = axis;
    if (outLo) *outLo = (axis == 0) ? s->r.top  : s->r.left;
    if (outHi) *outHi = (axis == 0) ? s->r.bottom : s->r.right;
    return 1;
}

/* Records which grid line the pointer is over, so the canvas can show the grip
 * and the window can offer a resize cursor before anything is pressed. The
 * grown run is worked out once here rather than per paint: it is the same
 * search st_move_line will do, and doing it on every WM_MOUSEMOVE would mean
 * re-walking the tree for a line that has not moved. */
static void update_line_hover(int sx, int sy)
{
    int axis = 0, pos = 0, lo = 0, hi = 0, grab = 0, spanLo = 0, spanHi = 0;
    /* The handle follows the pointer, so a move along the same line has to
     * repaint even though the line itself did not change. */
    int moved = (g_sl.hoverPt.x != sx || g_sl.hoverPt.y != sy);

    g_sl.hoverPt.x = sx;
    g_sl.hoverPt.y = sy;

    if (!hit_line(sx, sy, &axis, &pos, &lo, &hi, &grab)) {
        if (g_sl.haveHover) {
            g_sl.haveHover = 0;
            InvalidateRect(g_sl.hwnd, NULL, FALSE);
        }
        return;
    }
    st_line_target(&g_sl.tree, axis, pos, pos, lo, hi, &pos, &spanLo, &spanHi);
    if (g_sl.haveHover && !moved && g_sl.hoverAxis == axis && g_sl.hoverPos == pos &&
        g_sl.hoverLo == spanLo && g_sl.hoverHi == spanHi) return;
    g_sl.haveHover = 1;
    g_sl.hoverAxis = axis;
    g_sl.hoverPos  = pos;
    g_sl.hoverLo   = spanLo;
    g_sl.hoverHi   = spanHi;
    g_sl.hoverGrab = grab;
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Where the dragged line would land, clamped by the model rather than by the
 * window - the same call st_move_line makes on the way up, so the preview and
 * the result cannot disagree. */
static void update_line_drag(int sx, int sy)
{
    int base = (g_sl.grabAxis == 0) ? g_sl.view.left : g_sl.view.top;
    int ptr  = (g_sl.grabAxis == 0) ? sx : sy;
    int want;

    /* The handle tracks the pointer through the drag as well, so the line and
     * the thing being grabbed stay visibly attached to each other. */
    g_sl.hoverPt.x = sx;
    g_sl.hoverPt.y = sy;

    if (g_sl.scale <= 0) return;
    want = (int)((double)(ptr - g_sl.grabOff - base) / g_sl.scale);
    st_line_target(&g_sl.tree, g_sl.grabAxis, g_sl.grabPos, want,
                   g_sl.grabLo, g_sl.grabHi,
                   &g_sl.dragTo, &g_sl.dragLo, &g_sl.dragHi);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Both a moved line and a merged block rebuild the tree, and the rebuild frees
 * every Slice the window was holding. The selection is re-found by position
 * rather than cleared: a user who drags a line across the cell they had picked
 * expects that cell to still be picked, and one who merges a box expects the
 * merged cell to be picked afterwards. */
static void reanchor_selection(void)
{
    POINT c;
    if (g_sl.sel) {
        if (g_sl.selRect.right > g_sl.selRect.left &&
            g_sl.selRect.bottom > g_sl.selRect.top) {
            c.x = (g_sl.selRect.left + g_sl.selRect.right) / 2;
            c.y = (g_sl.selRect.top + g_sl.selRect.bottom) / 2;
        } else {
            c.x = (g_sl.sel->r.left + g_sl.sel->r.right) / 2;
            c.y = (g_sl.sel->r.top + g_sl.sel->r.bottom) / 2;
        }
        g_sl.sel = st_leaf_at_point(&g_sl.tree, c.x, c.y);
    }
    if (g_sl.hover) {
        c.x = (g_sl.hover->r.left + g_sl.hover->r.right) / 2;
        c.y = (g_sl.hover->r.top + g_sl.hover->r.bottom) / 2;
        g_sl.hover = st_leaf_at_point(&g_sl.tree, c.x, c.y);
    }
}

/* One nudge of the last line the pointer touched, from the arrow keys. The
 * target is clamped first, so the caller can report where the line actually
 * ended up rather than where it was asked to go. */
static int nudge_last_line(int step)
{
    int to = g_sl.lastPos, moved;
    if (!g_sl.haveLast || !g_sl.tree.root) return 0;
    st_line_target(&g_sl.tree, g_sl.lastAxis, g_sl.lastPos, g_sl.lastPos + step,
                   g_sl.lastLo, g_sl.lastHi, &to, &g_sl.lastLo, &g_sl.lastHi);
    if (to == g_sl.lastPos) return 0;
    moved = st_move_line(&g_sl.tree, g_sl.lastAxis, g_sl.lastPos, to,
                         g_sl.lastLo, g_sl.lastHi);
    if (!moved) return 0;
    g_sl.lastPos = to;
    reanchor_selection();
    return moved;
}

/* ------------------------------------------------------------------ */
/* the rubber band                                                     */
/* ------------------------------------------------------------------ */

/* Grows an image-space box until it exactly covers whole cells: the bounding
 * box of every cell the band touches.
 *
 * This is what makes dragging a band worth having at all. A band pulled by hand
 * never lands on a cell edge, and 合并 has to refuse a box that clips a cell -
 * so without snapping, the gesture the user has just performed would produce a
 * selection that cannot be merged, which is the worst possible outcome for a
 * control whose whole job is to make merging easy. */
static int snap_band_to_cells(RECT *out)
{
    Slice **lv;
    int n = 0, i, got = 0;
    RECT box;

    lv = st_leaves(&g_sl.tree, &n);
    if (!lv) return 0;
    for (i = 0; i < n; i++) {
        const RECT *r = &lv[i]->r;
        if (r->left >= out->right || out->left >= r->right ||
            r->top >= out->bottom || out->top >= r->bottom) continue;
        if (!got) { box = *r; got = 1; continue; }
        if (r->left   < box.left)   box.left   = r->left;
        if (r->top    < box.top)    box.top    = r->top;
        if (r->right  > box.right)  box.right  = r->right;
        if (r->bottom > box.bottom) box.bottom = r->bottom;
    }
    xfree(lv);
    if (!got) return 0;
    *out = box;
    return 1;
}

static void band_begin(int sx, int sy)
{
    g_sl.bandDown = 1;
    g_sl.bandFrom.x = sx;
    g_sl.bandFrom.y = sy;
    g_sl.bandTo.x = sx;
    g_sl.bandTo.y = sy;
    SetCapture(g_sl.hwnd);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Turns the band into the selection. A band that never moved is a plain click
 * on one cell, which is why this can stand in for one. */
static void band_end(int sx, int sy)
{
    RECT box;
    int lx0, ly0, lx1, ly1, cells = 0, aligned = 0;
    wchar_t m[160];

    g_sl.bandDown = 0;
    if (GetCapture() == g_sl.hwnd) ReleaseCapture();
    if (g_sl.scale <= 0) return;

    lx0 = screen_to_img_x((sx < g_sl.bandFrom.x ? sx : g_sl.bandFrom.x));
    lx1 = screen_to_img_x((sx > g_sl.bandFrom.x ? sx : g_sl.bandFrom.x) + 1);
    ly0 = screen_to_img_y((sy < g_sl.bandFrom.y ? sy : g_sl.bandFrom.y));
    ly1 = screen_to_img_y((sy > g_sl.bandFrom.y ? sy : g_sl.bandFrom.y) + 1);
    /* A click has both ends on one pixel, so the box is given a pixel of its
     * own - otherwise nothing overlaps it and the snap below finds no cell. */
    if (lx1 <= lx0) lx1 = lx0 + 1;
    if (ly1 <= ly0) ly1 = ly0 + 1;
    SetRect(&box, lx0, ly0, lx1, ly1);

    if (!snap_band_to_cells(&box)) {
        set_msg(L"框选范围内没有格子。", 0);
        InvalidateRect(g_sl.hwnd, NULL, FALSE);
        return;
    }
    g_sl.selRect = box;
    g_sl.selFrom = box;
    cells = st_rect_cells(&g_sl.tree, box, &aligned);
    g_sl.selWide = (cells > 1);
    g_sl.sel = st_leaf_at_point(&g_sl.tree, (box.left + box.right) / 2,
                                           (box.top + box.bottom) / 2);
    g_sl.hover = g_sl.sel;
    if (cells > 1)
        _snwprintf_s(m, 160, _TRUNCATE,
                     L"已框选 %d 个格子，按 M 或点「合并」并成一格。", cells);
    else {
        wcsncpy(m, L"已选中 1 个格子。", 159);
        m[159] = 0;
    }
    set_msg(m, 0);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

/* Both of the operations that lay the tree out afresh leave the remembered grid
 * line pointing at a position that means nothing any more, so it is dropped
 * along with the selection. Forgetting is the safe direction - the model also
 * refuses a position no edge sits on, so a stale one could not do damage - and
 * it means the next move of the pointer simply finds a real line again. */
static void forget_lines(void)
{
    g_sl.haveHover = 0;
    g_sl.haveLast  = 0;
    g_sl.gesture   = GESTURE_NONE;
    g_sl.drawing   = 0;
    g_sl.bandDown  = 0;
}
/* Both file pickers below are shell COM objects, and the thread has to be in an
 * apartment before they will create at all. Without this they do not fail
 * loudly - GetOpenFileNameW just comes straight back with nothing on screen and
 * no error, which is exactly what the Open button used to do. */
static int com_ready(void)
{
    /* 0 untried, 1 in an apartment, -1 unavailable. Only the first call on this
     * thread does any work; CoUninitialize is deliberately never called,
     * because the thread outlives every dialog and a dialog opened after a
     * matching uninitialise would fail. */
    static volatile LONG state = 0;
    LONG seen = InterlockedCompareExchange(&state, 1, 0);
    HRESULT hr;

    if (seen == 1) return 1;
    if (seen == -1) return 0;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    /* S_FALSE: already initialised here, which is fine. RPC_E_CHANGED_MODE: the
     * thread was made a free-threaded apartment by something else - the dialogs
     * still work in that, so it is a note rather than a refusal. */
    if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) return 1;
    InterlockedExchange(&state, -1);
    return 0;
}

/* Both pickers end the same way: take the chosen item's real file-system path
 * and copy it out. IShellItem hands the path back in COM memory, which has to
 * be released with CoTaskMemFree rather than free - leaking it is the classic
 * way a file dialog ends up owning a little heap for the life of the process. */
static int shell_item_path(IShellItem *item, wchar_t *out, size_t cch)
{
    PWSTR got = NULL;
    int ok = 0;

    if (!item || !out || cch == 0) return 0;
    if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &got)) && got) {
        wcsncpy(out, got, cch - 1);
        out[cch - 1] = 0;
        ok = 1;
    }
    if (got) CoTaskMemFree(got);
    return ok;
}

static int pick_image(HWND owner, wchar_t *out, size_t cch)
{
    static const COMDLG_FILTERSPEC kinds[] = {
        { L"图片文件",  L"*.png;*.jpg;*.jpeg" },
        { L"PNG 图片",  L"*.png" },
        { L"JPEG 图片", L"*.jpg;*.jpeg" }
    };
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    HRESULT hr;
    int ok = 0;

    if (!com_ready()) return 0;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) goto done;
    dlg->lpVtbl->SetFileTypes(dlg, (UINT)(sizeof(kinds) / sizeof(kinds[0])), kinds);
    dlg->lpVtbl->SetFileTypeIndex(dlg, 1);
    dlg->lpVtbl->SetTitle(dlg, L"打开图片");
    /* FOS_NOCHANGEDIR on purpose: the dialog must not move the process's
     * working directory, which would make a later relative --out resolve
     * somewhere the user never asked for. */
    dlg->lpVtbl->SetOptions(dlg, FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                               FOS_NOCHANGEDIR);
    hr = dlg->lpVtbl->Show(dlg, owner);
    /* Cancel comes back as S_OK plus an error code on older shells and as
     * HRESULT_FROM_WIN32(ERROR_CANCELLED) on newer ones; both are "no file",
     * not a failure worth reporting. */
    if (hr != S_OK) goto done;
    if (FAILED(dlg->lpVtbl->GetResult(dlg, &item))) goto done;
    ok = shell_item_path(item, out, cch);
done:
    if (item) item->lpVtbl->Release(item);
    if (dlg) dlg->lpVtbl->Release(dlg);
    return ok;
}

static int pick_folder(HWND owner, wchar_t *out, size_t cch)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL;
    HRESULT hr;
    int ok = 0;

    if (!com_ready()) return 0;
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) goto done;
    dlg->lpVtbl->SetTitle(dlg, L"选择切片输出文件夹");
    /* FORCEFILESYSTEM because the output path goes straight to a directory
     * create, and a virtual folder (a library, a search result) has no path on
     * disk to create. */
    dlg->lpVtbl->SetOptions(dlg, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    hr = dlg->lpVtbl->Show(dlg, owner);
    if (hr != S_OK) goto done;
    if (FAILED(dlg->lpVtbl->GetResult(dlg, &item))) goto done;
    ok = shell_item_path(item, out, cch);
    if (ok) wcs_trim(out);
done:
    if (item) item->lpVtbl->Release(item);
    if (dlg) dlg->lpVtbl->Release(dlg);
    return ok;
}

static void sync_options(void)
{
    g_sl.rowsN = read_int(g_sl.ctl[IDC_ROWS], g_sl.rowsN);
    g_sl.colsN = read_int(g_sl.ctl[IDC_COLS], g_sl.colsN);
    g_sl.cellPx = read_int(g_sl.ctl[IDC_CELL], 256);
    g_sl.divN  = read_int(g_sl.ctl[IDC_DIVN], 2);
    g_sl.quality = read_int(g_sl.ctl[IDC_QUALITY], 90);
    if (g_sl.rowsN < 1) g_sl.rowsN = 1;
    if (g_sl.colsN < 1) g_sl.colsN = 1;
    if (g_sl.cellPx < 1) g_sl.cellPx = 1;
    if (g_sl.divN  < 2) g_sl.divN  = 2;
    if (g_sl.quality < 1) g_sl.quality = 1;
    if (g_sl.quality > 100) g_sl.quality = 100;
    g_sl.asJpeg = (SendMessageW(g_sl.ctl[IDC_FORMAT], CB_GETCURSEL, 0, 0) == 1);
    if (g_sl.ctl[IDC_OUTDIR]) {
        GetWindowTextW(g_sl.ctl[IDC_OUTDIR], g_sl.outDir, MAX_PATH);
        g_sl.outDir[MAX_PATH - 1] = 0;
        wcs_trim(g_sl.outDir);
    }
    if (g_sl.ctl[IDC_PREFIX]) {
        GetWindowTextW(g_sl.ctl[IDC_PREFIX], g_sl.prefix, 128);
        g_sl.prefix[127] = 0;
    }
}

/* Drops the selection outright. Used by every operation that replaces the tree,
 * since a remembered box or a remembered line means nothing against a layout
 * that no longer exists. */
static void clear_selection(void)
{
    g_sl.sel = NULL;
    g_sl.hover = NULL;
    g_sl.selWide = 0;
    SetRect(&g_sl.selRect, 0, 0, 0, 0);
    SetRect(&g_sl.selFrom, 0, 0, 0, 0);
    forget_lines();
}

static int load_into_app(const wchar_t *path)
{
    SourceImage s;
    wchar_t msg[192];

    if (!img_is_supported(path)) {
        set_msg(L"只支持 JPG 和 PNG 图片。", 1);
        return 0;
    }
    if (!img_load(&s, path)) {
        set_msg(L"这张图片打不开，可能已经损坏。", 1);
        return 0;
    }

    img_free(&g_sl.src);
    g_sl.src = s;
    /* A subdivision of the previous picture says nothing about this one, so
     * the tree restarts and the row/column counts are re-applied to it. */
    st_set_size(&g_sl.tree, s.w, s.h);
    clear_selection();

    /* Only defaulted once: retyping the folder for one photo must survive
     * opening the next one. */
    if (!g_sl.outDir[0]) {
        wchar_t *d = img_default_dir(path);
        wcsncpy(g_sl.outDir, d, MAX_PATH - 1);
        g_sl.outDir[MAX_PATH - 1] = 0;
        xfree(d);
    }
    if (!g_sl.prefix[0]) {
        wchar_t *stem = img_stem(path);
        wcsncpy(g_sl.prefix, stem, 127);
        g_sl.prefix[127] = 0;
        xfree(stem);
    }
    SetWindowTextW(g_sl.ctl[IDC_OUTDIR], g_sl.outDir);
    SetWindowTextW(g_sl.ctl[IDC_PREFIX], g_sl.prefix);

    if (g_sl.rowsN < 1) g_sl.rowsN = 1;
    if (g_sl.colsN < 1) g_sl.colsN = 1;
    write_int(g_sl.ctl[IDC_ROWS], g_sl.rowsN);
    write_int(g_sl.ctl[IDC_COLS], g_sl.colsN);
    st_equal_grid(&g_sl.tree, g_sl.colsN, g_sl.rowsN);

    _snwprintf_s(msg, 192, _TRUNCATE, L"已打开 %s  (%d x %d)", s.path, s.w, s.h);
    set_msg(msg, 0);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
    return 1;
}

static void do_open(void)
{
    wchar_t path[MAX_PATH];
    if (!pick_image(g_sl.hwnd, path, MAX_PATH)) return;
    load_into_app(path);
}

static void do_layout(int mode)
{
    wchar_t m[160];
    int n;
    sync_options();
    clear_selection();
    switch (mode) {
    case 0:
        st_equal_rows(&g_sl.tree, g_sl.rowsN);
        set_msg(L"已按行均分。", 0);
        break;
    case 1:
        st_equal_cols(&g_sl.tree, g_sl.colsN);
        set_msg(L"已按列均分。", 0);
        break;
    case 2:
        n = st_fixed(&g_sl.tree, g_sl.cellPx);
        _snwprintf_s(m, 160, _TRUNCATE, L"已按 %d 像素切分，共 %d 个。", g_sl.cellPx, n);
        set_msg(m, 0);
        break;
    default:
        st_reset(&g_sl.tree);
        set_msg(L"已重置为 1 个切片。", 0);
        break;
    }
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Subdivide the picked region into divN x divN, then step the selection into
 * its first sub-region. That step is what makes "keep subdividing" work: the
 * next click goes one level deeper instead of re-cutting the same region to
 * the same depth, which would land on the same result every time. With nothing
 * picked it applies to every slice instead. */
static void do_divide(int all)
{
    wchar_t m[192];
    int n;
    sync_options();
    if (!g_sl.tree.root) return;

    if (all || !g_sl.sel) {
        n = st_divide_all(&g_sl.tree, g_sl.divN);
        _snwprintf_s(m, 192, _TRUNCATE, L"已把所有切片细分为 %d x %d，共 %d 个。",
                     g_sl.divN, g_sl.divN, n);
        g_sl.sel = NULL;
    } else {
        st_divide_both(g_sl.sel, g_sl.divN);
        g_sl.sel = st_first_child(g_sl.sel);
        n = st_count(&g_sl.tree);
        _snwprintf_s(m, 192, _TRUNCATE,
                     L"已细分为 %d x %d，已选中其中一格，可继续点细分。当前共 %d 个。",
                     g_sl.divN, g_sl.divN, n);
    }
    g_sl.hover = g_sl.sel;
    if (n > SLICE_MAX_CELLS) {
        _snwprintf_s(m, 192, _TRUNCATE, L"切片已超过 %d 个，继续细分会拖慢导出。",
                     SLICE_MAX_CELLS);
        set_msg(m, 1);
    } else {
        set_msg(m, 0);
    }
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* One axis only: axis 1 cuts the region into horizontal strips, 0 into
 * vertical ones - that is, a horizontal *line* separates rows. This is the
 * button to reach for when a region needs splitting one way and not the
 * other. */
static void do_divide_axis(int axis)
{
    wchar_t m[192];
    Slice *target = g_sl.sel;
    Slice **lv;
    int n, i, count = 0;
    sync_options();
    if (!g_sl.tree.root) return;

    if (target) {
        st_divide(target, axis, g_sl.divN);
        /* step in by one strip, so repeated clicks narrow down the same region
         * instead of replacing the division it already has */
        g_sl.sel = st_first_child(target);
    } else {
        /* nothing picked: do the same to every slice rather than quietly doing
         * nothing and leaving the button looking broken */
        lv = st_leaves(&g_sl.tree, &count);
        if (lv) {
            for (i = 0; i < count; i++) st_divide(lv[i], axis, g_sl.divN);
            xfree(lv);
        }
    }
    n = st_count(&g_sl.tree);
    _snwprintf_s(m, 192, _TRUNCATE, L"已%s分为 %d 份，当前共 %d 个切片。",
                 axis == 1 ? L"横" : L"竖", g_sl.divN, n);
    g_sl.hover = g_sl.sel;
    if (n > SLICE_MAX_CELLS)
        set_msg(L"切片数量过多，建议重置后再细分。", 1);
    else
        set_msg(m, 0);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

static void do_delete_sel(void)
{
    wchar_t m[160];
    if (!g_sl.sel) { set_msg(L"先在图上点一个格子。", 1); return; }
    if (!st_delete(&g_sl.tree, g_sl.sel)) {
        set_msg(L"顶层切片不能单独删除，它的像素没有去处；用「重置」回到整张图。", 1);
        return;
    }
    clear_selection();
    _snwprintf_s(m, 160, _TRUNCATE, L"已删除该格，现在共 %d 个切片。", st_count(&g_sl.tree));
    set_msg(m, 0);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

static void apply_draw(void)
{
    int cut;
    wchar_t m[160];
    cut = st_cut(&g_sl.tree, g_sl.lineAxis, g_sl.linePos, g_sl.lineLo, g_sl.lineHi);
    clear_selection();
    if (cut > 0) {
        _snwprintf_s(m, 160, _TRUNCATE, L"分割线切开 %d 个切片，现在共 %d 个。",
                     cut, st_count(&g_sl.tree));
        set_msg(m, 0);
    } else {
        /* the line landed on an existing border, or outside every region */
        set_msg(L"这条线没有切到任何格子。", 0);
    }
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Applies the grid line the pointer was dragging. */
static void apply_line_move(void)
{
    int n;
    wchar_t m[192];

    if (!g_sl.tree.root) return;
    n = st_move_line(&g_sl.tree, g_sl.grabAxis, g_sl.grabPos, g_sl.dragTo,
                     g_sl.grabLo, g_sl.grabHi);
    if (n > 0) {
        /* The remembered line follows the one that moved, so the arrow keys
         * carry on from where the drag left it. */
        g_sl.lastAxis = g_sl.grabAxis;
        g_sl.lastPos  = g_sl.dragTo;
        g_sl.lastLo   = g_sl.dragLo;
        g_sl.lastHi   = g_sl.dragHi;
        g_sl.haveLast = 1;
        reanchor_selection();
        _snwprintf_s(m, 192, _TRUNCATE,
                     L"网格线移到 %d，%d 个切片跟着变了，现在共 %d 个。",
                     g_sl.dragTo, n, st_count(&g_sl.tree));
        set_msg(m, 0);
    } else {
        set_msg(L"这条网格线移不动了。", 1);
    }
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Folds the picked box back into one cell.
 *
 * The refusal is worded rather than silent because the only thing that makes a
 * merge illegal is a box that clips a cell. A box from the rubber band or from
 * Shift-clicking two cells can never do that, so a failure here means the
 * picture changed underneath a selection made earlier - and saying which case it
 * was saves the user hunting for a button that is not broken. */
static void do_merge(void)
{
    RECT r = g_sl.selRect;
    int cells = 0, aligned = 0, n;
    wchar_t m[192];

    if (!g_sl.tree.root) return;
    if (r.right <= r.left || r.bottom <= r.top) {
        set_msg(L"先在图上点一个格子；按住右键拖出一个框，可以一次选中多格。", 1);
        return;
    }
    cells = st_rect_cells(&g_sl.tree, r, &aligned);
    if (!aligned) {
        set_msg(L"选区切到了格子的一半，合并不了；请沿着格子边缘框选。", 1);
        return;
    }
    if (cells < 2) {
        set_msg(L"选区里只有 1 个格子，本来就是完整的。", 0);
        return;
    }
    n = st_merge_rect(&g_sl.tree, r);
    if (n < 2) {
        set_msg(L"合并没有生效，选区没有对齐切片边缘。", 1);
        return;
    }
    /* The merged block becomes the selection, so 细分 can immediately cut it
     * up again and the user can keep working on the same area. */
    g_sl.sel = st_leaf_at_point(&g_sl.tree, (r.left + r.right) / 2,
                                           (r.top + r.bottom) / 2);
    g_sl.hover = g_sl.sel;
    g_sl.selWide = 0;
    if (g_sl.sel) { g_sl.selFrom = g_sl.sel->r; g_sl.selRect = g_sl.sel->r; }
    _snwprintf_s(m, 192, _TRUNCATE, L"已把 %d 个格子合并成 1 个，现在共 %d 个切片。",
                 n, st_count(&g_sl.tree));
    set_msg(m, 0);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Ctrl+A: the whole picture becomes the selection. Combined with 合并 that is
 * the same as 重置, and it is here so the box always has an obvious way to grow
 * to everything. */
static void do_select_all(void)
{
    if (!g_sl.tree.root || !g_sl.src.gp) return;
    SetRect(&g_sl.selRect, 0, 0, g_sl.src.w, g_sl.src.h);
    SetRect(&g_sl.selFrom, 0, 0, g_sl.src.w, g_sl.src.h);
    g_sl.selWide = 1;
    g_sl.sel = st_leaf_at_point(&g_sl.tree, g_sl.src.w / 2, g_sl.src.h / 2);
    g_sl.hover = g_sl.sel;
    set_msg(L"已选中整张图片，按 M 或点「合并」可回到 1 个切片。", 0);
    InvalidateRect(g_sl.hwnd, NULL, FALSE);
}

/* Writes every slice to disk. Returns the number of files written, or -1 if
 * the export could not be started at all (nothing written, message shown). */
static int export_slices(HWND owner, int quiet)
{
    Slice **lv;
    int total, i, wrote = 0;
    wchar_t path[MAX_PATH + 80];
    HCURSOR old;

    if (!g_sl.src.gp) {
        if (!quiet) set_msg(L"请先打开一张图片。", 1);
        return -1;
    }
    total = st_count(&g_sl.tree);
    if (total < 1) {
        if (!quiet) set_msg(L"没有可导出的切片。", 1);
        return -1;
    }
    if (total > SLICE_MAX_CELLS) {
        if (!quiet) {
            wchar_t m[160];
            _snwprintf_s(m, 160, _TRUNCATE,
                         L"切片数量 %d 超过上限 %d，请重置或减少细分。",
                         total, SLICE_MAX_CELLS);
            set_msg(m, 1);
        }
        return -1;
    }
    if (!g_sl.outDir[0]) {
        if (!quiet) set_msg(L"请先选择输出目录。", 1);
        return -1;
    }
    if (!img_mkdir_p(g_sl.outDir)) {
        if (!quiet) set_msg(L"输出文件夹创建失败，换一个目录试试。", 1);
        return -1;
    }

    lv = st_leaves(&g_sl.tree, &total);
    if (!lv) return -1;

    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    for (i = 0; i < total; i++) {
        wchar_t *name = st_slice_name(g_sl.prefix, i, total, g_sl.asJpeg);
        dir_join(path, MAX_PATH + 80, g_sl.outDir, name);
        xfree(name);
        if (img_save_rect(&g_sl.src, lv[i]->r, path, g_sl.asJpeg, g_sl.quality))
            wrote++;
    }
    SetCursor(old);
    xfree(lv);

    if (wrote != total) {
        wchar_t m[160];
        _snwprintf_s(m, 160, _TRUNCATE,
                     L"只写出 %d / %d 个文件，目录可能没有写入权限。", wrote, total);
        if (!quiet) set_msg(m, 1);
    }
    if (!quiet && wrote == total) {
        wchar_t m[192];
        _snwprintf_s(m, 192, _TRUNCATE, L"已导出 %d 个切片到 %s", wrote, g_sl.outDir);
        set_msg(m, 0);
        if (owner && MessageBoxW(owner, m, L"导出完成",
                                 MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(NULL, L"open", g_sl.outDir, NULL, NULL, SW_SHOWNORMAL);
    }
    return wrote;
}

static void do_export(void)
{
    sync_options();
    export_slices(g_sl.hwnd, 0);
}
/* ------------------------------------------------------------------ */
/* self-test                                                           */
/* ------------------------------------------------------------------ */

static int check(int cond, const wchar_t *what)
{
    wchar_t line[192];
    _snwprintf_s(line, 192, _TRUNCATE, L"  [%s] %s",
                 cond ? L"pass" : L"FAIL", what);
    app_logw(L"%s", line);
    return cond ? 0 : 1;
}

/* Same, but prints what the value actually was. A count assertion that only
 * says FAIL leaves the reader guessing between "off by one" and "completely
 * wrong", and those need different bugs. */
static int check_i(int got, int want, const wchar_t *what)
{
    if (got != want) {
        wchar_t d[256];
        _snwprintf_s(d, 256, _TRUNCATE, L"        got %d, wanted %d", got, want);
        app_logw(L"%s", d);
    }
    return check(got == want, what);
}

/* One pixel, by colour. `tol` is how far a channel may drift and still count:
 * zero for PNG, which stores the bytes exactly, and something generous for
 * JPEG, whose lossy blocks pull hard on a hard colour edge. Asking for "close
 * enough" instead of "exact" is the point - a JPEG round trip that moved a
 * channel by three counts as working, and one that moved it by eighty does not
 * hide behind the same assertion. */
static int px_is(const SourceImage *s, int x, int y,
                 int r, int g, int b, int tol)
{
    const unsigned char *p;
    int dr, dg, db, d;

    if (!s || !s->bits) return 0;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    p = (const unsigned char *)s->bits + (size_t)y * (size_t)s->stride
      + (size_t)x * 4;
    /* A fully transparent pixel still carries whatever colour was written into
     * the buffer, so it would sail through a colour check. Anything that has
     * been blended away cannot be called the colour we asked for. */
    if (p[3] < 250) return 0;
    dr = p[2] - r; if (dr < 0) dr = -dr;
    dg = p[1] - g; if (dg < 0) dg = -dg;
    db = p[0] - b; if (db < 0) db = -db;
    d = tol < 0 ? 0 : tol;
    return dr <= d && dg <= d && db <= d;
}

/* The one invariant every other guarantee rests on: the leaves tile the
 * picture exactly. Any operation that dropped a pixel, overlapped two slices
 * or left a hole shows up here, which is why the churn loop at the end checks
 * it after every single step. */
static int tree_problems(const SliceTree *t)
{
    Slice **lv;
    int n = 0, i, j, bad = 0;
    long long sum = 0;

    lv = st_leaves(t, &n);
    if (!lv) return 1;
    for (i = 0; i < n; i++) {
        const RECT *r = &lv[i]->r;
        if (r->right <= r->left || r->bottom <= r->top) bad++;
        if (r->left < 0 || r->top < 0 ||
            r->right > t->imgW || r->bottom > t->imgH) bad++;
        sum += (long long)(r->right - r->left) * (r->bottom - r->top);
    }
    if (sum != (long long)t->imgW * t->imgH) bad++;
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            const RECT *a = &lv[i]->r, *b = &lv[j]->r;
            if (a->left < b->right && b->left < a->right &&
                a->top < b->bottom && b->top < a->bottom) bad++;
        }
    }
    xfree(lv);
    return bad;
}

/* A four-quadrant test image - red / green / blue / white - so a crop that
 * lands on the wrong region or slips by a row shows up as a wrong colour
 * rather than as a plausible file of the right size. */
static int make_test_png(const wchar_t *path, int w, int h)
{
    void *bits = NULL;
    HBITMAP dib = make_dib(w, h, 32, &bits);
    int x, y, ok;
    if (!dib) return 0;
    for (y = 0; y < h; y++) {
        BGRAPX *row = (BGRAPX *)((BYTE *)bits + (size_t)y * w * 4);
        for (x = 0; x < w; x++) {
            BGRAPX *p = &row[x];
            p->a = 255;
            if (y < h / 2) {
                if (x < w / 2) { p->r = 255; p->g = 0;   p->b = 0;   }
                else           { p->r = 0;   p->g = 255; p->b = 0;   }
            } else {
                if (x < w / 2) { p->r = 0;   p->g = 0;   p->b = 255; }
                else           { p->r = 255; p->g = 255; p->b = 255; }
            }
        }
    }
    ok = gdiplus_save_buffer(bits, w, h, w * 4, path, L"image/png", 100);
    free_dib(dib, bits);
    return ok;
}

/* The same picture with its left half fully transparent. This is what drives
 * the checkerboard on the canvas and the "PNG keeps transparency" promise -
 * the path that silently degrades to black if anything flattens the alpha. */
static int make_test_png_alpha(const wchar_t *path, int w, int h)
{
    void *bits = NULL;
    HBITMAP dib = make_dib(w, h, 32, &bits);
    int x, y, ok;
    if (!dib) return 0;
    for (y = 0; y < h; y++) {
        BGRAPX *row = (BGRAPX *)((BYTE *)bits + (size_t)y * w * 4);
        for (x = 0; x < w; x++) {
            BGRAPX *p = &row[x];
            p->r = 0; p->g = 255; p->b = 0;
            p->a = (x < w / 2) ? 0 : 255;
        }
    }
    ok = gdiplus_save_buffer(bits, w, h, w * 4, path, L"image/png", 100);
    free_dib(dib, bits);
    return ok;
}

/* A plain LCG. A macro would read and write the seed in the same expression,
 * which is an unsequenced modification - the churn loop would then be testing
 * a different sequence on every compiler and could pass for the wrong reason. */
static int rnd_next(unsigned long *seed)
{
    *seed = *seed * 1103515245UL + 12345UL;
    return (int)((*seed >> 16) & 0x7FFF);
}

/* "h" / "v" for a --subdivide axis, or -1 for anything else. Defined with the
 * command line further down; the self-test reaches it to pin the axis
 * convention, which nothing else in the file can observe. */
static int axis_from_letter(const wchar_t *s);

int slice_selftest(void)
{
    ResSnapshot base, after;
    /* Zeroed before the first st_init: st_init releases an existing tree so
     * that re-initialising one cannot orphan it, and that means it has to be
     * handed a tree it can trust. */
    SliceTree t;
    int fails = 0, i;

    memset(&t, 0, sizeof(t));
    app_set_verbose(1);
    app_log("MuskSlice self-test");

    /* --- the empty picture ------------------------------------------------- */
    st_init(&t, 300, 200);
    fails += check_i(st_count(&t), 1, L"a fresh picture is one whole slice");
    fails += check(tree_problems(&t) == 0, L"the fresh slice covers everything");

    /* --- even layouts ------------------------------------------------------ */
    fails += check_i(st_equal_grid(&t, 3, 2), 6, L"a 3x2 grid makes 6 slices");
    fails += check(tree_problems(&t) == 0, L"a 3x2 grid still tiles exactly");
    {
        Slice **lv;
        int n = 0;
        lv = st_leaves(&t, &n);
        if (lv) {
            /* reading order: top-left, then right, then the second row */
            fails += check(lv[0]->r.left == 0 && lv[0]->r.top == 0,
                           L"slice 1 is the top-left one");
            fails += check(lv[1]->r.left == 100 && lv[1]->r.top == 0,
                           L"slice 2 is to its right");
            fails += check(lv[2]->r.left == 200 && lv[2]->r.top == 0,
                           L"slice 3 is the third column, same row");
            fails += check(lv[3]->r.top == 100, L"slice 4 wraps to the second row");
            xfree(lv);
        } else {
            fails += check(0, L"leaves are listed");
        }
    }
    fails += check_i(st_equal_cols(&t, 4), 4, L"4 columns");
    fails += check(tree_problems(&t) == 0, L"4 columns tile exactly");
    fails += check_i(st_equal_rows(&t, 5), 5, L"5 rows");
    fails += check(tree_problems(&t) == 0, L"5 rows tile exactly");
    fails += check_i(st_equal_cols(&t, 999), 300, L"more columns than pixels clamps");
    fails += check(tree_problems(&t) == 0, L"the clamped column layout still tiles");
    fails += check_i(st_equal_rows(&t, 1), 1, L"one row is a single slice");
    /* One column is not a special case the caller has to know about: the panel
     * re-applies the row and column boxes to every picture that is opened, and
     * the column box starts at 1. Asking for 1 x 5 has to make five slices. */
    fails += check_i(st_equal_grid(&t, 1, 5), 5, L"a 1-column grid makes one slice per row");
    fails += check(tree_problems(&t) == 0, L"the single-column grid tiles exactly");
    fails += check_i(st_equal_grid(&t, 1, 1), 1, L"a 1x1 grid is a single slice");
    fails += check_i(st_equal_grid(&t, 2, 1), 2, L"a 2x1 grid makes two slices");

    /* --- fixed cell size ---------------------------------------------------- */
    st_init(&t, 300, 200);
    /* 300/64 -> 5 columns (the last one 44px), 200/64 -> 4 rows (the last 8px) */
    fails += check_i(st_fixed(&t, 64), 20, L"64px cells over 300x200 make 20 slices");
    fails += check(tree_problems(&t) == 0, L"fixed cells tile exactly");
    {
        Slice **lv;
        int n = 0, lastW = 0, lastH = 0;
        lv = st_leaves(&t, &n);
        if (lv) {
            for (i = 0; i < n; i++) {
                if (lv[i]->r.right == 300 && lv[i]->r.right - lv[i]->r.left < 64)
                    lastW = lv[i]->r.right - lv[i]->r.left;
                if (lv[i]->r.bottom == 200 && lv[i]->r.bottom - lv[i]->r.top < 64)
                    lastH = lv[i]->r.bottom - lv[i]->r.top;
            }
            xfree(lv);
        }
        fails += check(lastW == 44, L"the leftover column strip keeps its 44px");
        fails += check(lastH == 8, L"the leftover row strip keeps its 8px");
    }

    /* --- subdividing one region, over and over ----------------------------- */
    st_init(&t, 400, 300);
    st_equal_grid(&t, 2, 2);
    fails += check_i(st_count(&t), 4, L"a 2x2 grid starts at 4 slices");
    {
        Slice *cell = st_leaf_at_point(&t, 50, 50);
        Slice *other, *deep;
        fails += check(cell != NULL && cell->r.right - cell->r.left == 200,
                       L"the top-left region is 200px wide");
        st_divide_both(cell, 2);
        fails += check_i(st_count(&t), 7, L"subdividing one region adds 3, not 12");
        fails += check(tree_problems(&t) == 0, L"the subdivided picture still tiles");
        /* The pointer stays a valid region, which is what lets the window keep
         * drilling: stepping into its first child and dividing that goes one
         * level deeper each time instead of re-cutting to the same depth.
         * Each step replaces the 2 leaves that child already held with the 4 of
         * a fresh 2x2, so the count climbs by 2, not by 3 - the third one is
         * already there. */
        deep = st_first_child(cell);
        fails += check(deep != NULL && deep != cell,
                       L"a divided region has a child to step into");
        st_divide_both(deep, 2);
        fails += check_i(st_count(&t), 9, L"going one level deeper adds 2 more");
        fails += check(tree_problems(&t) == 0, L"still tiled one level deeper");
        st_divide_both(st_first_child(deep), 2);
        fails += check_i(st_count(&t), 11, L"and again, it keeps going deeper");
        fails += check(tree_problems(&t) == 0, L"still tiled two levels deeper");
        /* the three untouched regions must not have moved */
        other = st_leaf_at_point(&t, 350, 250);
        fails += check(other && other->r.left == 200 && other->r.top == 150 &&
                       other->r.right == 400 && other->r.bottom == 300,
                       L"the neighbouring regions are left alone");
    }
    /* a region smaller than the requested count is clamped, not collapsed */
    st_init(&t, 10, 10);
    {
        Slice *c = st_leaf_at_point(&t, 0, 0);
        st_divide(c, 0, 99);
        fails += check_i(st_count(&t), 10, L"a 10px region cannot become 99 columns");
        fails += check(tree_problems(&t) == 0, L"the clamped split still tiles");
        {
            Slice **lv;
            int n = 0, minW = 999;
            lv = st_leaves(&t, &n);
            if (lv) {
                for (i = 0; i < n; i++) {
                    int w = lv[i]->r.right - lv[i]->r.left;
                    if (w < minW) minW = w;
                }
                xfree(lv);
            }
            fails += check(minW >= 1, L"no slice is ever narrower than a pixel");
        }
    }

    /* --- subdividing everything --------------------------------------------- */
    st_init(&t, 200, 200);
    st_equal_grid(&t, 2, 2);
    fails += check_i(st_divide_all(&t, 3), 36, L"dividing all 4 regions into 3x3 makes 36");
    fails += check(tree_problems(&t) == 0, L"dividing everything still tiles");

    /* --- a hand-drawn line --------------------------------------------------- */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 3, 1);
    fails += check_i(st_count(&t), 3, L"3 columns, 1 row");
    fails += check_i(st_cut(&t, 1, 100, 0, 300), 3, L"a line across the whole picture cuts all 3");
    fails += check_i(st_count(&t), 6, L"3 regions become 6 after one line");
    fails += check(tree_problems(&t) == 0, L"the line tiled the picture exactly");
    /* A second line at a different height only reaches the regions it actually
     * crosses - the three in the lower half, not all six. A line that cut
     * through rows that were already separated would be the bug. */
    fails += check_i(st_cut(&t, 1, 150, 0, 300), 3, L"a second line cuts only the 3 it crosses");
    fails += check_i(st_count(&t), 9, L"9 slices after two lines");
    fails += check(tree_problems(&t) == 0, L"still tiled after two lines");
    /* a line already on a boundary must do nothing rather than make zero-width
     * slices */
    fails += check_i(st_cut(&t, 1, 100, 0, 300), 0, L"a line on an existing edge cuts nothing");
    fails += check_i(st_count(&t), 9, L"repeating the line changed nothing");
    /* a short line only cuts what it was dragged across */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 3, 1);
    fails += check_i(st_cut(&t, 1, 100, 0, 100), 1, L"a line over one third cuts only that one");
    fails += check_i(st_count(&t), 4, L"one region became two");
    fails += check(tree_problems(&t) == 0, L"the partial line left no gap");
    /* a line through already-subdivided regions must reach the little ones */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 1, 1);
    st_divide_all(&t, 3);
    fails += check_i(st_count(&t), 9, L"a 3x3 grid to start with");
    fails += check_i(st_cut(&t, 1, 50, 0, 300), 3, L"a line cuts the 3 regions it crosses");
    fails += check_i(st_count(&t), 12, L"cutting through subdivided regions adds 3");
    fails += check(tree_problems(&t) == 0, L"cutting through subdivided regions tiles exactly");
    /* outside the picture */
    st_init(&t, 100, 100);
    fails += check_i(st_cut(&t, 0, -5, 0, 100), 0, L"a line off the left edge cuts nothing");
    fails += check_i(st_cut(&t, 0, 500, 0, 100), 0, L"a line off the right edge cuts nothing");
    fails += check_i(st_cut(&t, 1, 50, 0, 0), 0, L"a zero-length drag cuts nothing");

    /* The letters name the line, not the strips it leaves: "h" has to mean
     * rows. Every other test here would still pass with the two swapped,
     * because dividing 90x60 into 3 gives three either way - only the shape
     * tells them apart. */
    fails += check(axis_from_letter(L"h") == 1 && axis_from_letter(L"H") == 1 &&
                   axis_from_letter(L"v") == 0 && axis_from_letter(L"V") == 0 &&
                   axis_from_letter(L"x") == -1 && axis_from_letter(L"") == -1,
                   L"h is a horizontal line and v a vertical one");
    st_init(&t, 90, 60);
    {
        Slice **lv;
        int n = 0, k, rows = 0, cols = 0;
        st_divide(st_leaf_at_point(&t, 0, 0), axis_from_letter(L"h"), 3);
        lv = st_leaves(&t, &n);
        if (lv) {
            for (k = 0; k < n; k++) {
                if (lv[k]->r.bottom - lv[k]->r.top < 60) rows++;
                if (lv[k]->r.right - lv[k]->r.left < 90) cols++;
            }
            xfree(lv);
        }
        fails += check(n == 3 && rows == 3 && cols == 0, L"dividing by h gives 3 rows");
        st_init(&t, 90, 60);
        st_divide(st_leaf_at_point(&t, 0, 0), axis_from_letter(L"v"), 3);
        lv = st_leaves(&t, &n);
        rows = cols = 0;
        if (lv) {
            for (k = 0; k < n; k++) {
                if (lv[k]->r.bottom - lv[k]->r.top < 60) rows++;
                if (lv[k]->r.right - lv[k]->r.left < 90) cols++;
            }
            xfree(lv);
        }
        fails += check(n == 3 && rows == 0 && cols == 3, L"dividing by v gives 3 columns");
    }

    /* --- deleting ------------------------------------------------------------ */
    st_init(&t, 400, 300);
    st_equal_grid(&t, 2, 2);
    {
        Slice *c = st_leaf_at_point(&t, 50, 50);
        Slice *sub, *topLevel;
        st_divide_both(c, 2);
        fails += check_i(st_count(&t), 7, L"7 slices before deleting one");
        /* the useful case: drop one of the four quarters, and the half it
         * belonged to comes back as a single slice. Only the region that
         * *contained* it is affected - its 2 leaves become 1 again, so the
         * count drops by 1 rather than back to the pre-subdivision 4. */
        sub = st_leaf_at_point(&t, 50, 50);
        fails += check(sub != c, L"the quarter is a different region from its parent");
        fails += check_i(st_delete(&t, sub), 1, L"a subdivided quarter can be deleted");
        fails += check_i(st_count(&t), 6, L"deleting it hands its pixels back to its region");
        fails += check(tree_problems(&t) == 0, L"the picture still tiles after a delete");
        /* A slice directly under the root is refused: its pixels have nowhere
         * to go, and collapsing the root would throw away the whole grid.
         * st_equal_grid hangs the quadrants one level lower, so the tree has
         * to be cut across the root only for this to be reachable at all. */
        st_free(&t);
        st_init(&t, 400, 300);
        st_equal_cols(&t, 2);
        topLevel = st_leaf_at_point(&t, 300, 150);
        fails += check(topLevel && topLevel->r.left == 200,
                       L"a column cut leaves a direct child of the root");
        fails += check_i(st_delete(&t, topLevel), 0, L"a top-level slice cannot be deleted alone");
        fails += check_i(st_count(&t), 2, L"the refused delete changed nothing");
        fails += check_i(st_delete(&t, t.root), 0, L"the whole picture cannot be deleted");
    }
    st_free(&t);

    /* --- dragging a grid line ------------------------------------------------ */
    /* Everything below is about one invariant: the leaves have to tile the
     * picture exactly, before and after. A moved line trades a band of pixels
     * from one cell to another, and the whole operation is only worth having if
     * the arithmetic is exact - so every case checks tree_problems(), not just
     * the counts. */

    /* the plain case: one column boundary slides sideways */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 3, 1);
    fails += check_i(st_move_line(&t, 0, 100, 150, 0, 200), 2, L"moving a line changes 2 cells");
    fails += check_i(st_count(&t), 3, L"a line that moves does not change the cell count");
    fails += check(tree_problems(&t) == 0, L"a moved line still tiles exactly");
    {
        Slice **lv;
        int n = 0;
        lv = st_leaves(&t, &n);
        if (lv) {
            fails += check(lv[0]->r.right == 150, L"the cell in front of the line grew");
            fails += check(lv[1]->r.left == 150 && lv[1]->r.right == 200,
                           L"the cell behind it gave exactly that ground up");
            fails += check(lv[2]->r.left == 200, L"the cell past it did not move");
            xfree(lv);
        } else {
            fails += check(0, L"the three cells are still listed");
        }
    }
    /* and back the other way, which is the same trade in reverse */
    fails += check_i(st_move_line(&t, 0, 150, 100, 0, 200), 2, L"moving it back changes 2 cells");
    fails += check(tree_problems(&t) == 0, L"moving a line back tiles exactly");

    /* the whole run moves, not just the segment under the pointer: in a 3x3 grid
     * the three column segments share nothing but their x, and a user pointing
     * at one of them means the line, not the piece. */
    st_init(&t, 300, 300);
    st_equal_grid(&t, 3, 3);
    fails += check_i(st_move_line(&t, 0, 100, 140, 0, 100), 6,
                     L"grabbing one segment of a column line moves all three");
    fails += check(tree_problems(&t) == 0, L"the moved column line still tiles exactly");
    {
        Slice **lv;
        int n = 0, k, rowsAgree = 1;
        lv = st_leaves(&t, &n);
        if (lv) {
            /* Nothing may be left sitting on the boundary that was moved away
             * from: a cell still starting at x=100 would mean that row took the
             * move and the others did not. */
            for (k = 0; k < n; k++)
                if (lv[k]->r.left == 100) rowsAgree = 0;
            xfree(lv);
        }
        fails += check(rowsAgree, L"no cell is left on the old column boundary");
        fails += check_i(st_count(&t), 9, L"moving a column line keeps 9 cells");
    }

    /* a line that exists in only part of the picture moves only that part, and
     * the rows it does not reach are left exactly as they were */
    st_init(&t, 300, 200);
    st_cut(&t, 1, 100, 0, 300);
    st_cut(&t, 1, 150, 0, 300);
    st_cut(&t, 0, 100, 0, 100);
    fails += check_i(st_count(&t), 4, L"three bands, the top one cut in two");
    fails += check_i(st_move_line(&t, 0, 100, 160, 0, 100), 2,
                     L"only the band that had the edge moves");
    fails += check(tree_problems(&t) == 0, L"a partly present line still tiles exactly");
    {
        Slice **lv;
        int n = 0, k, untouched = 1;
        lv = st_leaves(&t, &n);
        if (lv) {
            for (k = 0; k < n; k++)
                if (lv[k]->r.top >= 100 && lv[k]->r.right == 100) untouched = 0;
            xfree(lv);
        }
        fails += check(untouched, L"the bands the line never crossed did not move");
    }
    /* Asking for the same line across the whole picture has to be refused: the
     * two bands below it run straight through x=100, so moving the line there
     * would tear the tiling. A refusal leaves the tree exactly as it was. */
    fails += check_i(st_move_line(&t, 0, 100, 160, 0, 200), 0,
                     L"a line through undivided bands is refused, not forced");
    fails += check(tree_problems(&t) == 0, L"the refused move changed nothing");
    fails += check_i(st_count(&t), 4, L"the refused move kept the cell count");

    /* dragging a line past its neighbour: the cell in the band stops existing
     * and hands its pixels to the one that grew over it */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 3, 1);
    fails += check_i(st_move_line(&t, 0, 100, 250, 0, 200), 3,
                     L"all three cells react to a line dragged past the next one");
    fails += check_i(st_count(&t), 2, L"the cell the line passed over is gone");
    fails += check(tree_problems(&t) == 0, L"swallowing a cell still tiles exactly");
    {
        Slice **lv;
        int n = 0;
        lv = st_leaves(&t, &n);
        if (lv) {
            fails += check(lv[0]->r.right == 250 && lv[1]->r.left == 250,
                           L"the two survivors share the new boundary");
            xfree(lv);
        }
    }

    /* the same on a horizontal line, so the axis convention is pinned both ways */
    st_init(&t, 200, 300);
    st_equal_rows(&t, 3);
    fails += check_i(st_move_line(&t, 1, 100, 175, 0, 200), 2, L"a horizontal line moves too");
    fails += check(tree_problems(&t) == 0, L"the moved row line still tiles exactly");
    {
        Slice **lv;
        int n = 0;
        lv = st_leaves(&t, &n);
        if (lv) {
            fails += check(lv[0]->r.bottom == 175, L"the row in front grew downwards");
            xfree(lv);
        }
    }

    /* every way a move can be refused, and the tree left alone through all of
     * them - a line that is not there, a move of no distance, a line on the
     * picture's own border, and a target outside the picture. */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 3, 1);
    fails += check_i(st_move_line(&t, 0, 137, 200, 0, 200), 0,
                     L"a position that is not on any edge moves nothing");
    fails += check_i(st_move_line(&t, 0, 100, 100, 0, 200), 0,
                     L"a move of zero distance moves nothing");
    fails += check_i(st_move_line(&t, 0, 0, 50, 0, 200), 0,
                     L"the picture's own left edge is not a movable line");
    fails += check_i(st_move_line(&t, 0, 300, 250, 0, 200), 0,
                     L"the picture's own right edge is not a movable line");
    fails += check(tree_problems(&t) == 0, L"nothing refused left the tree broken");
    /* a target past the edge is clamped to the edge rather than refused: the
     * user asked to push the line as far as it goes, and the edge is as far as
     * it goes. */
    {
        int to = -999;
        st_line_target(&t, 0, 100, 9999, 0, 200, &to, NULL, NULL);
        fails += check(to == 299, L"a target past the right edge clamps to it");
        st_line_target(&t, 0, 100, -9999, 0, 200, &to, NULL, NULL);
        fails += check(to == 1, L"a target past the left edge clamps to it");
        fails += check(st_line_target(&t, 0, 137, 200, 0, 200, &to, NULL, NULL) == 0,
                       L"st_line_target reports a line that cannot move");
    }
    /* and the preview has to name the same move that st_move_line then makes:
     * that agreement is the whole reason the preview is asked for, and it is
     * what stops the line the user watched from being a different line */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 4, 1);
    {
        int to = 0, lo = 0, hi = 0;
        st_line_target(&t, 0, 150, 290, 0, 200, &to, &lo, &hi);
        fails += check(lo == 0 && hi == 200, L"the preview grows to the whole line");
        fails += check_i(st_move_line(&t, 0, 150, to, lo, hi), 3,
                         L"the previewed move is the move that happens");
        fails += check(tree_problems(&t) == 0, L"and it tiles exactly");
        fails += check_i(st_count(&t), 3, L"the cell the previewed move passed is gone");
    }

    /* --- grabbing a grid line ------------------------------------------------ */
    /* nearest_edge decides whether a press grabs a line or falls through to
     * drawing a cut, and getting it wrong has no symptom anyone can see in the
     * app: the drag simply does the other thing. So the arithmetic is pinned
     * here directly, on a synthetic view, rather than left to be discovered by
     * someone trying it and concluding the feature is broken. */
    {
        RECT cell, view;
        int axis = -1, pos = -1, off = 0;
        /* A 400px picture at 2x inside a view whose origin is (0,0), so image
         * pixel p sits at screen 2p and the arithmetic below can be checked by
         * hand. cell = (100,100,200,200) therefore has its four borders at
         * screen 200 and 400 on both axes. */
        SetRect(&view, 0, 0, 400, 400);
        SetRect(&cell, 100, 100, 200, 200);

        fails += check(nearest_edge(&cell, &view, 2.0, 400, 400, 200, 250, 8,
                                    &axis, &pos, &off) && axis == 0 && pos == 100,
                       L"a press right on a vertical border grabs that line");
        fails += check(nearest_edge(&cell, &view, 2.0, 400, 400, 204, 250, 8,
                                    &axis, &pos, &off) && axis == 0 && pos == 100
                                    && off == 4,
                       L"a press a few pixels off it still grabs it, and keeps the offset");
        fails += check(nearest_edge(&cell, &view, 2.0, 400, 400, 250, 204, 8,
                                    &axis, &pos, &off) && axis == 1 && pos == 100
                                    && off == 4,
                       L"and the same on the other axis, which is a horizontal line");
        /* the horizontal wins when it is genuinely nearer, so a pointer sliding
         * along a border picks up the crossing line as it passes it */
        fails += check(nearest_edge(&cell, &view, 2.0, 400, 400, 300, 204, 8,
                                    &axis, &pos, &off) && axis == 1,
                       L"the closer of two neighbours wins");
        /* just outside the reach is a miss, which is what lets a drag starting in
         * the middle of a cell still draw a cut */
        fails += check(!nearest_edge(&cell, &view, 2.0, 400, 400, 209, 250, 8,
                                     &axis, &pos, &off),
                       L"a press just past the reach grabs nothing");
        fails += check(!nearest_edge(&cell, &view, 2.0, 400, 400, 290, 290, 8,
                                     &axis, &pos, &off),
                       L"a press in the middle of the cell grabs nothing");
        /* 204,204 is 4px from the vertical border at 200 and 4px from the
         * horizontal one at 200 - an exact tie, resolved to the vertical every
         * time, because a cursor flipping between the two as the pointer drifts
         * is what makes a handle feel like it is twitching */
        fails += check(nearest_edge(&cell, &view, 2.0, 400, 400, 204, 204, 8,
                                    &axis, &pos, &off) && axis == 0 && pos == 100,
                       L"a corner tie resolves to the vertical line");
        /* the picture's own border is not a line anyone can move */
        SetRect(&cell, 0, 0, 100, 100);
        fails += check(!nearest_edge(&cell, &view, 2.0, 400, 400, 2, 50, 8,
                                     &axis, &pos, &off),
                       L"the picture's own left border is not grabbable");
        fails += check(nearest_edge(&cell, &view, 2.0, 400, 400, 204, 50, 8,
                                    &axis, &pos, &off) && axis == 0 && pos == 100,
                       L"but the first interior border is");
    }

    /* --- merging cells -------------------------------------------------------- */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 2, 2);
    fails += check_i(st_count(&t), 4, L"a 2x2 grid starts at 4 slices");
    {
        RECT r;
        int cells = 0, aligned = 0;
        SetRect(&r, 0, 0, 150, 100);
        cells = st_rect_cells(&t, r, &aligned);
        fails += check(aligned, L"a box drawn on cell edges is aligned");
        fails += check_i(cells, 1, L"that box holds exactly one cell");
        fails += check_i(st_merge_rect(&t, r), 0, L"merging a single cell is refused");
        fails += check_i(st_count(&t), 4, L"the refused single-cell merge changed nothing");

        SetRect(&r, 0, 0, 150, 200);
        cells = st_rect_cells(&t, r, &aligned);
        fails += check_i(cells, 2, L"the left half holds two cells");
        fails += check_i(st_merge_rect(&t, r), 2, L"two cells merge into one");
        fails += check_i(st_count(&t), 3, L"a 2x2 grid becomes 3 after merging a column");
        fails += check(tree_problems(&t) == 0, L"the merged picture still tiles exactly");
        {
            Slice *m = st_leaf_at_point(&t, 50, 100);
            fails += check(m && m->r.left == 0 && m->r.top == 0 &&
                           m->r.right == 150 && m->r.bottom == 200,
                           L"the merged cell is exactly the box that was drawn");
            fails += check(m && m->nkid == 0, L"the merged cell is a plain cell again");
            /* and it can be cut up again, which is the point of handing back a
             * single cell rather than a flat list of four */
            st_divide_both(m, 2);
            fails += check_i(st_count(&t), 6, L"the merged cell subdivides again");
            fails += check(tree_problems(&t) == 0, L"re-dividing a merged cell tiles");
        }
    }
    /* a box that clips a cell is refused, and nothing about the tree changes */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 2, 2);
    {
        RECT r;
        int aligned = 0;
        SetRect(&r, 0, 0, 151, 100);
        fails += check_i(st_rect_cells(&t, r, &aligned), 0,
                         L"a box clipping a cell holds no cells");
        fails += check(!aligned, L"and it is reported as not aligned");
        fails += check_i(st_merge_rect(&t, r), 0, L"a misaligned box cannot be merged");
        fails += check_i(st_count(&t), 4, L"the misaligned merge changed nothing");
        fails += check(tree_problems(&t) == 0, L"and left the tiling intact");
        /* outside the picture is refused the same way */
        SetRect(&r, 0, 0, 400, 100);
        fails += check_i(st_merge_rect(&t, r), 0, L"a box past the edge cannot be merged");
        SetRect(&r, 0, 0, 0, 0);
        fails += check_i(st_merge_rect(&t, r), 0, L"an empty box cannot be merged");
    }
    /* merging the whole picture is the same as 重置 */
    st_init(&t, 300, 200);
    st_equal_grid(&t, 3, 2);
    st_divide_all(&t, 2);
    fails += check_i(st_count(&t), 24, L"6 cells subdivided into 2x2 makes 24");
    fails += check_i(st_merge_rect(&t, t.root->r), 24, L"merging everything swallows all 24");
    fails += check_i(st_count(&t), 1, L"and leaves one cell, like 重置");
    fails += check(tree_problems(&t) == 0, L"the fully merged picture still tiles");

    /* a box that covers a whole already-subdivided cell collapses that cell
     * rather than flattening it - this is the case a user hits after 细分. The
     * box has to be taken from the region before it is divided, because
     * afterwards the cell under that point is one of the quarters, and a box
     * around a quarter is just a quarter. */
    st_init(&t, 400, 300);
    st_equal_grid(&t, 2, 2);
    {
        Slice *cell = st_leaf_at_point(&t, 50, 50);
        RECT r = cell->r;
        st_divide_both(cell, 2);
        fails += check_i(st_count(&t), 7, L"7 slices before merging one back together");
        fails += check_i(st_merge_rect(&t, r), 4, L"the four quarters merge into one");
        fails += check_i(st_count(&t), 4, L"which is back to the plain 2x2 grid");
        fails += check(tree_problems(&t) == 0, L"the collapsed cell still tiles exactly");
        fails += check(cell->nkid == 0, L"the collapsed cell is a plain cell again");
    }

    /* --- move and merge together ---------------------------------------------- */
    /* A line dragged across a merged cell has to survive the merged cell's much
     * wider face on it - the two features lean on each other and have to be
     * tested against each other, not only on their own. */
    st_init(&t, 400, 300);
    st_equal_grid(&t, 2, 2);
    {
        RECT r;
        SetRect(&r, 0, 0, 200, 300);
        fails += check_i(st_merge_rect(&t, r), 2, L"the left column merges into one cell");
        fails += check_i(st_count(&t), 3, L"4 cells become 3 after merging a column");
        /* the vertical boundary is now a single 300px face rather than two
         * 150px ones, and the line has to move across all of it */
        fails += check_i(st_move_line(&t, 0, 200, 120, 0, 300), 3,
                         L"the line moves across the merged cell and both rows below it");
        fails += check_i(st_count(&t), 3, L"still three cells after the move");
        fails += check(tree_problems(&t) == 0, L"moving across a merged cell tiles exactly");
        {
            Slice **lv;
            int n = 0;
            lv = st_leaves(&t, &n);
            if (lv) {
                fails += check(lv[0]->r.right == 120 && lv[1]->r.left == 120,
                               L"both halves followed the line");
                xfree(lv);
            }
        }
    }

    /* --- naming ---------------------------------------------------------------- */
    {
        wchar_t *n;
        n = st_slice_name(L"photo", 0, 12, 0);
        fails += check(n && wcscmp(n, L"photo_01.png") == 0, L"first of 12 is photo_01.png");
        xfree(n);
        n = st_slice_name(L"photo", 11, 12, 0);
        fails += check(n && wcscmp(n, L"photo_12.png") == 0, L"last of 12 is photo_12.png");
        xfree(n);
        n = st_slice_name(L"photo", 99, 120, 1);
        fails += check(n && wcscmp(n, L"photo_100.jpg") == 0,
                       L"padding widens to the largest index, and jpg is used");
        xfree(n);
        n = st_slice_name(L"a/b:c*d?", 0, 3, 0);
        fails += check(n && wcscmp(n, L"abcd_01.png") == 0,
                       L"illegal file-name characters are dropped");
        xfree(n);
        n = st_slice_name(L"...", 0, 3, 0);
        fails += check(n && wcscmp(n, L"slice_01.png") == 0,
                       L"an all-dots prefix falls back to a real name");
        xfree(n);
        n = st_slice_name(L"", 0, 3, 0);
        fails += check(n && wcscmp(n, L"slice_01.png") == 0, L"an empty prefix falls back");
        xfree(n);
    }
/* --- path helpers ---------------------------------------------------------- */
    {
        wchar_t *s = img_stem(L"C:\\pics\\holiday shot.png");
        fails += check(s && wcscmp(s, L"holiday shot") == 0,
                       L"stem drops the folder and the extension");
        xfree(s);
        s = img_default_dir(L"C:\\pics\\holiday.png");
        fails += check(s && wcscmp(s, L"C:\\pics\\holiday_slices") == 0,
                       L"the default output folder sits beside the source");
        xfree(s);
        s = img_default_dir(L"holiday.png");
        fails += check(s && wcscmp(s, L"holiday_slices") == 0,
                       L"a bare file name gets no folder part");
        xfree(s);
        fails += check(img_is_supported(L"a.PNG") && img_is_supported(L"a.jpeg") &&
                       !img_is_supported(L"a.bmp") && !img_is_supported(L"a") &&
                       !img_is_supported(L"a.pngx"),
                       L"only .png/.jpg/.jpeg are accepted");
    }
    {
        /* dir_join is what keeps a folder typed without a trailing backslash
         * from producing "photo_slices_01.png" */
        wchar_t j[64];
        dir_join(j, 64, L"C:\\out", L"a_01.png");
        fails += check(wcscmp(j, L"C:\\out\\a_01.png") == 0, L"dir_join adds the separator");
        dir_join(j, 64, L"C:\\out\\", L"a_01.png");
        fails += check(wcscmp(j, L"C:\\out\\a_01.png") == 0, L"dir_join does not double it");
    }

    /* --- the control panel stays inside itself ----------------------------------- */
    {
        /* Sizes at and above SL_MIN_CLIENT_*, which is what WM_GETMINMAXINFO lets
         * the user actually produce. Below that the window cannot exist, so a
         * layout that only holds together for those sizes is not a layout anyone
         * will ever see. */
        static const int sizes[][2] = {
            {SL_MIN_CLIENT_W, SL_MIN_CLIENT_H},
            {1020, 700}, {900, 640}, {1200, 800}, {760, 600}
        };
        const RECT *rs[26];
        wchar_t worst[192];
        int bad = 0, s2;
        worst[0] = 0;
        for (s2 = 0; s2 < 5; s2++) {
            int n = 0, k;
            build_layout(sizes[s2][0], sizes[s2][1]);
            rs[n++] = &g_lay.open;      rs[n++] = &g_lay.export;
            rs[n++] = &g_lay.browse;    rs[n++] = &g_lay.dir;
            rs[n++] = &g_lay.rowsLbl;   rs[n++] = &g_lay.rowsMinus;
            rs[n++] = &g_lay.rowsEdit;  rs[n++] = &g_lay.rowsPlus;
            rs[n++] = &g_lay.colsLbl;   rs[n++] = &g_lay.colsMinus;
            rs[n++] = &g_lay.colsEdit;  rs[n++] = &g_lay.colsPlus;
            rs[n++] = &g_lay.cellLbl;   rs[n++] = &g_lay.cellEdit;
            rs[n++] = &g_lay.fixed;     rs[n++] = &g_lay.eqRows;
            rs[n++] = &g_lay.eqCols;    rs[n++] = &g_lay.reset;
            rs[n++] = &g_lay.selNote;   rs[n++] = &g_lay.divEdit;
            rs[n++] = &g_lay.divH;      rs[n++] = &g_lay.divV;
            rs[n++] = &g_lay.divAll;    rs[n++] = &g_lay.delSel;
            rs[n++] = &g_lay.mergeSel;
            rs[n++] = &g_lay.fmt;       rs[n++] = &g_lay.qual;
            for (k = 0; k < n; k++) {
                const RECT *r = rs[k];
                if (r->left < g_lay.panel.left || r->right > g_lay.panel.right ||
                    r->top < 0 || r->bottom > g_lay.panel.bottom) {
                    if (bad == 0)
                        _snwprintf_s(worst, 192, _TRUNCATE,
                                     L"rect %d,%d,%d,%d escapes the panel %d,%d,%d,%d",
                                     r->left, r->top, r->right, r->bottom,
                                     g_lay.panel.left, g_lay.panel.top,
                                     g_lay.panel.right, g_lay.panel.bottom);
                    bad++;
                }
            }
        }
        fails += check(bad == 0, L"every panel control stays inside the panel");
        if (bad) app_logw(L"        %d control(s) overflow; %s", bad, worst);
    }

    /* --- churn: every operation, in any order, must keep the tiling exact ------ */
    {
        unsigned long seed = 12345;
        int bad = 0;
        st_init(&t, 240, 160);
        st_equal_grid(&t, 2, 2);
        /* The baseline has to be taken with no tree alive. The churn leaves a
         * bigger tree than it started from - that is the point of a churn - so
         * snapshotting with one still standing would report its node count as a
         * leak and hide a real one behind it. */
        st_free(&t);
        res_snapshot(&base);
        st_init(&t, 240, 160);
        st_equal_grid(&t, 2, 2);
        for (i = 0; i < 600; i++) {
            int op = rnd_next(&seed) % 12;
            if (op < 4) {
                st_cut(&t, rnd_next(&seed) & 1, 1 + rnd_next(&seed) % 238, 0,
                       1 + rnd_next(&seed) % 160);
            } else if (op < 6) {
                Slice **lv;
                int n = 0, k, how;
                lv = st_leaves(&t, &n);
                if (lv) {
                    k = rnd_next(&seed) % n;
                    how = rnd_next(&seed);
                    if (how & 1) st_divide(lv[k], how & 1, 2 + rnd_next(&seed) % 4);
                    else         st_divide_both(lv[k], 2 + rnd_next(&seed) % 3);
                    xfree(lv);
                }
            } else if (op < 8) {
                Slice **lv;
                int n = 0;
                lv = st_leaves(&t, &n);
                if (lv && n > 1) st_delete(&t, lv[rnd_next(&seed) % n]);
                if (lv) xfree(lv);
            } else if (op < 9) {
                st_equal_grid(&t, 1 + rnd_next(&seed) % 5, 1 + rnd_next(&seed) % 4);
            } else if (op < 11) {
                /* A dragged line, with the target thrown well past the edge as
                 * often as not: the clamp is what stops that from producing a
                 * cell wider than the picture, and the tiling check below is
                 * what proves it. */
                int axis = rnd_next(&seed) & 1;
                int span = (axis == 0) ? 160 : 240;
                st_move_line(&t, axis, 1 + rnd_next(&seed) % (span - 2),
                             (int)(rnd_next(&seed) % 600) - 200, 0, span);
            } else {
                /* A merge over a random box. Half of them will be misaligned
                 * and get refused, which is just as much of the contract as the
                 * ones that go through. */
                Slice **lv;
                int n = 0, k, a;
                RECT r;
                lv = st_leaves(&t, &n);
                if (lv && n > 1) {
                    k = rnd_next(&seed) % n;
                    a = rnd_next(&seed) % n;
                    SetRect(&r, lv[k]->r.left, lv[k]->r.top,
                            lv[a]->r.right, lv[a]->r.bottom);
                    st_merge_rect(&t, r);
                    xfree(lv);
                } else if (lv) {
                    xfree(lv);
                }
            }
            /* Keep the churn bounded so the pairwise overlap check inside
             * tree_problems stays cheap. */
            if (st_count(&t) > 200) st_reset(&t);
            if (tree_problems(&t) != 0) { bad++; break; }
        }
        st_free(&t);
        res_snapshot(&after);
        fails += check(bad == 0, L"600 random cuts, splits, moves, merges and deletes "
                                  L"keep the tiling exact");
        if (after.allocs != base.allocs)
            app_log("        heap delta %+ld after the churn", after.allocs - base.allocs);
        fails += check(after.allocs == base.allocs, L"the churn leaks nothing");
        fails += check(after.gdiObjects == base.gdiObjects, L"the churn leaks no GDI objects");
    }

    /* --- the real thing: load, cut, write, read back --------------------------- */
    if (!gdiplus_start()) {
        fails += check(0, L"GDI+ starts for the image tests");
        app_logw(L"self-test %s (%d failures)", fails ? L"FAILED" : L"PASSED", fails);
        return 1;
    }

    {
        wchar_t dir[MAX_PATH], src[MAX_PATH], out[MAX_PATH], p[MAX_PATH];

        if (!GetTempPathW(MAX_PATH, dir)) dir[0] = 0;
        {
            wchar_t *d = wcs_format(L"%sMuskSliceTest", dir);
            wcsncpy(dir, d, MAX_PATH - 1);
            dir[MAX_PATH - 1] = 0;
            xfree(d);
        }
        _snwprintf_s(src, MAX_PATH, _TRUNCATE, L"%ssource.png", dir);
        _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%sout", dir);
        img_mkdir_p(out);

        fails += check(make_test_png(src, 200, 100), L"a 200x100 test PNG can be written");
        {
            SourceImage s;
            fails += check(img_load(&s, src), L"a PNG loads back");
            fails += check(s.w == 200 && s.h == 100, L"the loaded image keeps its size");
            fails += check(px_is(&s, 10, 10, 255, 0, 0, 0), L"top-left quadrant is red");
            fails += check(px_is(&s, 190, 10, 0, 255, 0, 0), L"top-right quadrant is green");
            fails += check(px_is(&s, 10, 90, 0, 0, 255, 0), L"bottom-left quadrant is blue");
            fails += check(px_is(&s, 190, 90, 255, 255, 255, 0), L"bottom-right quadrant is white");
            fails += check(!s.hasAlpha, L"an opaque PNG is not scanned as transparent");

            /* the whole pipeline, on a real tree: a 2x2 grid with one region cut
             * further, written out and read back one file at a time */
            st_init(&t, s.w, s.h);
            st_equal_grid(&t, 2, 2);
            st_divide_both(st_leaf_at_point(&t, 10, 10), 2);
            fails += check_i(st_count(&t), 7, L"the loaded image makes 7 slices");
            {
                Slice **lv;
                int n = 0, bad = 0, all = 0, k;
                /* Reading order is by top edge, then left edge - and that is
                 * not the same as "all of the subdivided region first". The
                 * untouched top-right region shares the top row with two of the
                 * quarters, so it sorts third, between them and the two quarters
                 * that sit 25px lower. */
                static const BYTE want[7][3] = {
                    {255, 0, 0}, {255, 0, 0}, {0, 255, 0}, {255, 0, 0},
                    {255, 0, 0}, {0, 0, 255}, {255, 255, 255}
                };
                lv = st_leaves(&t, &n);
                if (lv) {
                    for (k = 0; k < n; k++) {
                        wchar_t *name = st_slice_name(L"q", k, n, 0);
                        wchar_t path[MAX_PATH + 80];
                        SourceImage c;
                        dir_join(path, MAX_PATH + 80, out, name);
                        xfree(name);
                        all++;
                        if (!img_save_rect(&s, lv[k]->r, path, 0, 100)) { bad++; continue; }
                        if (!img_load(&c, path)) { bad++; continue; }
                        if (c.w != lv[k]->r.right - lv[k]->r.left ||
                            c.h != lv[k]->r.bottom - lv[k]->r.top) bad++;
                        else if (!px_is(&c, 1, 1, want[k][0], want[k][1], want[k][2], 2)) bad++;
                        img_free(&c);
                    }
                    xfree(lv);
                }
                if (bad) app_log("        %d of %d slices wrong", bad, all);
                fails += check(bad == 0 && all == 7,
                               L"all 7 slices are written, sized and coloured correctly");
                for (k = 0; k < 7; k++) {
                    _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%sq_%02d.png", out, k);
                    DeleteFileW(p);
                }
            }
            st_free(&t);
            img_free(&s);
        }

        /* --- transparency ------------------------------------------------------ */
        {
            wchar_t apath[MAX_PATH], tpath[MAX_PATH + 80], jpath[MAX_PATH + 80];
            SourceImage a, c;
            RECT r;

            _snwprintf_s(apath, MAX_PATH, _TRUNCATE, L"%strans.png", out);
            _snwprintf_s(tpath, MAX_PATH + 80, _TRUNCATE, L"%strans_cell.png", out);
            _snwprintf_s(jpath, MAX_PATH + 80, _TRUNCATE, L"%strans_cell.jpg", out);
            fails += check(make_test_png_alpha(apath, 80, 40), L"a PNG with alpha can be written");
            if (img_load(&a, apath)) {
                /* The canvas only draws the checkerboard when this is true, so a
                 * scan that never fires means a transparent PNG renders as a
                 * grey rectangle with no hint that anything is missing. */
                fails += check(a.hasAlpha, L"a transparent PNG is detected as transparent");
                fails += check(PX_AT(a.bits, a.stride, 10, 10).a == 0,
                               L"the transparent half really is transparent");
                SetRect(&r, 0, 0, 40, 40);
                fails += check(img_save_rect(&a, r, tpath, 0, 100),
                               L"a transparent slice can be written as PNG");
                if (img_load(&c, tpath)) {
                    fails += check(PX_AT(c.bits, c.stride, 5, 5).a == 0,
                                   L"PNG output keeps the alpha channel");
                    img_free(&c);
                } else {
                    fails += check(0, L"the transparent PNG slice loads back");
                }
                /* the same slice as JPEG: the alpha has to become white, not black */
                fails += check(img_save_rect(&a, r, jpath, 1, 92),
                               L"a transparent slice can be written as JPEG");
                if (img_load(&c, jpath)) {
                    BGRAPX q = PX_AT(c.bits, c.stride, 5, 5);
                    fails += check(q.r > 200 && q.g > 200 && q.b > 200,
                                   L"JPEG output flattens transparency onto white");
                    img_free(&c);
                } else {
                    fails += check(0, L"the transparent JPEG slice loads back");
                }
                img_free(&a);
            } else {
                fails += check(0, L"a transparent PNG loads back");
            }
            DeleteFileW(apath);
            DeleteFileW(tpath);
            DeleteFileW(jpath);
        }

        /* --- the JPEG path and clipping ------------------------------------------ */
        {
            wchar_t path[MAX_PATH + 80];
            SourceImage s, c;
            RECT r;
            if (img_load(&s, src)) {
                _snwprintf_s(path, MAX_PATH + 80, _TRUNCATE, L"%sq_jpeg.jpg", out);
                SetRect(&r, 0, 0, 100, 50);
                fails += check(img_save_rect(&s, r, path, 1, 92),
                               L"a slice can be written as JPEG");
                if (img_load(&c, path)) {
                    fails += check(c.w == 100 && c.h == 50,
                                   L"the JPEG slice decodes at the right size");
                    fails += check(px_is(&c, 5, 5, 255, 0, 0, 40),
                                   L"the JPEG slice keeps roughly the right colour");
                    img_free(&c);
                } else {
                    fails += check(0, L"the JPEG slice decodes back");
                }
                DeleteFileW(path);

                _snwprintf_s(path, MAX_PATH + 80, _TRUNCATE, L"%sclipped.png", out);
                SetRect(&r, 150, 80, 400, 300);
                fails += check(img_save_rect(&s, r, path, 0, 100),
                               L"an over-large rect is clipped to the image");
                if (img_load(&c, path)) {
                    fails += check(c.w == 50 && c.h == 20,
                                   L"the clipped slice is the visible part");
                    img_free(&c);
                } else {
                    fails += check(0, L"the clipped slice loads back");
                }
                DeleteFileW(path);
                img_free(&s);
            }
        }

        /* --- what must be refused ------------------------------------------------ */
        {
            SourceImage s;
            HANDLE fh;
            _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%snot-an-image.png", out);
            fh = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
            if (fh != INVALID_HANDLE_VALUE) {
                const char junk[] = "this is definitely not a png";
                DWORD wr = 0;
                WriteFile(fh, junk, (DWORD)sizeof(junk), &wr, NULL);
                CloseHandle(fh);
            }
            /* A decoder that trusts its input would hand back a zero-sized bitmap
             * and the slice would be a 0x0 file with a plausible name. */
            fails += check(!img_load(&s, p), L"a file that is not a PNG is refused");
            fails += check(!img_load(&s, L"C:\\definitely\\missing.png"),
                           L"a missing file is refused");
            fails += check(!img_load(&s, L""), L"an empty path is refused");
            DeleteFileW(p);
        }

        /* --- nested output directory ------------------------------------------------ */
        {
            wchar_t nested[MAX_PATH], mid[MAX_PATH], top[MAX_PATH];
            _snwprintf_s(nested, MAX_PATH, _TRUNCATE, L"%sa\\b\\c", out);
            _snwprintf_s(mid, MAX_PATH, _TRUNCATE, L"%sa\\b", out);
            _snwprintf_s(top, MAX_PATH, _TRUNCATE, L"%sa", out);
            fails += check(img_mkdir_p(nested), L"a nested output directory is created");
            fails += check(img_mkdir_p(nested), L"creating an existing directory succeeds");
            RemoveDirectoryW(nested);
            RemoveDirectoryW(mid);
            RemoveDirectoryW(top);
        }

        /* image load/save churn, with every resource counter watched */
        {
            SourceImage s;
            wchar_t path[MAX_PATH + 80];
            res_snapshot(&base);
            for (i = 0; i < 12; i++) {
                RECT r;
                if (!img_load(&s, src)) break;
                SetRect(&r, 0, 0, 1 + i * 10, 1 + i * 5);
                _snwprintf_s(path, MAX_PATH + 80, _TRUNCATE, L"%schurn.png", out);
                img_save_rect(&s, r, path, i & 1, 90);
                img_free(&s);
            }
            res_snapshot(&after);
            fails += check(after.allocs == base.allocs && after.gdiObjects == base.gdiObjects,
                           L"load/save churn leaks nothing");
            _snwprintf_s(path, MAX_PATH + 80, _TRUNCATE, L"%schurn.png", out);
            DeleteFileW(path);
        }

        /* Every GDI+ object the tests above created has to be gone before the
         * shutdown, or the next start inherits a dangling context. */
        DeleteFileW(src);
        RemoveDirectoryW(out);
    }

    checker_release();
    gdiplus_stop();

    app_logw(L"self-test %s (%d failures)", fails ? L"FAILED" : L"PASSED", fails);
    return fails ? 1 : 0;
}
/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

/* Hand-drawn lines from a script: --draw x1,y1,x2,y2, repeatable. The
 * coordinates are image pixels, and a line is stored the way a drag would leave
 * it: which axis, where along it, and the span it covers. */
typedef struct { int axis, pos, lo, hi; } DrawLine;
#define DRAW_MAX 32

/* Subdivisions from a script: --subdivide <index> <h|v> <n>, also repeatable.
 * Repeating it is how a script drills into one cell as far as it wants - each
 * one renumbers the slices against the tree as it stands at that moment, so
 * --subdivide 1 v 2 three times halves the first column three times over. */
typedef struct { int index, axis, n; } SubdivideReq;
#define SUBDIV_MAX 16

/* Grid lines from a script: --move <h|v> <from> <to>, also repeatable, and
 * applied in order against the layout as it stands at that point. The span is
 * left at the whole picture, which is what makes it a grid line rather than a
 * cut - the model grows that to the run of collinear edges the position belongs
 * to, so a 3x3 grid moves all three segments of the column at once. Repeating
 * it is how a script walks a line across in steps. */
typedef struct { int axis, from, to; } MoveReq;
#define MOVE_MAX 16

/* Merges from a script: --merge x1,y1,x2,y2, also repeatable and in order, so
 * "cut the whole picture into 9, put the top-left 2x2 back together" is two
 * lines and exports 6 slices. A box that does not line up with cell edges is
 * reported and skipped rather than failing the whole run, because one bad box
 * in a long script should not throw away the files the rest of it produced. */
typedef struct { RECT r; } MergeReq;
#define MERGE_MAX 16

/* "h" / "v" for a --subdivide axis, or -1 for anything else. The letter names
 * the line being drawn, not the strips being made: a horizontal line separates
 * rows, which is st_divide's axis 1. */
static int axis_from_letter(const wchar_t *s)
{
    if (!s || !*s) return -1;
    if (s[0] == L'h' || s[0] == L'H') return 1;   /* a horizontal line -> rows    */
    if (s[0] == L'v' || s[0] == L'V') return 0;   /* a vertical line   -> columns */
    return -1;
}

/* Headless batch mode, so the slicer can be driven from a script or a file
 * association. Prints one result line and returns the exit code. */
static int run_batch(int argc, LPWSTR *argv)
{
    wchar_t in[MAX_PATH], dir[MAX_PATH], prefix[128], msg[192];
    SourceImage s;
    /* Zeroed before the first st_init, for the reason st_init's own comment
     * gives: it releases whatever tree it is handed, so handing it a local that
     * has never been initialised makes it free a pointer out of stack garbage.
     * The window and the self-test both memset theirs; this path did not, and
     * whether it survived depended on what the stack happened to hold. */
    SliceTree t;
    int i, cols = 0, rows = 0, cell = 0, quality = 90, asJpeg = 0;
    int wrote = 0, total, k;
    DrawLine draws[DRAW_MAX];
    int ndraw = 0;
    SubdivideReq subs[SUBDIV_MAX];
    int nsub = 0;
    MoveReq moves[MOVE_MAX];
    int nmove = 0;
    MergeReq merges[MERGE_MAX];
    int nmerge = 0;

    wcsncpy(prefix, L"slice", 127);
    prefix[127] = 0;
    in[0] = 0;
    dir[0] = 0;
    memset(&t, 0, sizeof(t));

    for (i = 0; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--split") == 0 && i + 1 < argc) {
            wcsncpy(in, argv[++i], MAX_PATH - 1); in[MAX_PATH - 1] = 0;
        } else if (_wcsicmp(argv[i], L"--out") == 0 && i + 1 < argc) {
            wcsncpy(dir, argv[++i], MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
        } else if (_wcsicmp(argv[i], L"--prefix") == 0 && i + 1 < argc) {
            wcsncpy(prefix, argv[++i], 127); prefix[127] = 0;
        } else if (_wcsicmp(argv[i], L"--cols") == 0 && i + 1 < argc) {
            cols = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--rows") == 0 && i + 1 < argc) {
            rows = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--cell") == 0 && i + 1 < argc) {
            cell = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--quality") == 0 && i + 1 < argc) {
            quality = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--format") == 0 && i + 1 < argc) {
            i++;
            asJpeg = (_wcsicmp(argv[i], L"jpeg") == 0 || _wcsicmp(argv[i], L"jpg") == 0);
        } else if (_wcsicmp(argv[i], L"--draw") == 0 && i + 1 < argc && ndraw < DRAW_MAX) {
            /* x1,y1,x2,y2 - the same line a drag across the picture would leave:
             * the dominant direction picks the axis, the cursor's coordinate
             * gives the position, and the drag's own extent gives the span. So
             * "20,30,20,200" is a vertical line at x=20 running from y=30 down
             * to y=200, and a short sweep only cuts what it swept over. */
            wchar_t *spec = argv[++i];
            int a, b, c, d;
            if (swscanf_s(spec, L"%d,%d,%d,%d", &a, &b, &c, &d) != 4) {
                app_logw(L"FAIL: --draw wants x1,y1,x2,y2 (got '%s')", spec);
                return 2;
            }
            if (abs(d - b) >= abs(c - a)) {
                draws[ndraw].axis = 0;
                draws[ndraw].pos  = c;
                draws[ndraw].lo   = (b < d) ? b : d;
                draws[ndraw].hi   = (b < d) ? d + 1 : b + 1;
            } else {
                draws[ndraw].axis = 1;
                draws[ndraw].pos  = d;
                draws[ndraw].lo   = (a < c) ? a : c;
                draws[ndraw].hi   = (a < c) ? c + 1 : a + 1;
            }
            ndraw++;
        } else if (_wcsicmp(argv[i], L"--subdivide") == 0 && i + 3 < argc) {
            /* index, axis, n as three separate words - no scanf trickery over a
             * letter, and a typo is obvious in the usage line */
            int idx  = _wtoi(argv[++i]);
            int axis = axis_from_letter(argv[++i]);
            int cnt  = _wtoi(argv[++i]);
            if (axis < 0 || cnt < 2) {
                app_logw(L"FAIL: --subdivide wants <index> <h|v> <n>: index is 1-based, "
                         L"axis is h or v, n is at least 2");
                return 2;
            }
            if (nsub < SUBDIV_MAX) {
                subs[nsub].index = idx;
                subs[nsub].axis  = axis;
                subs[nsub].n     = cnt;
                nsub++;
            } else {
                app_logw(L"warning: only the first %d --subdivide steps are used",
                         SUBDIV_MAX);
            }
        } else if (_wcsicmp(argv[i], L"--move") == 0 && i + 3 < argc) {
            /* from, to as two numbers and the axis as a letter, matching
             * --subdivide's three-word shape: a typo in a coordinate is then
             * obvious in the usage line instead of silently landing somewhere
             * else. `to` is a position, not a distance, for the same reason it
             * is one on the canvas - it is clamped to what the picture can give,
             * and a script that asks to go past the edge gets the edge. */
            int axis = axis_from_letter(argv[++i]);
            int from = _wtoi(argv[++i]);
            int to   = _wtoi(argv[++i]);
            if (axis < 0) {
                app_logw(L"FAIL: --move wants <h|v> <from> <to>, the axis is h "
                         L"for a horizontal line or v for a vertical one");
                return 2;
            }
            if (nmove < MOVE_MAX) {
                moves[nmove].axis = axis;
                moves[nmove].from = from;
                moves[nmove].to   = to;
                nmove++;
            } else {
                app_logw(L"warning: only the first %d --move steps are used",
                         MOVE_MAX);
            }
        } else if (_wcsicmp(argv[i], L"--merge") == 0 && i + 1 < argc) {
            wchar_t *spec = argv[++i];
            int a, b, c, d;
            if (swscanf_s(spec, L"%d,%d,%d,%d", &a, &b, &c, &d) != 4) {
                app_logw(L"FAIL: --merge wants x1,y1,x2,y2 (got '%s')", spec);
                return 2;
            }
            if (c < a || d < b) {          /* accept either corner order */
                int sw = a; a = c; c = sw;
                sw = b; b = d; d = sw;
            }
            if (nmerge < MERGE_MAX) {
                SetRect(&merges[nmerge].r, a, b, c, d);
                nmerge++;
            } else {
                app_logw(L"warning: only the first %d --merge boxes are used",
                         MERGE_MAX);
            }
        }
    }

    if (!in[0]) {
        app_logw(L"usage: slice.exe --split <image> --out <dir> "
                 L"[--cols N] [--rows N] [--cell PX] "
                 L"[--draw x1,y1,x2,y2]... [--subdivide <index> <h|v> <n>] "
                 L"[--move <h|v> <from> <to>]... [--merge x1,y1,x2,y2]... "
                 L"[--format png|jpeg] [--quality N] [--prefix NAME]");
        return 2;
    }
    if (!img_is_supported(in)) {
        app_logw(L"FAIL: only JPG and PNG are supported (%s)", in);
        return 2;
    }
    if (!img_load(&s, in)) {
        app_logw(L"FAIL: could not load %s", in);
        return 2;
    }
    if (!dir[0]) {
        wchar_t *d = img_default_dir(in);
        wcsncpy(dir, d, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
        xfree(d);
    }
    if (!img_mkdir_p(dir)) {
        app_logw(L"FAIL: could not create %s", dir);
        img_free(&s);
        return 2;
    }

    st_init(&t, s.w, s.h);
    if (cell > 0) {
        st_fixed(&t, cell);
    } else {
        st_equal_grid(&t, cols > 0 ? cols : 1, rows > 0 ? rows : 1);
    }
    for (i = 0; i < ndraw; i++)
        st_cut(&t, draws[i].axis, draws[i].pos, draws[i].lo, draws[i].hi);
    /* In order, each against the leaves as they stand at that point - that is
     * what makes a repeated --subdivide 1 drill down instead of re-cutting the
     * same first slice to the same depth. */
    for (i = 0; i < nsub; i++) {
        Slice **lv;
        int n = 0;
        lv = st_leaves(&t, &n);
        if (!lv) break;
        if (subs[i].index >= 1 && subs[i].index <= n)
            st_divide(lv[subs[i].index - 1], subs[i].axis, subs[i].n);
        else
            app_logw(L"warning: --subdivide index %d is outside 1..%d", subs[i].index, n);
        xfree(lv);
    }
    /* Then the grid lines, in order, each against the layout the previous one
     * left behind - a script that walks a line across in five steps has to move
     * it from wherever the last step put it. The span is passed as the whole
     * picture, which is what makes it a grid line rather than a cut: the model
     * grows that to the run of collinear edges the position belongs to. */
    for (i = 0; i < nmove; i++) {
        int extent = (moves[i].axis == 0) ? s.h : s.w;
        int where = moves[i].from;
        if (!st_line_target(&t, moves[i].axis, moves[i].from, moves[i].to,
                            0, extent, &where, NULL, NULL)) {
            app_logw(L"warning: --move %s %d -> %d is not on a slice edge, skipped",
                     moves[i].axis == 0 ? L"v" : L"h", moves[i].from, moves[i].to);
            continue;
        }
        if (!st_move_line(&t, moves[i].axis, moves[i].from, where, 0, extent))
            app_logw(L"warning: --move %s %d -> %d was refused",
                     moves[i].axis == 0 ? L"v" : L"h", moves[i].from, where);
    }
    /* And the merges last, so a box is always measured against the final layout
     * however many lines moved before it. */
    for (i = 0; i < nmerge; i++) {
        int aligned = 0, inside = st_rect_cells(&t, merges[i].r, &aligned);
        if (!aligned) {
            app_logw(L"warning: --merge %d,%d,%d,%d does not line up with the "
                     L"slice edges, skipped",
                     merges[i].r.left, merges[i].r.top,
                     merges[i].r.right, merges[i].r.bottom);
            continue;
        }
        if (inside < 2) {
            app_logw(L"warning: --merge %d,%d,%d,%d covers %d cell(s), "
                     L"nothing to merge",
                     merges[i].r.left, merges[i].r.top,
                     merges[i].r.right, merges[i].r.bottom, inside);
            continue;
        }
        st_merge_rect(&t, merges[i].r);
    }

    total = st_count(&t);
    if (total > SLICE_MAX_CELLS) {
        app_logw(L"FAIL: %d slices exceeds the %d limit", total, SLICE_MAX_CELLS);
        st_free(&t);
        img_free(&s);
        return 2;
    }

    {
        Slice **lv = st_leaves(&t, &total);
        if (!lv) { st_free(&t); img_free(&s); return 1; }
        for (k = 0; k < total; k++) {
            wchar_t *name = st_slice_name(prefix, k, total, asJpeg);
            wchar_t path[MAX_PATH + 80];
            dir_join(path, MAX_PATH + 80, dir, name);
            xfree(name);
            if (img_save_rect(&s, lv[k]->r, path, asJpeg, quality)) wrote++;
        }
        xfree(lv);
    }
    _snwprintf_s(msg, 192, _TRUNCATE, L"RESULT: %d x %d image -> %d/%d slices in %s (%s)",
                 s.w, s.h, wrote, total, dir, asJpeg ? L"jpeg" : L"png");
    app_logw(L"%s", msg);
    st_free(&t);
    img_free(&s);
    return (wrote == total && wrote > 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* controls                                                            */
/* ------------------------------------------------------------------ */

static HFONT ui_font(void)
{
    int px = app_scale_px(13);
    if (g_sl.uiFont && g_sl.uiFontPx == px) return g_sl.uiFont;
    if (g_sl.uiFont && g_sl.uiFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(g_sl.uiFont);
    g_sl.uiFont = CreateFontW(-px, 0, 0, 0, 400, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!g_sl.uiFont) g_sl.uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_sl.uiFontPx = px;
    return g_sl.uiFont;
}

static HWND add_ctl(int id, const wchar_t *cls, const wchar_t *text,
                    DWORD style, int exStyle)
{
    HWND h = CreateWindowExW((DWORD)exStyle, cls, text,
                             WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, g_sl.hwnd,
                             (HMENU)(INT_PTR)id, g_sl.inst, NULL);
    if (h) {
        SendMessageW(h, WM_SETFONT, (WPARAM)ui_font(), TRUE);
        g_sl.ctl[id] = h;
    }
    return h;
}

static void create_controls(void)
{
    static const wchar_t *btns[] = {
        L"打开图片", L"浏览", L"导  出",
        L"-", L"+", L"-", L"+",
        L"按尺寸切", L"按行均分", L"按列均分", L"重置",
        L"横分", L"竖分", L"细分全部", L"删除格", L"合  并"
    };
    static const int ids[] = {
        IDC_OPEN, IDC_BROWSE, IDC_EXPORT,
        IDC_ROWS_MINUS, IDC_ROWS_PLUS, IDC_COLS_MINUS, IDC_COLS_PLUS,
        IDC_FIXED, IDC_EQ_ROWS, IDC_EQ_COLS, IDC_RESET,
        IDC_DIV_H, IDC_DIV_V, IDC_DIV_ALL, IDC_DEL, IDC_MERGE
    };
    size_t k;

    for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++)
        add_ctl(ids[k], L"BUTTON", btns[k], BS_PUSHBUTTON | WS_TABSTOP, 0);

    /* Added on its own rather than to the arrays above: it is not one of the
     * panel's rows of work, and its id is not adjacent to theirs. */
    add_ctl(IDC_ABOUT, L"BUTTON", L"关  于", BS_PUSHBUTTON | WS_TABSTOP, 0);

    add_ctl(IDC_ROWS, L"EDIT", L"1", ES_NUMBER | ES_CENTER | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add_ctl(IDC_COLS, L"EDIT", L"1", ES_NUMBER | ES_CENTER | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add_ctl(IDC_CELL, L"EDIT", L"256", ES_NUMBER | ES_CENTER | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add_ctl(IDC_DIVN, L"EDIT", L"2", ES_NUMBER | ES_CENTER | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add_ctl(IDC_QUALITY, L"EDIT", L"90", ES_NUMBER | ES_CENTER | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add_ctl(IDC_PREFIX, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);
    add_ctl(IDC_OUTDIR, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE);

    add_ctl(IDC_FORMAT, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0);
    if (g_sl.ctl[IDC_FORMAT]) {
        SendMessageW(g_sl.ctl[IDC_FORMAT], CB_ADDSTRING, 0, (LPARAM)L"PNG 保留透明");
        SendMessageW(g_sl.ctl[IDC_FORMAT], CB_ADDSTRING, 0, (LPARAM)L"JPEG 白底");
        SendMessageW(g_sl.ctl[IDC_FORMAT], CB_SETCURSEL, 0, 0);
    }
}

static void on_command(int id)
{
    /* The steppers adjust what is on screen, so a value typed into the edit
     * box first has to be taken back out of it - otherwise "+" on a box
     * reading "7" would step the remembered 1 and overwrite the 7. */
    switch (id) {
    case IDC_ROWS_MINUS: case IDC_ROWS_PLUS:
    case IDC_COLS_MINUS: case IDC_COLS_PLUS:
        sync_options();
        break;
    default:
        break;
    }

    switch (id) {
    case IDC_OPEN: do_open(); break;
    case IDC_BROWSE:
        if (pick_folder(g_sl.hwnd, g_sl.outDir, MAX_PATH)) {
            SetWindowTextW(g_sl.ctl[IDC_OUTDIR], g_sl.outDir);
            set_msg(L"已选择输出目录。", 0);
        }
        break;
    case IDC_EXPORT: do_export(); break;
    case IDC_ROWS_MINUS:
        if (g_sl.rowsN > 1) { g_sl.rowsN--; write_int(g_sl.ctl[IDC_ROWS], g_sl.rowsN); }
        break;
    case IDC_ROWS_PLUS:
        if (g_sl.rowsN < 512) { g_sl.rowsN++; write_int(g_sl.ctl[IDC_ROWS], g_sl.rowsN); }
        break;
    case IDC_COLS_MINUS:
        if (g_sl.colsN > 1) { g_sl.colsN--; write_int(g_sl.ctl[IDC_COLS], g_sl.colsN); }
        break;
    case IDC_COLS_PLUS:
        if (g_sl.colsN < 512) { g_sl.colsN++; write_int(g_sl.ctl[IDC_COLS], g_sl.colsN); }
        break;
    case IDC_EQ_ROWS: do_layout(0); break;
    case IDC_EQ_COLS: do_layout(1); break;
    case IDC_FIXED:   do_layout(2); break;
    case IDC_RESET:   do_layout(3); break;
    case IDC_DIV_H:   do_divide_axis(1); break;   /* a horizontal line -> rows    */
    case IDC_DIV_V:   do_divide_axis(0); break;   /* a vertical line   -> columns */
    case IDC_DIV_ALL: do_divide(1); break;
    case IDC_DEL:     do_delete_sel(); break;
    case IDC_MERGE:   do_merge(); break;
    case IDC_ABOUT:   app_show_about(g_sl.hwnd, SLICE_ABOUT_NAME,
                                     SLICE_ABOUT_NOTES); break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* window procedure                                                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK sl_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Slicer *sl = &g_sl;
    int x, y;

    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(h);
        return 0;

    case WM_GETMINMAXINFO: {
        /* The panel is a fixed column of controls, so there is a smallest
         * window that can hold all of it. Without this the user can drag the
         * frame smaller than the content and the bottom controls - the export
         * button above all - end up outside the client area. */
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = app_scale_px(SL_MIN_CLIENT_W);
        mm->ptMinTrackSize.y = app_scale_px(SL_MIN_CLIENT_H);
        return 0;
    }

    case WM_SIZE:
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_DPICHANGED: {
        const RECT *sug = (const RECT *)l;
        app_set_dpi_from_window(h);
        /* Every size in the panel comes from the scale, so the font has to be
         * rebuilt for the new one and the checkerboard discarded - otherwise
         * the controls sit at the old metric while everything around it
         * moves. */
        g_sl.uiFontPx = 0;
        ui_font();
        checker_release();
        SetWindowPos(h, NULL, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_SETCURSOR: {
        HCURSOR c = NULL;
        /* HTCLIENT is the only condition that matters. wParam carries a cursor
         * handle, and the older `w == IDC_ARROW` idiom compares that handle
         * against a MAKEINTRESOURCE value - a pointer-integer comparison that
         * means nothing. lParam is not coordinates either: its low word is the
         * hit-test code and its high word the mouse message number, so reading
         * GET_X/Y_LPARAM(l) handed in_view() a constant point whose membership
         * depended on the window size rather than on where the cursor is. The
         * cursor is read live: by the time this runs, it sits where the move
         * that triggered the message landed. */
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            if (GetCursorPos(&pt)) {
                int axis = 0, pos = 0, lo = 0, hi = 0, grab = 0;
                ScreenToClient(h, &pt);
                /* The line being dragged keeps its resize cursor even when the
                 * pointer has wandered off it, otherwise it flickers to an
                 * arrow the moment the user drags past the end of the run. */
                if (sl->gesture == GESTURE_LINE)
                    c = LoadCursorW(NULL, sl->grabAxis == 0 ? IDC_SIZEWE : IDC_SIZENS);
                else if (hit_line(pt.x, pt.y, &axis, &pos, &lo, &hi, &grab))
                    c = LoadCursorW(NULL, axis == 0 ? IDC_SIZEWE : IDC_SIZENS);
                else if (in_view(pt.x, pt.y))
                    c = LoadCursorW(NULL, IDC_CROSS);
            }
        }
        SetCursor(c ? c : LoadCursorW(NULL, IDC_ARROW));
        return 1;
    }

    case WM_MOUSEMOVE:
        x = GET_X_LPARAM(l);
        y = GET_Y_LPARAM(l);
        if (sl->gesture == GESTURE_LINE) {
            update_line_drag(x, y);
        } else if (sl->gesture == GESTURE_BAND) {
            sl->bandTo.x = x;
            sl->bandTo.y = y;
            InvalidateRect(h, NULL, FALSE);
        } else if (sl->down) {
            /* still deciding between a click and a drag */
            if (sl->gesture == GESTURE_NONE &&
                (abs(x - sl->downX) > SL_DRAG_SLOP || abs(y - sl->downY) > SL_DRAG_SLOP)) {
                if (in_view(sl->downX, sl->downY) && in_view(x, y)) {
                    sl->gesture = GESTURE_CUT;
                    sl->drawing = 1;
                    SetCapture(h);
                }
            }
            if (sl->drawing) update_line(x, y);
        } else {
            update_hover(x, y);
            update_line_hover(x, y);
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

    case WM_MOUSELEAVE:
        if (!sl->drawing && sl->hover) {
            sl->hover = NULL;
            InvalidateRect(h, NULL, FALSE);
        }
        /* The grip goes too: the pointer has left the window, so there is no
         * line under it any more. */
        if (!sl->down && sl->haveHover) {
            sl->haveHover = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN:
        sl->downX = GET_X_LPARAM(l);
        sl->downY = GET_Y_LPARAM(l);
        sl->down = 1;
        sl->drawing = 0;
        sl->gesture = GESTURE_NONE;
        /* Ctrl turns the left button into the band as well, so the gesture is
         * reachable from the button the hand is already on. */
        if ((GetKeyState(VK_CONTROL) & 0x8000) &&
            in_view(sl->downX, sl->downY)) {
            band_begin(sl->downX, sl->downY);
            sl->gesture = GESTURE_BAND;
            return 0;
        }
        /* A press that lands on a grid line is a gesture of its own, decided
         * at once rather than after a slop threshold: by the time a line has
         * been dragged far enough to count it is already visibly dragging, and
         * waiting would make it lag behind the pointer. */
        if (hit_line(sl->downX, sl->downY, &sl->grabAxis, &sl->grabPos,
                     &sl->grabLo, &sl->grabHi, &sl->grabOff)) {
            sl->gesture = GESTURE_LINE;
            sl->hoverPt.x = sl->downX;
            sl->hoverPt.y = sl->downY;
            SetCapture(h);
            if (st_line_target(&g_sl.tree, sl->grabAxis, sl->grabPos, sl->grabPos,
                               sl->grabLo, sl->grabHi,
                               &sl->dragTo, &sl->dragLo, &sl->dragHi)) {
                sl->lastAxis = sl->grabAxis;
                sl->lastPos  = sl->grabPos;
                sl->lastLo   = sl->dragLo;
                sl->lastHi   = sl->dragHi;
                sl->haveLast = 1;
            }
        }
        return 0;

    case WM_LBUTTONUP: {
        int moved = (abs(GET_X_LPARAM(l) - sl->downX) > SL_DRAG_SLOP ||
                     abs(GET_Y_LPARAM(l) - sl->downY) > SL_DRAG_SLOP);
        int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        sl->down = 0;

        if (sl->gesture == GESTURE_BAND) {
            sl->gesture = GESTURE_NONE;
            band_end(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        } else if (sl->gesture == GESTURE_LINE) {
            update_line_drag(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            sl->gesture = GESTURE_NONE;
            ReleaseCapture();
            apply_line_move();
        } else if (sl->drawing) {
            update_line(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            sl->drawing = 0;
            sl->gesture = GESTURE_NONE;
            ReleaseCapture();
            apply_draw();
        } else if (!moved && in_view(GET_X_LPARAM(l), GET_Y_LPARAM(l))) {
            /* a click, not a drag: pick the cell under the pointer so the 细分
             * and 合并 buttons have something to act on */
            Slice *c = st_leaf_at_point(&g_sl.tree,
                                        screen_to_img_x(GET_X_LPARAM(l)),
                                        screen_to_img_y(GET_Y_LPARAM(l)));
            sl->sel = c;
            sl->hover = c;
            if (c) {
                /* Shift spans the box between the cell the box was measured from
                 * and this one, which is how a block is picked out without a
                 * tool to switch to. The first Shift-click of a pair measures
                 * from the cell already picked, so there is no separate anchor
                 * state to keep. */
                if (shift && sl->selFrom.right > sl->selFrom.left) {
                    int l0 = (sl->selFrom.left   < c->r.left)   ? sl->selFrom.left   : c->r.left;
                    int t0 = (sl->selFrom.top    < c->r.top)    ? sl->selFrom.top    : c->r.top;
                    int r0 = (sl->selFrom.right  > c->r.right)  ? sl->selFrom.right  : c->r.right;
                    int b0 = (sl->selFrom.bottom > c->r.bottom) ? sl->selFrom.bottom : c->r.bottom;
                    SetRect(&sl->selRect, l0, t0, r0, b0);
                    sl->selWide = (l0 != c->r.left || t0 != c->r.top ||
                                   r0 != c->r.right || b0 != c->r.bottom);
                } else {
                    sl->selFrom = c->r;
                    sl->selRect = c->r;
                    sl->selWide = 0;
                }
            } else {
                sl->selWide = 0;
                SetRect(&sl->selRect, 0, 0, 0, 0);
                SetRect(&sl->selFrom, 0, 0, 0, 0);
            }
            InvalidateRect(h, NULL, FALSE);
        }
        sl->downX = sl->downY = 0;
        return 0;
    }

    case WM_RBUTTONDOWN:
        /* The rubber band, on the button the left hand is not on. Left-drag
         * already means "pick a cell" and "draw a cut", and merging is
         * impossible without picking more than one cell - so it cannot be left
         * to a modifier key nobody will find. Started only over the picture: a
         * band begun on the grey margin has nothing to snap to. */
        if (in_view(GET_X_LPARAM(l), GET_Y_LPARAM(l))) {
            band_begin(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            sl->gesture = GESTURE_BAND;
        }
        return 0;

    case WM_RBUTTONUP:
        if (sl->gesture == GESTURE_BAND) {
            sl->gesture = GESTURE_NONE;
            band_end(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        }
        return 0;

    case WM_CAPTURECHANGED:
        /* the drag ended somewhere else - a modal menu, an alt-tab */
        sl->drawing = 0;
        sl->down = 0;
        sl->bandDown = 0;
        sl->gesture = GESTURE_NONE;
        sl->downX = sl->downY = 0;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_KEYDOWN: {
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (w == 'O' || w == 'o')) { do_open(); return 0; }
        if (ctrl && (w == 'E' || w == 'e')) { do_export(); return 0; }
        if (ctrl && (w == 'S' || w == 's')) { do_export(); return 0; }
        if (ctrl && (w == 'A' || w == 'a')) { do_select_all(); return 0; }
        if (w == VK_SUBTRACT || w == VK_OEM_MINUS) {
            sync_options();
            if (g_sl.divN > 2) { g_sl.divN--; write_int(g_sl.ctl[IDC_DIVN], g_sl.divN); }
            return 0;
        }
        if (w == VK_ADD || w == VK_OEM_PLUS) {
            sync_options();
            if (g_sl.divN < 64) { g_sl.divN++; write_int(g_sl.ctl[IDC_DIVN], g_sl.divN); }
            return 0;
        }
        if (w == 'S' || w == 's') { do_divide(1); return 0; }
        if (w == 'M' || w == 'm') { do_merge(); return 0; }
        /* Only when the canvas has the focus: a caret in the prefix field has
         * to be able to delete a character with the same key, and the arrow keys
         * have to still move a caret. */
        if (GetFocus() == h) {
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int step = shift ? 10 : 1;
            /* The arrow keys nudge the line the pointer touched last, one pixel
             * at a time. Dragging is coarse and a grid line often has to land
             * on an exact column, so this is the only way to get there. Only
             * the keys that fit the line's axis do anything, so the canvas does
             * not feel like it is eating input. */
            if (sl->haveLast && w == VK_LEFT  && sl->lastAxis == 0) goto nudge;
            if (sl->haveLast && w == VK_RIGHT && sl->lastAxis == 0) goto nudge;
            if (sl->haveLast && w == VK_UP    && sl->lastAxis == 1) goto nudge;
            if (sl->haveLast && w == VK_DOWN  && sl->lastAxis == 1) goto nudge;
            if (w == VK_DELETE || w == VK_BACK) {
                do_delete_sel();
                return 0;
            }
            goto nodone;
        nudge:
            /* The step is signed by which way the key points along the axis. */
            step = (w == VK_LEFT || w == VK_UP) ? -step : step;
            if (nudge_last_line(step)) {
                _snwprintf_s(g_sl.msg, 192, _TRUNCATE,
                             sl->lastAxis == 0 ? L"竖线移到 x = %d。" : L"横线移到 y = %d。",
                             sl->lastPos);
                g_sl.msgIsError = 0;
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        nodone: ;
        }
        if (w == VK_ESCAPE) {
            /* Esc first gives up a drag in progress, then the selection - the
             * order a user expects, since a drag in progress is the thing in
             * their hand. */
            if (sl->gesture == GESTURE_LINE || sl->drawing || sl->bandDown) {
                sl->gesture = GESTURE_NONE;
                sl->drawing = 0;
                sl->bandDown = 0;
                sl->down = 0;
                if (GetCapture() == h) ReleaseCapture();
            } else if (sl->sel || sl->selWide) {
                clear_selection();
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        /* A tray-menu command arrives with lParam == 0. Anything with an
         * lParam came from a child control, and the two kinds have to be told
         * apart by the notification code rather than by lParam alone: the edit
         * boxes fire EN_CHANGE on every programmatic SetWindowText, which must
         * not read as a press, while the buttons fire BN_CLICKED - and that is
         * the only thing any of the panel buttons ever send, so treating them
         * all as edit noise is what used to leave the whole panel dead. */
        if (l != 0) {
            if (code == EN_KILLFOCUS) {
                if (id == IDC_ROWS)   { sync_options(); write_int(g_sl.ctl[IDC_ROWS], g_sl.rowsN); }
                if (id == IDC_COLS)   { sync_options(); write_int(g_sl.ctl[IDC_COLS], g_sl.colsN); }
                if (id == IDC_DIVN)   { sync_options(); write_int(g_sl.ctl[IDC_DIVN], g_sl.divN); }
                if (id == IDC_QUALITY) { sync_options(); }
            } else if (code == BN_CLICKED) {
                on_command(id);
            }
            return 0;
        }
        return 0;
    }

    case WM_HOTKEY:
        if (w == HOTKEY_ID) { do_open(); return 0; }
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        sl->quit = 1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    LPWSTR *argv = NULL;
    int argc = 0, i, hotkeys = 0, rc = 0;
    wchar_t openOnStart[MAX_PATH];

    (void)prev; (void)show; (void)cmd;
    app_attach_parent_console();
    app_set_dpi_aware();
    app_install_crash_log(L"slice");
    app_set_verbose(0);

    memset(&g_sl, 0, sizeof(g_sl));
    g_sl.inst = inst;
    g_sl.rowsN = 1;
    g_sl.colsN = 1;
    g_sl.cellPx = 256;
    g_sl.divN = 2;
    g_sl.quality = 90;
    st_init(&g_sl.tree, 1, 1);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) argc = 0;
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--selftest") == 0) {
            if (argv) LocalFree(argv);
            return slice_selftest();
        }
        if (_wcsicmp(argv[i], L"--about") == 0) {
            /* No owner: this one was asked for on a command line, so there is
             * no window to centre on and nothing to disable while it is up. */
            app_show_about(NULL, SLICE_ABOUT_NAME, SLICE_ABOUT_NOTES);
            if (argv) LocalFree(argv);
            return 0;
        }
        if (_wcsicmp(argv[i], L"--verbose") == 0) app_set_verbose(1);
    }
    if (argc > 1 && _wcsicmp(argv[1], L"--split") == 0) {
        if (!gdiplus_start()) {
            if (argv) LocalFree(argv);
            return 1;
        }
        rc = run_batch(argc, argv);
        gdiplus_stop();
        if (argv) LocalFree(argv);
        return rc;
    }
    /* Anything that is not a switch is the file to open, which is what makes
     * "open with" on a .png work. */
    openOnStart[0] = 0;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == L'-') continue;
        wcsncpy(openOnStart, argv[i], MAX_PATH - 1);
        openOnStart[MAX_PATH - 1] = 0;
        break;
    }
    if (argv) LocalFree(argv);

    if (!gdiplus_start()) {
        MessageBoxW(NULL, L"无法初始化 GDI+，程序无法启动。", L"MuskSlice",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GdipCreateFontFamilyFromName(L"Microsoft YaHei UI", NULL, &g_sl.fam) != Ok)
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &g_sl.fam);
    if (GdipCreateFontFamilyFromName(L"Consolas", NULL, &g_sl.mono) != Ok)
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &g_sl.mono);

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = sl_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = SL_CLASS;
    /* The logo build.ps1 embeds as res\logo-slice.ico - the same artwork the
     * .exe shows in Explorer. */
    wc.hIcon         = app_icon_load(inst, 0);
    wc.hIconSm       = app_icon_load(inst, GetSystemMetrics(SM_CXSMICON));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    {
        int wx, wy, ww, wh;
        app_fit_window(app_scale_px(1020), app_scale_px(760), &wx, &wy, &ww, &wh);
        g_sl.hwnd = CreateWindowExW(0, SL_CLASS, L"MuskSlice  图片分割器",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                    WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME |
                                    WS_CLIPCHILDREN,
                                    wx, wy, ww, wh, NULL, NULL, inst, NULL);
    }
    if (!g_sl.hwnd) return 1;
    app_set_dpi_from_window(g_sl.hwnd);

    create_controls();
    {
        RECT cr;
        GetClientRect(g_sl.hwnd, &cr);
        build_layout(cr.right, cr.bottom);
    }

    /* No tray icon. The notification area belongs to the screenshot and the
     * colour picker, which hide themselves after a capture and need somewhere
     * to be brought back from; this one keeps a window of its own. The hot
     * key is registered directly rather than as a side effect of the icon
     * appearing - with no icon it is the only way to raise a running copy. */
    if (!RegisterHotKey(g_sl.hwnd, HOTKEY_ID,
                        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S')) {
        if (!RegisterHotKey(g_sl.hwnd, HOTKEY_ID,
                            MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'S'))
            hotkeys = 1;
    }

    if (openOnStart[0]) load_into_app(openOnStart);

    ShowWindow(g_sl.hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_sl.hwnd);

    while (!g_sl.quit) {
        if (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            break;
        }
    }

    UnregisterHotKey(g_sl.hwnd, HOTKEY_ID);
    /* The balloon that used to carry this warning went with the tray icon, and
     * it fired after the window was already gone - which is a poor time to
     * raise a dialog. Say it once, here, while there is still a window. */
    if (hotkeys)
        MessageBoxW(NULL, L"全局快捷键 Ctrl+Alt+S / Ctrl+Shift+S 都被占用了，\r\n"
                          L"只能从窗口打开。", L"MuskSlice", MB_OK | MB_ICONWARNING);
    checker_release();
    img_free(&g_sl.src);
    st_free(&g_sl.tree);
    if (g_sl.mem) GdipDisposeImage((GpImage *)g_sl.mem);
    if (g_sl.memBmp) free_dib(g_sl.memBmp, g_sl.memBits);
    if (g_sl.uiFont && g_sl.uiFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(g_sl.uiFont);
    if (g_sl.fam) GdipDeleteFontFamily(g_sl.fam);
    if (g_sl.mono) GdipDeleteFontFamily(g_sl.mono);
    DestroyWindow(g_sl.hwnd);
    UnregisterClassW(SL_CLASS, inst);
    gdiplus_stop();
    return 0;
}