/* shot.h - screen capture + annotation tool ("Musk scribble" screenshot). */
#ifndef SHOT_H
#define SHOT_H

#include "../common/winutil.h"
#include "../common/tray.h"
#include "../common/trayicon.h"
#include "../common/uikit.h"
#include <gdiplus.h>

typedef enum {
    T_PEN = 0,      /* freehand - the signature rough scribble          */
    T_ERASER,
    T_LINE,
    T_RECT,
    T_ELLIPSE,
    T_ARROW,
    T_TEXT,
    T_MOSAIC,
    T_BLUR,
    T_FILL,         /* solid block, for redacting                        */
    T_COUNT
} ToolKind;

typedef struct Stroke {
    ToolKind kind;
    ARGB     color;
    int      width;
    int      filled;
    int      block;        /* mosaic block size, in px                    */
    int      fontSize;
    RECT     r;            /* canvas-space bounds                         */
    POINT   *pts;          /* freehand samples (canvas space)             */
    int      npts, cpts;
    wchar_t *text;
    void    *penCache;   /* borrowed GpPen used only while rendering */
} Stroke;

void stroke_free(Stroke *s);
void stroke_add_point(Stroke *s, int x, int y);

/* One toolbar slot. */
typedef struct {
    int      id;           /* TB_* below, or -1 for a separator            */
    RECT     hit;          /* virtual-screen coords                        */
    int      colorIndex;   /* >=0 for a colour swatch                     */
    wchar_t  tip[64];
} TbItem;

enum {
    TB_PEN = 1, TB_ERASER, TB_LINE, TB_RECT, TB_ELLIPSE, TB_ARROW,
    TB_TEXT, TB_MOSAIC, TB_BLUR, TB_FILL,
    TB_UNDO, TB_REDO, TB_FILLMODE, TB_THICKER, TB_THINNER,
    TB_COPY, TB_SAVE, TB_DONE, TB_CANCEL,
    TB_SWATCH0 = 100
};

enum {
    IDM_TRAY_CAPTURE = 1001,
    IDM_TRAY_DELAY3  = 1002,
    IDM_TRAY_DELAY5  = 1003,
    IDM_TRAY_ABOUT   = 1005,
    IDM_TRAY_EXIT    = 1006
};

/* The global capture shortcut, and what happens when another program already
 * owns the combination. See hotkey.c. */
#define HOTKEY_SLOTS 8        /* ids 1..HOTKEY_SLOTS, any may be in force */

typedef struct {
    UINT mods;   /* MOD_* flags */
    UINT vk;     /* virtual-key code */
} Hotkey;

/* How many window rectangles a drag can snap against. A desktop with more than
 * this many visible windows on it has more than anybody can align to anyway,
 * and the array is fixed so the drag never allocates. */
#define MAX_EDGES 96

/* How near a window edge a dragged corner has to be before the edge takes it,
 * at 100% scaling; the overlay scales this like everything else. Wide enough
 * to catch a near miss by hand, narrow enough that it never grabs a corner the
 * user was clearly aiming elsewhere. Shared with the self-test so the threshold
 * is checked against the same number the code uses. */
#define SNAP_PX 7

typedef struct ShotApp {
    HINSTANCE inst;
    HWND      hwnd;          /* hidden message window            */
    HWND      overlay;       /* full virtual-screen editor window */
    int       capturing;     /* shot_begin is on the stack        */
    Tray      tray;
    Hotkey    hotkey;        /* the combination actually in force */
    int       hotkeyOk;      /* at least one shortcut registered  */
    int       quit;

    /* virtual screen geometry, screen coords */
    int vsx, vsy, vsw, vsh;

    /* the frozen screen capture, virtual-screen sized 32bpp DIB */
    HBITMAP   screenBmp;
    void     *screenBits;
    HDC       screenDC;      /* screenBmp selected, for BitBlt sources   */
    GpBitmap *screenGp;      /* GDI+ view over screenBits        */

    /* the selection cropped out of screenBmp, for mosaic/blur sampling */
    HBITMAP   baseBmp;
    void     *baseBits;
    GpBitmap *baseGp;         /* GDI+ view over baseBmp, kept across paints */
    int       baseW, baseH;

    /* current selection, in virtual-screen coords */
    RECT      sel;
    int       haveSel;
    int       selecting;     /* still choosing a new region   */
    int       selDrag;       /* button is down, region is growing */

    /* click-to-pick: before a drag starts the press is undecided, and a
     * release without movement takes the window under the cursor instead */
    int       btnDown;       /* press held, drag vs click undecided */
    int       downX, downY;  /* where the press landed              */

    /* the window a click would take, tracked while hovering */
    RECT      hoverRect;
    int       hoverValid;

    /* annotation state */
    GpBitmap *layer;         /* selection-sized 32bppPARGB       */
    void     *layerBits;     /* the pixels behind `layer`, ours  */
    Stroke  **strokes;
    int       nstrokes, cstrokes, cursor;   /* cursor = undo boundary */
    ToolKind  tool;
    ARGB      color;
    int       width;
    int       fillMode;
    int       mosaicBlock;
    int       fontSize;
    int       dragging;      /* a stroke is in progress          */
    Stroke   *active;        /* in-progress stroke, not in list  */

    /* magnifier (selection mode only) */
    int       magX, magY, magSel;
    int       magnify;

    /* edge snapping while a region is being dragged */
    RECT      edge[MAX_EDGES];  /* windows a selection can line up with  */
    int       nedge;
    int       snapX, snapY;     /* guide lines, -1 when nothing is snapped */

    /* toolbar */
    TbItem   *tb;
    int       ntb, ctb;
    RECT      tbRect;        /* virtual-screen coords             */
    int       tbHot;         /* index under cursor, -1 none      */
    int       tbDown;        /* index pressed                     */
    DWORD     tipSince;      /* GetTickCount when tbHot last moved */
    int       tipShown;      /* the bubble currently on screen, -1 none */

    /* back buffer for painting, virtual-screen sized */
    HBITMAP   compBmp;
    void     *compBits;
    GpBitmap *compGp;

    GpFontFamily *fontFamily;
} ShotApp;

