/* main.c - tray icon, global hotkey, startup registration, self-test. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"
#include <gdiplus.h>

#define TIMER_TRAY  1

/* This program's fill for logo.svg: the violet shot.exe has always used, and
 * the same violet res\logo-shot.ico is filled with, so the tray icon and the
 * .exe icon can never disagree. */
#define TRAY_ICON_COLOR APP_ICON_COLOR_SHOT

/* Menu ids. Kept well clear of any child-control id space: a menu command and
 * a child notification both arrive as WM_COMMAND, and only lParam (0 for a
 * menu) tells them apart. */
enum {
    MI_CAPTURE = 40001, MI_DELAY3 = 40002, MI_DELAY5 = 40003,
    MI_HOTKEYS = 40005, MI_ABOUT = 40006, MI_EXIT = 40007,
    MI_SETKEY = 40008, MI_HKSTATUS = 40009
};

static ShotApp g_app;

/* Scratch strings for the tray menu, which has to show the shortcut currently
 * in force - and that only exists as text at menu-build time. */
static wchar_t g_menuHotkey[128];
static wchar_t g_menuHotkeyItem[160];

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

void shot_notify(ShotApp *a, const wchar_t *msg)
{
    if (a) tray_balloon(&a->tray, L"截图工具", msg, NIIF_INFO);
}

/* The About box. The name, the icon and the credit line come from
 * src\common\about.c, so all four programs say the same thing about who wrote
 * them; only the notes below are shot's own. */
static void show_about(HWND owner)
{
    wchar_t path[MAX_PATH];
    wchar_t msg[1024];
    wchar_t *hk = hotkey_text(&g_app.hotkey);
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) path[0] = 0;
    _snwprintf_s(msg, 1024, _TRUNCATE,
        L"全局快捷键   %s\r\n"
        L"选取         点击窗口直接选 · 拖动可自由框选 · Esc 取消\r\n"
        L"涂抹         P 画笔 · L 直线 · A 箭头 · R 矩形 · O 椭圆\r\n"
        L"             T 文字 · M 马赛克 · B 模糊 · F 遮挡 · E 橡皮\r\n"
        L"颜色         数字键 1-9\r\n"
        L"粗细         [ 变细 · ] 加粗 · Ctrl+滚轮\r\n"
        L"编辑         Ctrl+Z 撤销 · Ctrl+Y 重做\r\n"
        L"输出         Ctrl+C 复制 · Ctrl+S 保存 · Enter 完成 · Esc 取消\r\n"
        L"\r\n"
        L"工具栏每个按钮悬停都有说明气泡；快捷键被占用时可在托盘菜单里改。\r\n"
        L"\r\n%s", hk ? hk : L"(未设置，请从托盘菜单设置)", path);
    if (hk) xfree(hk);
    app_show_about(owner, L"MuskShot  ·  截图涂抹工具", msg);
}

static void show_hotkeys(HWND owner)
{
    wchar_t msg[768];
    wchar_t *hk = hotkey_text(&g_app.hotkey);
    _snwprintf_s(msg, 768, _TRUNCATE,
        L"当前全局截图快捷键：%s\r\n\r\n"
        L"默认是 Ctrl+Shift+A。\r\n"
        L"如果按了没有反应，通常是被别的程序先注册走了——\r\n"
        L"程序启动时发现它被占用，会直接弹出设置窗口让你换一个，\r\n"
        L"点下面的「修改快捷键」也能随时改。窗口里会实时显示你按下的组合，\r\n"
        L"并直接告诉你它有没有被占用，确认可用再确定。\r\n\r\n"
        L"定下来的组合会被记住，下次启动继续用。",
        hk ? hk : L"(未设置)");
    if (hk) xfree(hk);
    MessageBoxW(owner, msg, L"快捷键", MB_OK | MB_ICONINFORMATION);
}

/* The tray menu's "modify shortcut" path. Returns TRUE if a new combination
 * was accepted, so the caller can re-register and refresh the icon tip. */
static int edit_hotkey(void)
{
    Hotkey want;
    if (!hotkey_dialog(g_app.hwnd, &g_app.hotkey, &want)) return 0;
    /* Free the old one first, or the dialog's suggestion is already ours and
     * RegisterHotKey would fail against our own registration. */
    hotkey_unregister(g_app.hwnd);
    if (!RegisterHotKey(g_app.hwnd, 1, want.mods | MOD_NOREPEAT, want.vk)) {
        wchar_t *t = hotkey_text(&want);
        tray_balloon(&g_app.tray, L"MuskShot",
                     t ? L"这个快捷键刚刚被别的程序抢走了，请再试一个"
                       : L"设置失败", NIIF_WARNING);
        if (t) xfree(t);
        /* Put something back so the app is never left with no shortcut. */
        g_app.hotkeyOk = hotkey_register(g_app.hwnd, &g_app.hotkey, NULL) != 0;
        return 0;
    }
    g_app.hotkey = want;
    g_app.hotkeyOk = 1;
    /* Tell the hotkey module about the claim made two lines above, so that
     * opening the dialog again and keeping this combination is recognised as
     * the one already in force rather than as a conflict with ourselves. */
    hotkey_note_claim(g_app.hwnd, 1, &want);
    hotkey_store(&want);
    {
        wchar_t *t = hotkey_text(&want);
        if (t) {
            wchar_t tip[160];
            _snwprintf_s(tip, 160, _TRUNCATE, L"MuskShot 截图涂抹  —  %s", t);
            tray_set_tip(&g_app.tray, tip);
            tray_balloon(&g_app.tray, L"MuskShot", t, NIIF_INFO);
            xfree(t);
        }
    }
    return 1;
}

static void do_capture(ShotApp *a, int delayMs)
{
    if (shot_is_running(a)) return;
    shot_begin(a, delayMs);
}

/* Fills g_menuHotkeyItem with the "current shortcut" row, and returns the item
 * table for the popup. Rebuilt on every right-click so it can never show a
 * combination that is no longer the one in force. */
static const wchar_t **build_menu_items(int *count)
{
    static const wchar_t *items[12];
    int n = 0;
    wchar_t *hk = hotkey_text(&g_app.hotkey);

    if (g_app.hotkeyOk && hk)
        _snwprintf_s(g_menuHotkeyItem, 160, _TRUNCATE,
                     L"快捷键：%s\t%d", hk, MI_HKSTATUS);
    else
        _snwprintf_s(g_menuHotkeyItem, 160, _TRUNCATE,
                     L"快捷键：未生效\t%d", MI_HKSTATUS);

    items[n++] = L"截图标注\t40001";
    items[n++] = L"延时 3 秒截图\t40002";
    items[n++] = L"延时 5 秒截图\t40003";
    items[n++] = L"-\t0";
    items[n++] = g_menuHotkeyItem;
    items[n++] = L"设置快捷键…\t40008";
    items[n++] = L"快捷键说明\t40005";
    items[n++] = L"-\t0";
    items[n++] = L"关于\t40006";
    items[n++] = L"-\t0";
    items[n++] = L"退出\t40007";
    (void)g_menuHotkey;

    if (hk) xfree(hk);
    *count = n;
    return items;
}