/* surface.c */
int   shot_capture_screen(ShotApp *a);
void  shot_free_surface(ShotApp *a);
int   shot_ensure_layer(ShotApp *a);
void  shot_free_layer(ShotApp *a);
int   shot_ensure_base(ShotApp *a);
void  shot_clear_strokes(ShotApp *a);
void  shot_undo(ShotApp *a);
void  shot_redo(ShotApp *a);
void  shot_clear_selection(ShotApp *a);

/* draw.c */
void  draw_init(ShotApp *a);
void  draw_shutdown(ShotApp *a);
int   render_layer(ShotApp *a, Stroke *inProgress);
int   draw_stroke(ShotApp *a, GpGraphics *g, Stroke *s);
void  draw_toolbar(ShotApp *a, GpGraphics *g);
int   draw_toolbar_tip(ShotApp *a, GpGraphics *g);
void  draw_selection_guides(ShotApp *a, GpGraphics *g);
int   hit_toolbar(const ShotApp *a, int sx, int sy);
void  layout_toolbar(ShotApp *a);

/* chrome.c - shared by editor.c for its HUD drawing */
ARGB shot_palette(int index);
int   shot_palette_count(void);
int   shot_text_width(ShotApp *a, const wchar_t *s, int px);
void  draw_hud_text(ShotApp *a, GpGraphics *g, const wchar_t *s,
                    int x, int y, int w, int h, ARGB color, int px);
ARGB  shot_dim_color(void);

/* output.c */
int   shot_copy_to_clipboard(ShotApp *a);
int   shot_save_to_file(ShotApp *a, const wchar_t *path);
int   shot_finish_and_copy(ShotApp *a);
wchar_t *shot_default_filename(void);
wchar_t *shot_temp_png_path(void);
void  shot_cleanup_temp(void);
/* The CF_HTML byte stream put_html hands to the clipboard, split out so the
 * self-test can parse its offsets back out. Caller xfree()s *bytesOut. */
int   shot_build_cfhtml(const wchar_t *imgPath, int w, int h,
                        char **bytesOut, size_t *lenOut);

/* main.c */
int   shot_selftest(void);
void  shot_notify(ShotApp *a, const wchar_t *msg);

/* hotkey.c - the global capture shortcut and its conflict handling */
wchar_t *hotkey_text(const Hotkey *hk);            /* caller xfree()s */
int      hotkey_parse(const wchar_t *s, Hotkey *out);
void     hotkey_default_of(Hotkey *out);           /* the owner's default */
int      hotkey_available(const Hotkey *hk);
int      hotkey_register(HWND hwnd, Hotkey *out, Hotkey *refused);
void     hotkey_store(const Hotkey *hk);
void     hotkey_unregister(HWND hwnd);
void     hotkey_note_claim(HWND hwnd, int id, const Hotkey *hk);
void     hotkey_conflict_hint(const Hotkey *want, wchar_t *out, int cch);
int      hotkey_dialog(HWND owner, const Hotkey *cur, Hotkey *out);

/* editor.c */
int   shot_begin(ShotApp *a, int delayMs);
/* Edge snapping. Exported so the self-test can drive it against a fixed set of
 * rectangles: the real path fills a->edge from live windows, which a self-test
 * on a console cannot do for itself. */
void  shot_snap_collect(ShotApp *a);
void  shot_snap_apply(ShotApp *a, int *x, int *y);
/* The frontmost window whose rectangle contains the screen point, clipped to
 * the virtual screen. skipA/skipB name windows to step over - the overlay
 * itself covers the screen while a capture is up, and the self-test drives
 * this with windows of its own process, where there is no overlay. */
int   shot_window_under_cursor(int sx, int sy, HWND skipA, HWND skipB, RECT *out);
void  shot_cancel(ShotApp *a);
LRESULT shot_overlay_proc(ShotApp *a, HWND h, UINT msg, WPARAM wp, LPARAM lp);
int   shot_text_input(HWND owner, wchar_t **outText, int *outSize);
int   shot_is_running(ShotApp *a);

#endif /* SHOT_H */