/* ------------------------------------------------------------------ */
/* host window                                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    ShotApp *a = &g_app;

    /* Explorer restarts and drops every tray icon; re-add ours. */
    if (a->tray.taskbarRebuilt && m == a->tray.taskbarRebuilt) {
        tray_recreate(&a->tray);
        return 0;
    }

    switch (m) {
    case WM_HOTKEY:
        /* While a tray menu tracks, its modal loop would wrap this capture:
         * the overlay then lives inside TrackPopupMenu and the menu pops
         * back out over whatever the user did next. */
        if (tray_menu_up(&a->tray)) return 0;
        if (w >= 1 && w <= HOTKEY_SLOTS) do_capture(a, 0);
        return 0;

    case WM_TIMER:
        /* Explorer drops every notification icon when it restarts and does not
         * always send TaskbarCreated to a window that is not foreground. NIM_ADD
         * on an id that is already present just refreshes it, so re-asserting
         * periodically is a cheap way to make the tray survive that. */
        if (w == TIMER_TRAY && a->tray.visible) {
            NOTIFYICONDATAW probe = a->tray.nid;
            probe.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
            if (!Shell_NotifyIconW(NIM_MODIFY, &probe)) tray_recreate(&a->tray);
        }
        return 0;

    case WM_COMMAND:
        /* lParam is 0 for a menu command and the child HWND for a control
         * notification; only act on the former. */
        if (l != 0) break;
        switch (LOWORD(w)) {
        case MI_CAPTURE:   do_capture(a, 0); break;
        case MI_DELAY3:    do_capture(a, 3000); break;
        case MI_DELAY5:    do_capture(a, 5000); break;
        case MI_HOTKEYS:   show_hotkeys(a->hwnd); break;
        case MI_SETKEY:    edit_hotkey(); break;
        case MI_HKSTATUS:  break;   /* status row: greyed out, never fires */
        case MI_ABOUT:     show_about(a->hwnd); break;
        case MI_EXIT:      a->quit = 1; break;
        default: break;
        }
        return 0;

    case WM_APP + 1:   /* tray callback */
        /* A menu is already tracking from a previous callback: with
         * NOTIFYICON_VERSION_4 one physical right-click delivers both
         * WM_RBUTTONUP and WM_CONTEXTMENU, and a second click during
         * tracking re-enters this handler - it used to destroy the menu
         * under TrackPopupMenu and rebuild, which is the
         * click-the-tray-a-few-times-and-it-dies report. Drop the extras. */
        if (tray_menu_up(&a->tray)) return 0;
        if (LOWORD(l) == WM_LBUTTONUP) {
            app_logw(L"tray: capture");
            do_capture(a, 0);
            return 0;
        }
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            HMENU mnu;
            UINT cmd;
            POINT pt;
            int count = 0;
            const wchar_t **items = build_menu_items(&count);
            mnu = tray_build_menu(&a->tray, count, items);
            if (!mnu) return 0;
            /* the status row reports, it is not a command */
            EnableMenuItem(mnu, MI_HKSTATUS, MF_BYCOMMAND | MF_GRAYED);
            GetCursorPos(&pt);
            app_logw(L"tray: menu");
            tray_menu_set_up(&a->tray, 1);
            SetForegroundWindow(h);
            cmd = (UINT)TrackPopupMenu(mnu,
                          TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                          pt.x, pt.y, 0, h, NULL);
            tray_menu_set_up(&a->tray, 0);
            PostMessageW(h, WM_NULL, 0, 0);
            if (cmd) SendMessageW(h, WM_COMMAND, (WPARAM)cmd, 0);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        a->quit = 1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* self-test                                                           */
/* ------------------------------------------------------------------ */

static int check(int cond, const char *what)
{
    app_log("  [%s] %s", cond ? "pass" : "FAIL", what);
    return cond ? 0 : 1;
}

/* Compares two snapshots and logs the deltas, so a failure is diagnosable. */
static int check_res(const ResSnapshot *b, const ResSnapshot *a, const char *what)
{
    int ok = (a->gdiObjects == b->gdiObjects &&
              a->userObjects == b->userObjects &&
              a->handles == b->handles &&
              a->allocs == b->allocs);
    if (!ok)
        app_log("        delta gdi=%+ld user=%+ld handle=%+ld heap=%+ld",
                (long)a->gdiObjects - (long)b->gdiObjects,
                (long)a->userObjects - (long)b->userObjects,
                (long)a->handles - (long)b->handles,
                a->allocs - b->allocs);
    return check(ok, what);
}

int shot_selftest(void)
{
    ResSnapshot base, after;
    int fails = 0;
    int i;

    app_set_verbose(1);
    app_log("MuskShot self-test");

    if (!gdiplus_start()) { app_log("GDI+ start failed"); return 2; }
    draw_init(&g_app);

    /* palette ------------------------------------------------------------ */
    fails += check(shot_palette_count() == 10, "palette has 10 entries");
    fails += check(shot_palette(0) != 0 && shot_palette(999) == 0,
                   "palette index bounds are safe");

    /* toolbar layout churn ---------------------------------------------- */
    /* warm-up first: the very first pass allocates the persistent toolbar
     * array, which legitimately stays alive for the life of the app. */
    g_app.vsx = 0; g_app.vsy = 0; g_app.vsw = 1920; g_app.vsh = 1080;
    SetRect(&g_app.sel, 0, 0, 800, 600);
    g_app.haveSel = 1;
    layout_toolbar(&g_app);
    res_snapshot(&base);
    for (i = 0; i < 300; i++) {
        g_app.vsw = 400 + (i % 9) * 300;
        g_app.vsh = 300 + (i % 7) * 200;
        SetRect(&g_app.sel, 0, 0, 30 + (i % 13) * 40, 30 + (i % 11) * 30);
        layout_toolbar(&g_app);
    }
    res_snapshot(&after);
    fails += check_res(&base, &after, "layout_toolbar x300 leaks nothing");

    /* stroke alloc churn ------------------------------------------------- */
    res_snapshot(&base);
    for (i = 0; i < 500; i++) {
        Stroke *s = (Stroke *)xcalloc(1, sizeof(Stroke));
        int k;
        if (!s) break;
        s->kind = T_PEN;
        for (k = 0; k < 200; k++) stroke_add_point(s, k, k);
        s->text = xwcsdup(L"测试文字 hello");
        stroke_free(s);
    }
    res_snapshot(&after);
    fails += check(after.allocs == base.allocs, "stroke alloc/free x500 balances");

    /* render every tool, repeatedly -------------------------------------- */
    /* GDI+ allocates font/text caches on first use, so run one warm-up round
     * before taking the baseline - otherwise that cache looks like a leak. */
    for (i = 0; i < 2; i++) {
        ShotApp t;
        int k;
        memset(&t, 0, sizeof(t));
        t.fontFamily = g_app.fontFamily;
        t.baseW = 200; t.baseH = 120;
        t.baseBits = xcalloc((size_t)(t.baseW * t.baseH), 4);
        if (!t.baseBits) break;
        SetRect(&t.sel, 0, 0, t.baseW, t.baseH);
        t.haveSel = 1;
        if (!shot_ensure_layer(&t)) { xfree(t.baseBits); break; }
        for (k = 0; k < T_COUNT; k++) {
            Stroke *s = (Stroke *)xcalloc(1, sizeof(Stroke));
            if (!s) break;
            s->kind = (ToolKind)k;
            s->color = 0xFFFF0000;
            s->width = 3; s->block = 8; s->fontSize = 18;
            SetRect(&s->r, 10, 10, 180, 100);
            s->text = xwcsdup(L"标注 Aa");
            stroke_add_point(s, 12, 12);
            stroke_add_point(s, 170, 95);
            if (t.nstrokes == t.cstrokes) {
                int cap = t.cstrokes ? t.cstrokes * 2 : 16;
                Stroke **ns = (Stroke **)xrealloc(t.strokes,
                                    (size_t)cap * sizeof(Stroke *));
                if (!ns) { stroke_free(s); break; }
                t.strokes = ns; t.cstrokes = cap;
            }
            t.strokes[t.nstrokes++] = s;
        }
        t.cursor = t.nstrokes;
        render_layer(&t, NULL);
        xfree(t.baseBits); t.baseBits = NULL;
        shot_clear_strokes(&t);
            shot_free_layer(&t);
    }
    res_snapshot(&base);
    for (i = 0; i < 40; i++) {
        ShotApp t;
        int k;
        memset(&t, 0, sizeof(t));
        t.fontFamily = g_app.fontFamily;
        t.baseW = 200; t.baseH = 120;
        t.baseBits = xcalloc((size_t)(t.baseW * t.baseH), 4);
        if (!t.baseBits) break;
        SetRect(&t.sel, 0, 0, t.baseW, t.baseH);
        t.haveSel = 1;
        if (!shot_ensure_layer(&t)) { xfree(t.baseBits); break; }
        for (k = 0; k < T_COUNT; k++) {
            Stroke *s = (Stroke *)xcalloc(1, sizeof(Stroke));
            if (!s) break;
            s->kind = (ToolKind)k;
            s->color = 0xFFFF0000;
            s->width = 3;
            s->block = 8;
            s->fontSize = 18;
            s->filled = (i & 1);
            SetRect(&s->r, 10, 10, 180, 100);
            s->text = xwcsdup(L"标注 Aa");
            stroke_add_point(s, 12, 12);
            stroke_add_point(s, 170, 95);
            if (t.nstrokes == t.cstrokes) {
                int cap = t.cstrokes ? t.cstrokes * 2 : 16;
                Stroke **ns = (Stroke **)xrealloc(t.strokes,
                                    (size_t)cap * sizeof(Stroke *));
                if (!ns) { stroke_free(s); break; }
                t.strokes = ns; t.cstrokes = cap;
            }
            t.strokes[t.nstrokes++] = s;
        }
        t.cursor = t.nstrokes;
        render_layer(&t, NULL);
        shot_undo(&t);
        shot_redo(&t);
        xfree(t.baseBits); t.baseBits = NULL;
        shot_clear_strokes(&t);
            shot_free_layer(&t);
    }
    res_snapshot(&after);
    fails += check_res(&base, &after, "render_layer x40 (all 10 tools) leaks nothing");

    /* bitmap lockability ------------------------------------------------ */
    /* Reported, not asserted. GdipBitmapLockBits is unusable on this build -
     * it returns InvalidParameter for every bitmap, whether the bits came
     * from GDI+ or from us - so nothing in this app may call it. That is why
     * the annotation layer, the mosaic/blur scratch images and the composed
     * output are all created over a buffer we own: the pixels are then read
     * and written as plain memory, with no locking involved. The numbers are
     * logged so a future GDI+ that fixes this is visible rather than silent. */
    {
        GpBitmap *b1 = NULL, *b2 = NULL;
        BitmapData bd;
        void *own = xcalloc(64 * 64 * 4, 1);

        if (GdipCreateBitmapFromScan0(64, 64, 256, PixelFormat32bppARGB,
                                      NULL, &b1) == Ok) {
            int r1 = GdipBitmapLockBits(b1, NULL, PixelFormat32bppARGB,
                                        ImageLockModeRead, &bd);
            if (r1 == Ok) GdipBitmapUnlockBits(b1, &bd);
            app_log("  [info] GdipBitmapLockBits on a null-scan0 bitmap -> %d "
                    "(0 would mean it works; the app no longer calls it)", r1);
            GdipDisposeImage((GpImage *)b1);
        }
        if (own && GdipCreateBitmapFromScan0(64, 64, 256, PixelFormat32bppARGB,
                                             own, &b2) == Ok) {
            int r2 = GdipBitmapLockBits(b2, NULL, PixelFormat32bppARGB,
                                        ImageLockModeRead, &bd);
            if (r2 == Ok) GdipBitmapUnlockBits(b2, &bd);
            app_log("  [info] GdipBitmapLockBits on our own buffer -> %d", r2);
            GdipDisposeImage((GpImage *)b2);
        }
        xfree(own);
    }

    /* Drawing into a bitmap over our own buffer ----------------------- */
    /* GdipBitmapLockBits is unusable on this build (see above), so every
     * pixel read/write goes through a buffer we own and hand to
     * GdipCreateBitmapFromScan0. This proves that path actually delivers the
     * pixels, which is what mosaic, blur and the PNG writer depend on. */
    {
        const int W = 32, H = 32;
        BYTE *buf = xcalloc((size_t)(W * H * 4), 1);
        GpBitmap *bmp = NULL;
        GpGraphics *g = NULL;
        int drew = 0;

        if (buf && GdipCreateBitmapFromScan0(W, H, W * 4, PixelFormat32bppARGB,
                                             buf, &bmp) == Ok) {
            if (GdipGetImageGraphicsContext((GpImage *)bmp, &g) == Ok) {
                gx_fill_rect(g, 0xFFFF0000, 0, 0, (REAL)W, (REAL)H);
                GdipDeleteGraphics(g);
            }
            /* every pixel should now be opaque red in *our* buffer */
            {
                int i, bad = 0;
                for (i = 0; i < W * H; i++) {
                    BGRAPX *p = &((BGRAPX *)buf)[i];
                    if (p->a != 255 || p->r != 255 || p->g != 0 || p->b != 0) bad++;
                }
                drew = (bad == 0);
                app_log("       own-buffer draw: %d of %d pixels wrong", bad, W * H);
            }
            GdipDisposeImage((GpImage *)bmp);
        }
        xfree(buf);
        fails += check(drew, "drawing into a bitmap over our own buffer lands in that buffer");
    }

    /* mosaic / blur must actually change the pixels ------------------- */
    /* These two read the base image and write a *derived* image into the
     * layer, so every failure mode in them is invisible to a leak check:
     * a tool that quietly renders nothing still passes "leaks nothing". The
     * only way to know is to look at what came out. A hard checkerboard is
     * the input that makes the result unambiguous - a mosaic must average
     * each cell to one flat colour, a blur must soften the edges. */
    {
        const int W = 64, H = 64, cell = 8;
        ShotApp t;
        int x, y, i, flatCells = 0, cells = 0;
        int mosaicOk = 0, blurOk = 0;
        static const ToolKind KINDS[2] = { T_MOSAIC, T_BLUR };

        for (i = 0; i < 2; i++) {
            memset(&t, 0, sizeof(t));
            t.fontFamily = g_app.fontFamily;
            t.baseW = W; t.baseH = H;
            t.baseBits = xcalloc((size_t)(W * H), 4);
            if (!t.baseBits) break;
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    BGRAPX *p = &PX_AT(t.baseBits, W * 4, x, y);
                    int on = (((x / cell) + (y / cell)) & 1);
                    p->r = p->g = p->b = (BYTE)(on ? 255 : 0);
                    p->a = 255;
                }
            SetRect(&t.sel, 0, 0, W, H);
            t.haveSel = 1;
            if (!shot_ensure_layer(&t)) { xfree(t.baseBits); break; }
            t.strokes = (Stroke **)xcalloc(1, sizeof(Stroke *));
            t.strokes[0] = (Stroke *)xcalloc(1, sizeof(Stroke));
            if (!t.strokes || !t.strokes[0]) break;
            t.strokes[0]->kind = KINDS[i];
            t.strokes[0]->color = 0xFFFF0000;
            t.strokes[0]->block = cell;
            SetRect(&t.strokes[0]->r, 0, 0, W, H);
            t.nstrokes = 1; t.cursor = 1;
            render_layer(&t, NULL);

            /* count how many cells came out perfectly flat */
            flatCells = 0; cells = 0;
            if (t.layerBits) {
                const BGRAPX *px = (const BGRAPX *)t.layerBits;
                for (y = 0; y + cell <= H; y += cell)
                    for (x = 0; x + cell <= W; x += cell) {
                        BGRAPX first = px[(size_t)y * (size_t)W + x];
                        int flat = 1, u, v;
                        for (v = 0; v < cell && flat; v++)
                            for (u = 0; u < cell && flat; u++) {
                                BGRAPX c2 = px[(size_t)(y + v) * (size_t)W + (x + u)];
                                if (c2.a != first.a || c2.r != first.r ||
                                    c2.g != first.g || c2.b != first.b)
                                    flat = 0;
                            }
                        if (flat) flatCells++;
                        cells++;
                    }
            }
            if (KINDS[i] == T_MOSAIC) {
                /* every cell averaged flat: the checkerboard is gone */
                mosaicOk = (cells > 0 && flatCells == cells);
                app_log("       mosaic: %d/%d cells flat", flatCells, cells);
            } else {
                /* a blur keeps detail, it just removes the hard steps */
                blurOk = (flatCells < cells);
                app_log("       blur: %d/%d cells flat", flatCells, cells);
            }
            xfree(t.strokes[0]);
            xfree(t.strokes);
            t.strokes = NULL; t.nstrokes = t.cstrokes = t.cursor = 0;
            xfree(t.baseBits); t.baseBits = NULL;
            shot_free_layer(&t);
        }
        fails += check(mosaicOk, "mosaic replaces the pixels with flat cells");
        fails += check(blurOk, "blur alters the pixels without flattening them");
    }

    /* hotkey text round trip ------------------------------------------- */
    /* The registry stores the shortcut as text and the settings dialog
     * parses text back, so a mismatch here would quietly reset the user's
     * shortcut to a different combination on the next launch. */
    {
        static const struct { UINT mods; UINT vk; const wchar_t *text; } CASES[] = {
            { MOD_CONTROL | MOD_ALT,   (UINT)'A', L"Ctrl+Alt+A" },
            { MOD_CONTROL | MOD_SHIFT, (UINT)'A', L"Ctrl+Shift+A" },
            { MOD_CONTROL | MOD_ALT,   (UINT)'S', L"Ctrl+Alt+S" },
            { MOD_CONTROL | MOD_WIN,   (UINT)'Q', L"Ctrl+Win+Q" },
            { MOD_CONTROL,             VK_F5,    L"Ctrl+F5" },
            { MOD_ALT | MOD_SHIFT,     (UINT)'4', L"Alt+Shift+4" },
        };
        int i, roundTrip = 1, allRight = 1;
        Hotkey hk, back;
        wchar_t *s;

        for (i = 0; i < (int)(sizeof(CASES) / sizeof(CASES[0])); i++) {
            hk.mods = CASES[i].mods; hk.vk = CASES[i].vk;
            s = hotkey_text(&hk);
            if (!s || wcscmp(s, CASES[i].text) != 0) {
                app_log("        format: got '%ls' want '%ls'",
                        s ? s : L"(null)", CASES[i].text);
                allRight = 0;
            }
            if (s) xfree(s);
            memset(&back, 0, sizeof(back));
            if (!hotkey_parse(CASES[i].text, &back) ||
                back.mods != CASES[i].mods || back.vk != CASES[i].vk) {
                app_log("        parse: '%ls' -> mods=%u vk=%u", CASES[i].text,
                        (unsigned)back.mods, (unsigned)back.vk);
                roundTrip = 0;
            }
        }
        fails += check(allRight, "hotkey text is formatted as documented");
        fails += check(roundTrip, "hotkey text round-trips through the parser");
        /* a bare key would swallow an ordinary keystroke system-wide */
        fails += check(!hotkey_parse(L"A", &back), "a modifier-less key is rejected");
        fails += check(!hotkey_parse(L"Ctrl", &back), "a lone modifier is rejected");
        fails += check(!hotkey_parse(L"", &back), "empty text is rejected");
        /* the conflict message is what the user is told when the shortcut
         * they expected is taken by something else */
        {
            Hotkey want = { MOD_CONTROL | MOD_SHIFT, (UINT)'A' };
            wchar_t msg[256];
            hotkey_conflict_hint(&want, msg, 256);
            fails += check(msg[0] != 0, "a conflict message is produced");
            /* And it has to name the combination, or the user is told
             * something is in the way without being told what. */
            fails += check(wcsstr(msg, L"Ctrl+Shift+A") != NULL,
                           "the conflict message names the combination");

            /* The default is the owner's choice: Ctrl+Shift+A. It was once
             * changed behind their back to Ctrl+Alt+A - this assertion is
             * what makes doing that again fail the build instead. */
            {
                Hotkey def = { 0, 0 };
                wchar_t *dt;
                hotkey_default_of(&def);
                fails += check(def.mods == (MOD_CONTROL | MOD_SHIFT) &&
                               def.vk == (UINT)'A',
                               "the default shortcut is Ctrl+Shift+A, as set by the owner");
                dt = hotkey_text(&def);
                fails += check(dt && wcscmp(dt, L"Ctrl+Shift+A") == 0,
                               "the default formats as Ctrl+Shift+A");
                if (dt) xfree(dt);
            }
        }
    }

    /* edge snapping ----------------------------------------------------- */
    /* Driven with a fixed set of rectangles rather than with the real windows
     * on the machine, so the answer is the same everywhere and it can be wrong
     * loudly. A snap that fires a pixel too eagerly is the whole feature being
     * broken, and a snap that never fires is equally broken - both are decided
     * by distance, which is what these cases pin down. */
    {
        ShotApp sa;
        int x, y;
        /* The threshold is scaled by the DPI, so every distance below is
         * counted from the same number the overlay uses. Hard-coding "7" here
         * is how this check came to fail on a 125% display while the feature
         * was working exactly as designed. */
        int lim = app_scale_px(SNAP_PX);
        memset(&sa, 0, sizeof(sa));
        sa.vsx = 0; sa.vsy = 0; sa.vsw = 1920; sa.vsh = 1080;
        /* One window: left 100, top 200, right 800, bottom 600. */
        SetRect(&sa.edge[0], 100, 200, 800, 600);
        sa.nedge = 1;
        sa.snapX = sa.snapY = -1;

        x = 804; y = 500;                    /* 4 px right of its edge */
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 800 && sa.snapX == 800,
                       "a corner 4px from a window edge is pulled onto it");
        fails += check(sa.snapY < 0, "the far axis is left alone");

        x = 803; y = 500;                    /* 3 px: still inside the threshold */
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 800 && sa.snapX == 800,
                       "3px is inside the threshold too, so it snaps as well");

        x = 800 + lim + 1; y = 500;          /* one pixel out of reach */
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 800 + lim + 1 && sa.snapX < 0,
                       "one pixel past the threshold is left alone");

        x = 812; y = 500;
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 812 && sa.snapX < 0, "a corner well past it does not move");

        x = 500; y = 596;                    /* 4 px below its bottom */
        shot_snap_apply(&sa, &x, &y);
        fails += check(y == 600 && sa.snapY == 600,
                       "a corner 4px below a window edge is pulled onto it");

        /* The two axes are decided on their own: lined up on the left, four
         * pixels out on the bottom, and the guide has to show for the one that
         * earned it without dragging the other along. */
        x = 103; y = 604;
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 100 && y == 600 && sa.snapX == 100 && sa.snapY == 600,
                       "x and y snap independently, each with its own guide");

        /* The screen's own edges need no window to be near them. */
        x = 1916; y = 500;
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 1920, "the screen edge is a snap target of its own");

        /* Nothing within reach must clear the guides, or a stale red line
         * would be left on screen claiming an edge the corner is not on. */
        x = 500; y = 500;
        shot_snap_apply(&sa, &x, &y);
        fails += check(sa.snapX < 0 && sa.snapY < 0,
                       "no guide is left when nothing is snapped");

        /* And with no windows collected at all - which is the case when the
         * desktop has nothing to line up with - the screen edges still work. */
        sa.nedge = 0;
        x = 3; y = 500;
        shot_snap_apply(&sa, &x, &y);
        fails += check(x == 0 && sa.snapX == 0,
                       "an empty edge list still snaps to the screen");
        memset(&sa, 0, sizeof(sa));
    }

    /* window picking ----------------------------------------------------- */
    /* Click-to-pick is decided by shot_window_under_cursor, and that one is
     * honest to drive headlessly: real windows at real coordinates answer for
     * real. Two topmost popups at fixed screen coords, the second created
     * above the first, pin the Z order; a third hangs off the right edge of
     * the virtual screen to pin the clamp against the captured area. */
    {
        static const wchar_t *PICKCLS = L"MuskShotPickTest";
        WNDCLASSEXW wc;
        HINSTANCE hinst = GetModuleHandleW(NULL);
        RECT r1, r2, r3, got;
        HWND w1, w2, w3;
        int ok;

        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hinst;
        wc.lpszClassName = PICKCLS;
        RegisterClassExW(&wc);

        w1 = CreateWindowExW(WS_EX_TOPMOST, PICKCLS, L"", WS_POPUP,
                             60, 60, 220, 120, NULL, NULL, hinst, NULL);
        w2 = CreateWindowExW(WS_EX_TOPMOST, PICKCLS, L"", WS_POPUP,
                             100, 90, 220, 120, NULL, NULL, hinst, NULL);
        w3 = CreateWindowExW(WS_EX_TOPMOST, PICKCLS, L"", WS_POPUP,
                             GetSystemMetrics(SM_XVIRTUALSCREEN) +
                                 GetSystemMetrics(SM_CXVIRTUALSCREEN) - 60,
                             240, 220, 100, NULL, NULL, hinst, NULL);
        ShowWindow(w1, SW_SHOWNA);
        ShowWindow(w2, SW_SHOWNA);
        ShowWindow(w3, SW_SHOWNA);
        GetWindowRect(w1, &r1);
        GetWindowRect(w2, &r2);
        GetWindowRect(w3, &r3);

        fails += check(w1 && w2 && w3, "the pick-test windows were created");

        /* a point inside both: the one created last is on top */
        ok = shot_window_under_cursor(160, 120, NULL, NULL, &got) &&
             EqualRect(&got, &r2);
        fails += check(ok, "the frontmost window at a point is the one picked");

        /* a point only the first window covers */
        ok = shot_window_under_cursor(70, 70, NULL, NULL, &got) &&
             EqualRect(&got, &r1);
        fails += check(ok, "a window is found where only it covers");

        /* skipping the front window hands the pick to the one below it -
         * the same thing excluding the overlay does during a real capture */
        ok = shot_window_under_cursor(160, 120, w2, NULL, &got) &&
             EqualRect(&got, &r1);
        fails += check(ok, "a skipped window yields to the one underneath");

        /* the third window runs past the right edge of the capture; the
         * pick is clipped to what the frozen frame actually has */
        ok = shot_window_under_cursor(r3.left + 20, 290, NULL, NULL, &got) &&
             got.right <= GetSystemMetrics(SM_XVIRTUALSCREEN) +
                          GetSystemMetrics(SM_CXVIRTUALSCREEN) &&
             got.right < r3.right;
        fails += check(ok, "a window off the screen edge is clipped to the capture");

        if (w3) DestroyWindow(w3);
        if (w2) DestroyWindow(w2);
        if (w1) DestroyWindow(w1);
        UnregisterClassW(PICKCLS, hinst);
    }

    /* PNG round trip ----------------------------------------------------- */
    {
        int w = 137, h = 91, k;
        void *bits = NULL;
        HBITMAP d = make_dib(w, h, 32, &bits);
        unsigned char *png = NULL;
        size_t len = 0;
        int ok = 0;
        if (d && bits) {
            for (k = 0; k < w * h; k++) {
                BGRAPX *p = &PX_AT(bits, w * 4, k % w, k / w);
                p->r = (BYTE)(k & 0xFF);
                p->g = (BYTE)(k >> 8);
                p->b = 0x33;
                p->a = 255;
            }
            ok = gdiplus_encode_png(bits, w, h, w * 4, &png, &len) &&
                 len > 8 && png[0] == 0x89 && png[1] == 'P' &&
                 png[2] == 'N' && png[3] == 'G';
            xfree(png);
        }
        fails += check(ok, "PNG encode produces a valid signature");
        free_dib(d, bits);
    }

    /* CF_HTML construction ----------------------------------------------- */
    /* The clipboard's HTML flavour is what Word and rich-text editors paste
     * from. Its header carries four byte offsets into the UTF-8 document; a
     * consumer that follows them has to land on the tags they name, and a
     * producer that counts characters instead of bytes only works until the
     * image path carries something outside ASCII. */
    {
        static const wchar_t *PATHS[2] = {
            L"C:/Temp/shotclip/123.png",
            L"C:/\x4E34\x65F6/\x622A\x56FE.png"     /* 临时/截图 */
        };
        int p;
        for (p = 0; p < 2; p++) {
            char *bytes = NULL;
            size_t len = 0;
            if (!shot_build_cfhtml(PATHS[p], 100, 50, &bytes, &len)) {
                fails += check(0, "CF_HTML document builds");
                continue;
            }
            {
                /* parse the header back the way a consumer does */
                long v[4] = { -1, -1, -1, -1 };
                static const char *F[4] = { "StartHTML:", "EndHTML:",
                                            "StartFragment:", "EndFragment:" };
                char *at, *fragEnd;
                int i, ok = 1;
                for (i = 0; i < 4; i++) {
                    at = strstr(bytes, F[i]);
                    if (!at) { ok = 0; break; }
                    v[i] = atol(at + strlen(F[i]));
                }
                if (ok) {
                    if (v[0] < 0 || (size_t)v[0] >= len ||
                        strncmp(bytes + v[0], "<html>", 6) != 0) ok = 0;
                    if (v[2] < 0 || (size_t)v[2] >= len ||
                        strncmp(bytes + v[2], "<img ", 5) != 0) ok = 0;
                    if (v[1] != (long)len) ok = 0;
                    /* the fragment ends exactly where the end comment starts */
                    fragEnd = strstr(bytes, "<!--EndFragment-->");
                    if (!fragEnd || fragEnd != bytes + v[3]) ok = 0;
                }
                fails += check(ok, p == 0
                    ? "CF_HTML offsets land on the tags they name"
                    : "CF_HTML offsets stay byte-accurate for a non-ASCII path");
                app_log("       StartHTML=%ld EndHTML=%ld StartFragment=%ld "
                        "EndFragment=%ld of %lu bytes",
                        v[0], v[1], v[2], v[3], (unsigned long)len);
            }
            xfree(bytes);
        }
    }

    draw_shutdown(&g_app);
    gdiplus_stop();

    app_log("self-test %s (%d failures)", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    int argc = 0;
    LPWSTR *argv;
    int i;

    (void)prev; (void)show;
    app_attach_parent_console();
    app_set_dpi_aware();
    app_install_crash_log(L"shot");
    app_set_verbose(0);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--selftest") == 0) {
            if (argv) LocalFree(argv);
            return shot_selftest();
        }
        if (_wcsicmp(argv[i], L"--verbose") == 0) app_set_verbose(1);
        /* --icon <file.png> [size] - write the tray icon out and stop. Handy for
         * pinning the app to the taskbar or picking the artwork up elsewhere. */
        if (_wcsicmp(argv[i], L"--icon") == 0 && i + 1 < argc) {
            int size = (i + 2 < argc) ? _wtoi(argv[i + 2]) : 256;
            int ok = trayicon_export_png(argv[i + 1], TRAY_ICON_COLOR, size);
            if (argv) LocalFree(argv);
            return ok ? 0 : 1;
        }
        /* --ico <file.ico> [#RRGGBB] - write the multi-size .ico a program
         * embeds, in the fill given or this program's own when omitted, and
         * stop. build.ps1 -RegenIcon runs this once per program, since each of
         * the four exes carries its own colour. */
        if (_wcsicmp(argv[i], L"--ico") == 0 && i + 1 < argc) {
            ARGB color = TRAY_ICON_COLOR;
            int ok, bad = 0;
            if (i + 2 < argc) {
                unsigned long rgb;
                wchar_t *end = NULL;
                /* Strict about the colour on purpose: a typo that quietly fell
                 * back to the default would embed the wrong icon in an .exe
                 * and the build's colour check would only see a mismatch much
                 * later, if at all. */
                bad = (argv[i + 2][0] != L'#');
                rgb = bad ? 0 : wcstoul(argv[i + 2] + 1, &end, 16);
                bad = bad || !end || *end != L'\0' || rgb > 0xFFFFFFul;
                if (!bad) color = (ARGB)(0xFF000000u | (ARGB)rgb);
            }
            ok = !bad && trayicon_write_ico(argv[i + 1], color);
            if (bad) app_logw(L"--ico: colour must be #RRGGBB, got %ls", argv[i + 2]);
            if (argv) LocalFree(argv);
            return ok ? 0 : 1;
        }
    }
    if (argv) LocalFree(argv);

    memset(&g_app, 0, sizeof(g_app));
    g_app.inst = inst;
    g_app.tbHot = g_app.tbDown = -1;
    g_app.tool = T_PEN;
    g_app.color = 0xFFFF3B30;
    g_app.width = 4;
    g_app.mosaicBlock = 12;
    g_app.fontSize = 28;
    g_app.fillMode = 0;
    shot_cleanup_temp();

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = host_proc;
    wc.hInstance     = inst;
    wc.lpszClassName = L"MuskShotHost";
    /* The logo build.ps1 embeds as res\logo-shot.ico, so the Alt+Tab entry and
     * anything that asks the class for its icon shows the same artwork as the
     * .exe file. */
    wc.hIcon         = app_icon_load(inst, 0);
    wc.hIconSm       = app_icon_load(inst, GetSystemMetrics(SM_CXSMICON));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    g_app.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"MuskShotHost", L"", 0,
                                 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_app.hwnd) return 1;

    /* tray_init_icon takes ownership of the icon it is given and destroys it in
     * tray_free; it falls back to the stock system icon if this returns NULL. */
    tray_init_icon(&g_app.tray, g_app.hwnd, WM_APP + 1,
                   trayicon_from_logo(TRAY_ICON_COLOR),
                   L"MuskShot 截图涂抹");
    if (!tray_add(&g_app.tray)) {
        MessageBoxW(NULL, L"无法创建托盘图标，程序将退出。", L"MuskShot",
                    MB_OK | MB_ICONERROR);
        DestroyWindow(g_app.hwnd);
        return 1;
    }
    SetTimer(g_app.hwnd, TIMER_TRAY, 15000, NULL);

    /* Register the shortcut: what the user last had working, else
     * Ctrl+Shift+A. If that is taken, the app asks rather than picking a
     * substitute - see the note at the top of hotkey.c. A shortcut that
     * silently failed to register is the single most confusing way this app
     * can break, and so is one that registers a combination the user never
     * asked for. */
    {
        Hotkey refused;
        memset(&refused, 0, sizeof(refused));
        g_app.hotkeyOk = hotkey_register(g_app.hwnd, &g_app.hotkey, &refused) != 0;
        if (g_app.hotkeyOk) {
            wchar_t *t = hotkey_text(&g_app.hotkey);
            if (t) {
                wchar_t tip[160];
                _snwprintf_s(tip, 160, _TRUNCATE, L"MuskShot 截图涂抹  —  %s", t);
                tray_set_tip(&g_app.tray, tip);
                xfree(t);
            }
        } else {
            wchar_t msg[256];
            /* Names the combination that was refused, not "something" - the
             * user pressed these exact keys for years in another program, and
             * being vague about which ones is what makes a message useless. */
            hotkey_conflict_hint(&refused, msg, 256);
            _snwprintf_s(g_menuHotkeyItem, 160, _TRUNCATE, L"快捷键：未生效\t%d", MI_HKSTATUS);
            tray_balloon(&g_app.tray, L"MuskShot", msg, NIIF_WARNING);
            /* Ask now rather than at the first confused press. The dialog is
             * the same one behind 设置快捷键…, and edit_hotkey() registers,
             * stores and re-tips whatever comes out of it - so a shortcut set
             * here survives the restart exactly like one set from the menu. */
            if (edit_hotkey()) {
                wchar_t *t = hotkey_text(&g_app.hotkey);
                if (t) {
                    _snwprintf_s(g_menuHotkeyItem, 160, _TRUNCATE,
                                 L"快捷键：%s\t%d", t, MI_HKSTATUS);
                    xfree(t);
                }
            }
        }
    }

    while (!g_app.quit) {
        if (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            break;
        }
    }

    KillTimer(g_app.hwnd, TIMER_TRAY);
    hotkey_unregister(g_app.hwnd);
    draw_shutdown(&g_app);
    tray_free(&g_app.tray);
    DestroyWindow(g_app.hwnd);
    UnregisterClassW(L"MuskShotHost", inst);
    return 0;
}
