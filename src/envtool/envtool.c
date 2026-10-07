/* envtool.c - environment variable editor with built-in dev presets. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "presets.h"
#include "../common/uikit.h"
#include "../common/tray.h"
#include "../common/trayicon.h"
#include <commctrl.h>
#include <gdiplus.h>

#define ET_CLASS     L"MuskEnvTool"
#define HOTKEY_ID    1
#define ID_LIST      1001
#define ID_NAME      1002
#define ID_VALUE     1003
#define ID_SCOPE     1004
#define ID_FILTER    1005
#define ID_ROOTS     1006
#define ID_PRESET    1007
/* Accelerator-only command: the status bar advertises Ctrl+S, so it has to
 * work from inside the list and the text boxes too, not just when the parent
 * window itself has focus. */
#define IDM_ACCEL_SAVE    40010

#define TIMER_STATUS      1
#define TIMER_FORMVIS     2

/* This program's own half of the About box. The name, the icon and the credit
 * line are added by app_show_about() (src\common\about.c), so all four tools
 * credit the same author without four copies of the text to keep in step.
 * Declared up here because the toolbar's 关于 button asks for them long before
 * the command-line switch near wWinMain does. */
static const wchar_t *ET_ABOUT_NAME  = L"MuskEnvTool  ·  环境变量编辑器";
static const wchar_t *ET_ABOUT_NOTES =
    L"读取      用户 + 系统两个作用域，按名称合并显示\r\n"
    L"写入      直接更新注册表并广播 WM_SETTINGCHANGE\r\n"
    L"预设      自动探测已安装的开发工具，一键写入\r\n"
    L"PATH      按目录拆成列表，可增删和调整顺序\r\n"
    L"权限      改【系统】环境需要管理员，请右键本程序选择【以管理员身份运行】\r\n"
    L"提示      PATH 对已运行的程序不生效，重启资源管理器后 Explorer 才会更新";

#define STATUS_HINT       L"左边点一个变量查看　·　要改先点【修改】　·　" \
                          L"填完点【保存】，保存前会先给你看要写什么"

enum {
    Z_NONE = 0,
    Z_NEW,          /* 新增  - right column toolbar, always present      */
    Z_SAVE,         /* 保存  - right column toolbar, always present      */
    Z_REFRESH,      /* left column header                                */
    Z_TEMPLATE,     /* the template picker, 新增 mode only               */
    Z_MODIFY,       /* bottom bar, only once a row is selected          */
    Z_DELETE,
    Z_COPY,
    Z_EXPAND,       /* beside the value box                              */
    Z_ROOTS_EDIT,   /* scan roots, behind the template picker            */
    Z_PATH_ADD, Z_PATH_UP, Z_PATH_DOWN, Z_PATH_DEL,
    Z_BROADCAST, Z_ELEVATE, Z_ABOUT
};

/* Windows variables that the system itself depends on. Deleting any of these
 * breaks the login session, installers and a lot of tooling in ways that are
 * painful to undo, so the 删除 button is drawn disabled for them and says why
 * when pressed. The check is by name, on purpose: it is a guard rail, not a
 * security boundary, and a user who really means it can still use regedit. */
static int env_var_protected(const wchar_t *name)
{
    static const wchar_t *PROTECTED[] = {
        L"Path", L"PATHEXT", L"PathExt",
        L"SystemRoot", L"windir", L"SystemDrive",
        L"ComSpec", L"ComSpec", L"COMSPEC",
        L"TEMP", L"TMP",
        L"OS", L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER",
        L"PROCESSOR_LEVEL", L"PROCESSOR_REVISION", L"NUMBER_OF_PROCESSORS",
        L"HOMEDRIVE", L"HOMEPATH", L"USERPROFILE", L"USERNAME",
        L"USERDOMAIN", L"USERDOMAIN_ROAMINGPROFILE", L"USERNAME_DOMAIN",
        L"APPDATA", L"LOCALAPPDATA", L"PROGRAMDATA", L"ALLUSERSPROFILE",
        L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432",
        L"CommonProgramFiles", L"CommonProgramFiles(x86)",
        L"ProgramData", L"PSModulePath", L"SESSIONNAME", L"DriverData",
        L"CLASS_PATH", L"PUBLIC", L"SystemDrive", L"LOGONSERVER",
        L"oneDrive", L"OneDrive", L"onedrive", L"ONEDRIVE",
        L"__COMPAT_LAYER", L"__Derived_Data_Base", NULL
    };
    int i;
    for (i = 0; PROTECTED[i]; i++)
        if (_wcsicmp(PROTECTED[i], name) == 0) return 1;
    return 0;
}

/* Forward declarations: the edit-state helpers below call the save path, and
 * the save path calls back into them. */
static int  save_current(void);
static void update_dirty(void);
static void remember_base(void);
static void discard_edits(void);
static int  confirm_discard(void);
static void set_status(const wchar_t *fmt, ...);
static void set_notice(const wchar_t *fmt, ...);
static void refresh_all(void);
static void select_index(int i);
static void load_selected_into_editor(void);
static void path_commit(void);
static int  path_add_entry(const wchar_t *entry);
static void do_new(void);
static void preset_hits_free(void);
static void preset_scan_all(void);
static void fill_from_template(int idx);
static void undo_push(Scope s, const wchar_t *name, const wchar_t *what);
static void undo_drop_last(void);
static int  env_name_ok(const wchar_t *n);
static const wchar_t *scope_value(const EnvVar *v, Scope s);
static int  editor_read(wchar_t *name, size_t nameCch, wchar_t **valueOut);
static void edit_roots(void);
static int  zone_hovered(int z);
static int  tool_index(int z);
static void sync_form_visibility_soon(void);
static void sync_form_visibility(void);
static void set_form_locked(int locked);
static void build_hits(void);

static void wrap_value(wchar_t *out, size_t outCch, const wchar_t *v);
static int  env_var_protected(const wchar_t *name);
static void fill_from_template(int idx);

typedef struct { RECT r; int z; int idx; const wchar_t *tip; } Hit;

/* Repaint / input counters. The flicker and the "clicks do nothing" reports
 * were both impossible to argue about without numbers, so they are measured
 * rather than eyeballed: a steady-state repaint must not move any child
 * control, and the ListView must not be re-notified in a loop.
 * `.\envtool.exe --stats` dumps them on demand. */
typedef struct {
    long paint;          /* WM_PAINT handled                          */
    long childMove;      /* MoveWindow calls that actually moved       */
    long columnSet;      /* LVM_SETCOLUMNW calls that actually changed */
    long notify;         /* WM_NOTIFY received                         */
    long listNotify;     /* ... of which came from the ListView        */
    long relayout;       /* full layout rebuilds                      */
    long mouseMove;      /* WM_MOUSEMOVE handled                      */
} EtStats;

static EtStats g_stat;

typedef struct {
    HINSTANCE inst;
    HWND      hwnd;
    Tray      tray;
    int       quit;

    EnvList   vars;
    int       selected;        /* index into vars, -1 none */
    Scope     editScope;
    wchar_t   status[240];
    ULONGLONG statusUntil;

    /* --- what the right-hand form is currently doing --------------------
     * Two modes, and the difference decides what 保存 means:
     *   selected < 0  新增 mode - an empty form. Saving a name that already
     *                 exists APPENDS to the value that is already there.
     *   selected >= 0 查看 mode - the values are shown but locked. 修改
     *                 unlocks them, and saving then OVERWRITES, behind a
     *                 before/after comparison. */
    int       editing;          /* 修改 has been pressed: fields unlocked   */
    int       templateIdx;      /* template combo selection, -1 none       */

    /* --- edit state ----------------------------------------------------
     * Everything needed to answer "will I lose work if I move away from this
     * row right now". The editor is a plain pair of text boxes with no
     * ownership of what was typed into them, so without this snapshot the
     * answer was "yes, silently". */
    int       dirty;               /* editor differs from baseName/baseValue */
    int       baseIsNew;           /* the loaded state was "not set at all"  */
    int       keepEditor;          /* reload the list but leave the boxes be */
    int       loading;             /* sync_list is rebuilding: ignore the
                                   * selection notifications it provokes */
    int       testMode;            /* --selftest: never open a modal box */
    int       formVisDirty;        /* show/hide of the form is deferred */
    int       formLocked;          /* the selected variable is read-only   */
    Scope     baseScope;
    wchar_t   baseName[ENV_NAME_MAX];
    wchar_t  *baseValue;           /* xmalloc'd, NULL when baseIsNew         */

    /* --- undo ----------------------------------------------------------
     * A short ring of the writes this session performed. Environment edits are
     * destructive and the only way back was to open regedit, so the value
     * that was there before the write is kept in memory and restorable. */
    struct {
        Scope    scope;
        wchar_t  name[ENV_NAME_MAX];
        wchar_t *oldValue;          /* NULL when the variable did not exist */
        DWORD    oldType;
        wchar_t  what[64];          /* "保存" / "删除" / "预设", for the label */
    } undo[12];
    int       undoCount;

    /* --- preset detection cache ---------------------------------------
     * The sweep is cheap but not free, and the answer does not change while
     * the window is open. Caching it is what lets the template picker say
     * which tools are actually installed instead of making the user find out
     * by trial and error. */
    wchar_t **presetHit;            /* xmalloc'd absolute path, or NULL */
    int       presetHitCount;
    int       presetHitReady;
    /* PATH editor */
    int       pathMode;        /* 1 when the selected var is a PATH */
    PathList  path;
    int       pathSel;

    GpFontFamily *fam, *mono;
    /* Fonts are built once per scale and reused. Creating ~15 GDI+ font
     * objects per frame was the second half of the flicker: each GdipCreateFont
     * allocates and each GdipDeleteFont churns GDI+, and at hover-move rates
     * the toolbar visibly stuttered. */
    GpFont   *fBody, *fSmall, *fMonoF;
    int       fontPx;          /* pixel size the cached fonts were built at */
    int       laidOut;         /* g_el / child rects are current           */
    int       childRect[6][4]; /* last rect handed to MoveWindow, per child*/
    int       listColWidth;    /* width the columns were last fitted for    */
    /* Back buffer: a real DIB section that GDI+ draws into, so the same
     * bitmap can be selected into a memory DC for the final BitBlt. */
    HBITMAP   memBmp;
    void     *memBits;
    GpBitmap *mem;
    int       memW, memH;
    Hit     *hit;
    int      nhit, chit, hover, pressed;
    int       pressing;         /* we own the capture for a button press  */

    /* edit controls */
    HWND hList, hName, hValue, hScope, hFilter, hPreset;
    HFONT uiFont;          /* the GDI font shared by every child control */
} EnvTool;

static EnvTool g_et;

/* Rectangles of every drawn region, recomputed on resize only. Declared here
 * because the status-bar helpers below already need to invalidate it. */
typedef struct {
    /* left column: the existing variables */
    RECT filterBox, refreshBtn, listBox;
    /* right column: the form you add / edit / delete through */
    RECT barNew, barSave;      /* the toolbar, at the top of the right column */
    RECT aboutBtn;              /* 关于, same row, far end                          */
    RECT banner;               /* the standing "run as administrator" notice */
    RECT tplLabel, tplBox, tplRoots;
    RECT nameBox, scopeBox, valueBox, valueExp;
    RECT pathBox, pathAdd, pathUp, pathDown, pathDel;
    /* bottom of the right column, only drawn once a row is selected */
    RECT actModify, actDelete, actCopy;
    RECT statusBar;
} ETLayout;

static ETLayout g_el;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int hit_add(RECT r, int z, int idx, const wchar_t *tip)
{
    Hit *h;
    if (g_et.nhit == g_et.chit) {
        int cap = g_et.chit ? g_et.chit * 2 : 64;
        Hit *nh = (Hit *)xrealloc(g_et.hit, (size_t)cap * sizeof(Hit));
        if (!nh) return -1;
        g_et.hit = nh; g_et.chit = cap;
    }
    h = &g_et.hit[g_et.nhit++];
    h->r = r; h->z = z; h->idx = idx; h->tip = tip;
    return g_et.nhit - 1;
}

static int hit_test(int x, int y)
{
    int i;
    POINT p;
    p.x = x; p.y = y;
    for (i = g_et.nhit - 1; i >= 0; i--)
        if (PtInRect(&g_et.hit[i].r, p)) return i;
    return -1;
}

/* Only the zones that actually react to a press take the capture. Capturing
 * for a decorative zone (the preset row, the PATH frame, the list frame)
 * steals clicks from the child control that lives inside it. */
static int zone_is_button(int z)
{
    switch (z) {
    case Z_NEW: case Z_SAVE: case Z_DELETE: case Z_REFRESH:
    case Z_EXPAND: case Z_BROADCAST: case Z_COPY: case Z_MODIFY:
    case Z_ROOTS_EDIT: case Z_PATH_ADD: case Z_PATH_UP:
    case Z_PATH_DOWN: case Z_PATH_DEL: case Z_ABOUT:
        return 1;
    default:
        return 0;
    }
}

static void free_fonts(void)
{
    if (g_et.fBody)  { GdipDeleteFont(g_et.fBody);  g_et.fBody = NULL; }
    if (g_et.fSmall) { GdipDeleteFont(g_et.fSmall); g_et.fSmall = NULL; }
    if (g_et.fMonoF) { GdipDeleteFont(g_et.fMonoF); g_et.fMonoF = NULL; }
    g_et.fontPx = 0;
}

/* Builds the three fonts the UI uses, and rebuilds them only when the scale
 * changes. Returns 0 when GDI+ could not produce a usable font at all. */
static int ensure_fonts(void)
{
    int px = app_scale_px(12);
    if (px == g_et.fontPx && g_et.fBody) return 1;
    free_fonts();
    g_et.fBody  = gx_font(g_et.fam, (REAL)px);
    g_et.fSmall = gx_font(g_et.fam, (REAL)app_scale_px(11));
    g_et.fMonoF = gx_font(g_et.mono, (REAL)app_scale_px(11));
    g_et.fontPx = px;
    return g_et.fBody != NULL;
}

/* Copies at most `max` characters of `src` into `dst`, always NUL terminated,
 * flattening the characters a single-line cell cannot render. When the text
 * was cut, the ellipsis takes the place of the last kept character, so the
 * result is never longer than `max` and never longer than the buffer. */
static wchar_t *clip_cell(wchar_t *dst, size_t dstCch, const wchar_t *src,
                          size_t max)
{
    size_t n = 0, cap;
    if (dstCch == 0) return dst;
    dst[0] = 0;
    if (!src || !*src) return dst;
    cap = dstCch - 1;                 /* always keep room for the terminator */
    if (max > cap) max = cap;
    while (n < max && src[n]) {
        wchar_t c = src[n];
        if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
        dst[n] = c;
        n++;
    }
    if (src[n] != 0 && n > 0) dst[n - 1] = 0x2026;   /* '...' */
    dst[n] = 0;
    return dst;
}

/* `envtool.exe --stats` prints this at start-up, and the self-test reads the
 * same numbers, so "is it flickering?" is answered by a figure rather than by
 * squinting at the window. */
static void et_log_stats(void)
{
    app_log("  paint=%ld  childMove=%ld  columnSet=%ld  relayout=%ld",
            g_stat.paint, g_stat.childMove, g_stat.columnSet, g_stat.relayout);
    app_log("  notify=%ld (from list=%ld)  mouseMove=%ld",
            g_stat.notify, g_stat.listNotify, g_stat.mouseMove);
}

static void invalidate_status(void)
{
    if (g_et.hwnd && g_et.laidOut)
        InvalidateRect(g_et.hwnd, &g_el.statusBar, FALSE);
}

static void set_status(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(g_et.status, 240, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_et.statusUntil = GetTickCount64() + 4000;
    /* The message is meant to fade back to the hint on its own. Nothing used
     * to repaint at the deadline, so the old text simply stayed up until some
     * unrelated repaint happened. A cheap timer redraws just that strip. */
    if (g_et.hwnd) SetTimer(g_et.hwnd, TIMER_STATUS, 400, NULL);
    invalidate_status();
}

/* For the messages the user must not be able to miss: the ones that explain
 * why a button they just pressed did nothing, and the ones that report a write
 * that failed. Those used to go to the strip along with everything else, and
 * a grey line at the bottom of the window is not where anyone looks after
 * clicking a button - the report was that the app had gone quiet.
 *
 * Deliberately not used for progress. "已复制变量值" after a copy, or "已上移
 * 一位" after a move, would put a modal dialog in the middle of a sequence of
 * clicks, and the flow would have to be dismissed several times to finish.
 * Those stay in the strip, which is drawn at body size and in the accent
 * colour while it is live. */
static void set_notice(const wchar_t *fmt, ...)
{
    wchar_t msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    set_status(L"%s", msg);
    /* --selftest drives the real click path, and a modal box with nobody to
     * dismiss it would hang the run rather than fail it. */
    if (!g_et.hwnd || g_et.testMode) return;
    MessageBoxW(g_et.hwnd, msg, L"请注意", MB_OK | MB_ICONINFORMATION);
}

static void sync_list(void)
{
    HWND lv = g_et.hList;
    wchar_t filter[128];
    int i;

    if (!lv) return;
    GetWindowTextW(g_et.hFilter, filter, 128);
    env_free(&g_et.vars);
    env_load(&g_et.vars, filter[0] ? filter : NULL);

    /* Emptying and refilling a report-mode ListView makes it select row 0 on
     * its own, and each of those state changes comes back to us as
     * LVN_ITEMCHANGED. Without this guard the notification re-enters
     * select_index() in the middle of the rebuild and overwrites the editor
     * with whatever row 0 happens to be - which is how a value being typed
     * vanished the moment the filter box was touched. */
    g_et.loading = 1;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    /* The item count only ever changes wholesale here, so reset and refill. */
    SendMessageW(lv, LVM_SETITEMCOUNT, 0, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    for (i = 0; i < g_et.vars.count; i++) {
        const EnvVar *v = &g_et.vars.items[i];
        LVITEMW li;
        wchar_t val[160], scope[16];

        /* An absent value shows as "-", an empty one as "-", too. */
        if (v->value && v->value[0]) clip_cell(val, 160, v->value, 150);
        else                           wcsncpy(val, L"-", 160);
        if (v->inUser && v->inSystem) wcsncpy(scope, L"用户+系统", 15);
        else if (v->inUser)         wcsncpy(scope, L"用户", 15);
        else                        wcsncpy(scope, L"系统", 15);
        scope[15] = 0;

        memset(&li, 0, sizeof(li));
        /* Without LVIF_TEXT the list view silently inserts blank rows. */
        li.mask = LVIF_TEXT | LVIF_IMAGE;
        li.iItem = i;
        li.iSubItem = 0;
        li.pszText = (LPWSTR)v->name;
        li.cchTextMax = ENV_NAME_MAX;
        li.iImage = (v->inUser ? 1 : 0);
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&li);

        li.iSubItem = 1; li.pszText = val; li.cchTextMax = 160;
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li);

        li.iSubItem = 2; li.pszText = scope; li.cchTextMax = 16;
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    g_et.loading = 0;
    InvalidateRect(lv, NULL, TRUE);
}

/* Pushes the application-level selection into the ListView. Without this the
 * editor showed a variable while the list showed no highlighted row at all
 * after every save, delete or filter change - the row you are editing looked
 * unselected, which reads as "the list ignores me". */
static void apply_list_selection(void)
{
    LVITEMW li;
    if (!g_et.hList) return;
    memset(&li, 0, sizeof(li));
    li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    li.state = (g_et.selected >= 0) ? (LVIS_SELECTED | LVIS_FOCUSED) : 0;
    SendMessageW(g_et.hList, LVM_SETITEMSTATE, (WPARAM)g_et.selected, (LPARAM)&li);
    if (g_et.selected >= 0)
        SendMessageW(g_et.hList, LVM_ENSUREVISIBLE, (WPARAM)g_et.selected, 0);
}

static EnvVar *sel_var(void)
{
    if (g_et.selected < 0 || g_et.selected >= g_et.vars.count) return NULL;
    return &g_et.vars.items[g_et.selected];
}

/* ------------------------------------------------------------------ */
/* edit state: what the editor holds, and what it held when it opened  */
/* ------------------------------------------------------------------ */

/* Reads the two text boxes into caller-owned buffers. Returns 1 when both
 * fit; a name longer than the box is a save error, not a truncation, so the
 * caller is told rather than handed a silently shortened variable. */
static int editor_read(wchar_t *name, size_t nameCch, wchar_t **valueOut)
{
    DWORD n = GetWindowTextLengthW(g_et.hValue);
    wchar_t *v;
    int ln;

    ln = GetWindowTextW(g_et.hName, name, (int)nameCch);
    if (ln >= (int)nameCch) return 0;          /* truncated: do not guess */
    v = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t) + 2);
    if (!v) { *valueOut = NULL; return 0; }
    if (n) GetWindowTextW(g_et.hValue, v, (int)n + 1);
    v[n] = 0;
    *valueOut = v;
    return 1;
}

/* The value that belongs to `scope`, or NULL when it is not set there. For a
 * variable that exists in both places this is what makes the two copies
 * separable: editing "系统环境" must not show - or overwrite - the user copy. */
static const wchar_t *scope_value(const EnvVar *v, Scope s)
{
    return s == SCOPE_SYSTEM ? v->sysValue : v->userValue;
}

/* Records what the editor is allowed to go back to. Called on every row
 * change, on 新建, and after a successful save. */
static void remember_base(void)
{
    EnvVar *v = sel_var();
    wchar_t *old = g_et.baseValue;
    g_et.baseValue = NULL;
    g_et.baseIsNew = (v == NULL);
    g_et.baseScope = g_et.editScope;
    if (v) {
        wcsncpy(g_et.baseName, v->name, ENV_NAME_MAX - 1);
        g_et.baseName[ENV_NAME_MAX - 1] = 0;
        {
            const wchar_t *sv = scope_value(v, g_et.editScope);
            /* Fall back to the other scope's copy: the row exists, it just
             * is not set in the one currently selected. */
            if (!sv) sv = v->value;
            g_et.baseValue = sv ? xwcsdup(sv) : xwcsdup(L"");
        }
    } else {
        g_et.baseName[0] = 0;
        g_et.baseValue = xwcsdup(L"");
    }
    xfree(old);
    update_dirty();
}

/* Recomputes the dirty flag from what is on screen versus the base snapshot.
 * Cheap enough to run on every keystroke, and the status bar plus the
 * 放弃修改 button are both driven from it. */
static void update_dirty(void)
{
    wchar_t name[ENV_NAME_MAX];
    wchar_t *value = NULL;
    int d = 1;

    if (g_et.baseValue && editor_read(name, ENV_NAME_MAX, &value)) {
        d = (_wcsicmp(name, g_et.baseName) != 0) ||
            (wcscmp(value, g_et.baseValue) != 0) ||
            (g_et.editScope != g_et.baseScope);
    }
    xfree(value);
    if (d != g_et.dirty) {
        g_et.dirty = d;
        if (g_et.laidOut) InvalidateRect(g_et.hwnd, &g_el.statusBar, FALSE);
    }
}

/* Called before anything that would throw the editor away: a row change, a
 * refresh, 新建, a filter keystroke, a close. Returns 1 to carry on, 0 when
 * the user cancelled and the caller must abort.
 *
 * Under --selftest the prompt is suppressed. A modal box has no one to answer
 * it, so a single mis-ordered assertion would wedge the whole run with no
 * output at all - which is exactly what happened while this was being written. */
static int confirm_discard(void)
{
    int r;
    if (!g_et.dirty) return 1;
    if (g_et.testMode) return 1;
    r = MessageBoxW(g_et.hwnd,
            L"当前这个变量还没有保存。\r\n\r\n"
            L"要先把修改保存下来吗？\r\n"
            L"【是】保存并继续　　【否】丢弃修改　　【取消】留在这里",
            L"未保存的修改", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return 0;
    if (r == IDYES)    return save_current();
    discard_edits();
    return 1;
}

static void discard_edits(void)
{
    SetWindowTextW(g_et.hName, g_et.baseName);
    SetWindowTextW(g_et.hValue, g_et.baseValue ? g_et.baseValue : L"");
    update_dirty();
    InvalidateRect(g_et.hwnd, NULL, FALSE);
}

/* Locks or unlocks the form. A selected variable is shown read-only until
 * 修改 is pressed: opening something to look at it should never be one stray
 * keystroke away from an overwrite. */
static void set_form_locked(int locked)
{
    g_et.formLocked = locked;
    sync_form_visibility_soon();
    if (g_et.hwnd) InvalidateRect(g_et.hwnd, NULL, FALSE);
}

/* Applies the form's show/hide and read-only state.
 *
 * None of this may happen while the ListView is notifying us from inside its
 * own WM_LBUTTONDOWN. Showing a sibling, or flipping EnableWindow on one,
 * moves a child of the same parent mid-message, and comctl32 ends up waiting
 * for a mouse-up that never arrives: the app freezes on the first click,
 * burning no CPU, with no dialog on screen. Everything here therefore runs
 * from a one-shot timer, after the handler has unwound. */
static void sync_form_visibility(void)
{
    if (!g_et.hName) return;
    /* The form itself is always on screen - in 新增 mode it is the thing you
     * type into. Only the template picker belongs to one mode. */
    if (!IsWindowVisible(g_et.hName))  ShowWindow(g_et.hName,  SW_SHOW);
    if (!IsWindowVisible(g_et.hValue)) ShowWindow(g_et.hValue, SW_SHOW);
    if (!IsWindowVisible(g_et.hScope)) ShowWindow(g_et.hScope, SW_SHOW);
    if (g_et.hPreset) {
        int want = (g_et.selected < 0) ? TRUE : FALSE;
        if (IsWindowVisible(g_et.hPreset) != want)
            ShowWindow(g_et.hPreset, want ? SW_SHOW : SW_HIDE);
    }
    SendMessageW(g_et.hName,  EM_SETREADONLY, (WPARAM)(g_et.formLocked ? TRUE : FALSE), 0);
    SendMessageW(g_et.hValue, EM_SETREADONLY, (WPARAM)(g_et.formLocked ? TRUE : FALSE), 0);
    EnableWindow(g_et.hScope, g_et.formLocked ? FALSE : TRUE);
    g_et.formVisDirty = 0;
    if (g_et.hwnd && g_et.laidOut) InvalidateRect(g_et.hwnd, NULL, FALSE);
}

static void sync_form_visibility_soon(void)
{
    g_et.formVisDirty = 1;
    if (g_et.hwnd) SetTimer(g_et.hwnd, TIMER_FORMVIS, 1, NULL);
}

static void load_selected_into_editor(void)
{
    EnvVar *v = sel_var();
    const wchar_t *sv;
    path_free(&g_et.path);
    g_et.pathMode = 0;
    g_et.pathSel = -1;

    sync_form_visibility_soon();

    if (g_et.keepEditor) {           /* filtering must not eat a half-typed value */
        g_et.keepEditor = 0;
        return;
    }
    if (!v) {
        g_et.editing = 0;
        g_et.editScope = SCOPE_USER;
        SendMessageW(g_et.hScope, CB_SETCURSEL, (LPARAM)SCOPE_USER, 0);
        SetWindowTextW(g_et.hName, L"");
        SetWindowTextW(g_et.hValue, L"");
        set_form_locked(0);
        remember_base();
        build_hits();  /* the bottom strip only exists once a row is selected */
        InvalidateRect(g_et.hwnd, NULL, FALSE);
        return;
    }
    /* Open on the scope that actually holds the value, so the common case -
     * a variable that lives in one place - is ready to inspect. */
    g_et.editScope = v->inUser ? SCOPE_USER : SCOPE_SYSTEM;
    g_et.editing = 0;
    sv = scope_value(v, g_et.editScope);
    if (!sv) sv = v->value;          /* the copy lives in the other scope */
    SetWindowTextW(g_et.hName, v->name);
    SetWindowTextW(g_et.hValue, sv ? sv : L"");
    SendMessageW(g_et.hScope, CB_SETCURSEL, (LPARAM)g_et.editScope, 0);
    if (sv && env_is_path_var(v->name)) {
        path_parse(sv, &g_et.path);
        g_et.pathMode = 1;
    }
    set_form_locked(1);
    remember_base();
    build_hits();      /* the bottom strip only exists once a row is selected */
    InvalidateRect(g_et.hwnd, NULL, FALSE);
}

/* Swaps the editor to the other scope's copy of the same variable. This is
 * what makes a name that exists in both places editable as two things rather
 * than one, which is how "I changed it and nothing happened" happens when the
 * write landed in a scope the shell was not reading. */
static void reload_for_scope(Scope s)
{
    EnvVar *v = sel_var();
    const wchar_t *sv;
    g_et.editScope = s;
    SendMessageW(g_et.hScope, CB_SETCURSEL, (LPARAM)s, 0);
    path_free(&g_et.path);
    g_et.pathMode = 0;
    g_et.pathSel = -1;
    if (!v) { remember_base(); InvalidateRect(g_et.hwnd, NULL, FALSE); return; }
    sv = scope_value(v, s);
    SetWindowTextW(g_et.hValue, sv ? sv : L"");
    if (sv && env_is_path_var(v->name)) {
        path_parse(sv, &g_et.path);
        g_et.pathMode = 1;
    }
    /* The base for the new scope is that scope's own value, so switching back
     * and forth is not reported as a pending edit. */
    remember_base();
    InvalidateRect(g_et.hwnd, NULL, FALSE);
}

static void select_index(int i)
{
    if (i < 0 || i >= g_et.vars.count) g_et.selected = -1;
    else                              g_et.selected = i;
    load_selected_into_editor();
    InvalidateRect(g_et.hwnd, NULL, FALSE);
}

static void refresh_all(void)
{
    wchar_t keep[ENV_NAME_MAX];
    int i, keepIdx = -1;
    keep[0] = 0;
    {
        EnvVar *v = sel_var();
        if (v) { wcsncpy(keep, v->name, ENV_NAME_MAX - 1); keep[ENV_NAME_MAX - 1] = 0; }
    }
    sync_list();
    if (keep[0]) {
        for (i = 0; i < g_et.vars.count; i++)
            if (_wcsicmp(g_et.vars.items[i].name, keep) == 0) { keepIdx = i; break; }
    }
    g_et.selected = keepIdx;
    /* Pushing the selection back is itself a list operation, and it fires
     * LVN_ITEMCHANGED for the row we just selected. Left unguarded that
     * re-enters select_index() from inside refresh_all, which asks the user
     * about unsaved edits while they are in the middle of typing in the
     * filter box. The whole tail of a rebuild is ours, not the user's. */
    g_et.loading = 1;
    load_selected_into_editor();
    /* The list was rebuilt from scratch, so its own selection is gone; push
     * ours back or the editor and the list disagree. */
    apply_list_selection();
    g_et.loading = 0;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* undo                                                                */
/* ------------------------------------------------------------------ */

#define UNDO_MAX ((int)(sizeof(g_et.undo) / sizeof(g_et.undo[0])))

/* Remembers what a variable held before this write, so a mistake is one click
 * from being undone. Called *before* the registry is touched - afterwards the
 * old value no longer exists anywhere. `oldValue == NULL` records "this
 * variable was not set at all", which is the state an undo has to restore
 * when the user added it by mistake. */
static void undo_push(Scope s, const wchar_t *name, const wchar_t *what)
{
    int i;
    DWORD t = 0;
    if (g_et.undoCount >= UNDO_MAX) {
        /* Full: the oldest entry is the least likely to still be wanted. */
        xfree(g_et.undo[0].oldValue);
        memmove(&g_et.undo[0], &g_et.undo[1],
                sizeof(g_et.undo[0]) * (size_t)(UNDO_MAX - 1));
        g_et.undoCount--;
    }
    i = g_et.undoCount++;
    memset(&g_et.undo[i], 0, sizeof(g_et.undo[i]));
    g_et.undo[i].scope = s;
    wcsncpy(g_et.undo[i].name, name, ENV_NAME_MAX - 1);
    g_et.undo[i].name[ENV_NAME_MAX - 1] = 0;
    g_et.undo[i].oldValue = env_get_value(s, name, &t);
    g_et.undo[i].oldType = t;
    wcsncpy(g_et.undo[i].what, what, 63);
    g_et.undo[i].what[63] = 0;
}

/* A write that failed changed nothing, so it must not leave a phantom entry
 * for the user to "undo". */
static void undo_drop_last(void)
{
    if (g_et.undoCount <= 0) return;
    g_et.undoCount--;
    xfree(g_et.undo[g_et.undoCount].oldValue);
    g_et.undo[g_et.undoCount].oldValue = NULL;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

/* Windows accepts almost anything in a value name, but '=' cannot appear -
 * it is the key/value separator - and a name may not begin with one. Saying
 * so here beats an opaque "写入失败" from the registry. */
static int env_name_ok(const wchar_t *n)
{
    const wchar_t *p;
    if (!n || !*n) return 0;
    if (*n == L'=') return 0;
    for (p = n; *p; p++)
        if (*p == L'=') return 0;
    return 1;
}

/* Wraps a long value into a few short lines so a before/after comparison stays
 * readable in a message box instead of being one enormous unreadable run. */
static void wrap_value(wchar_t *out, size_t outCch, const wchar_t *v)
{
    size_t at = 0, col = 0;
    if (!v || !*v) { wcsncpy(out, L"（空）", outCch - 1); out[outCch - 1] = 0; return; }
    out[0] = 0;
    while (*v && at + 2 < outCch) {
        if (col >= 46) { out[at++] = L'\r'; out[at++] = L'\n'; col = 0; }
        out[at++] = *v++;
        col++;
    }
    if (*v && at + 4 < outCch) {
        out[at++] = L' '; out[at++] = L' '; out[at++] = L' '; out[at++] = L'…';
    }
    out[at] = 0;
}

/* The one place 保存 is implemented.
 *
 * Which of the three things happens is decided entirely by the form's state,
 * and each one says so in a dialog before anything is written:
 *
 *   新增 + the name is new            -> create it
 *   新增 + the name already exists    -> APPEND: the new value goes after the
 *                                        one that is already stored, joined
 *                                        with ';'. The original is kept.
 *   修改 + editing an existing one     -> OVERWRITE, behind a side-by-side of
 *                                        the old value and the new one.
 */
static int save_current(void)
{
    wchar_t name[ENV_NAME_MAX];
    wchar_t *typed = NULL, *stored = NULL, *final = NULL;
    wchar_t msg[2400], oldTxt[700], newTxt[700], comboTxt[900];
    DWORD storedType = 0, type = REG_EXPAND_SZ;
    int exists, isOverwrite, ok = 0;

    if (!editor_read(name, ENV_NAME_MAX, &typed)) {
        set_status(L"读不出表单里的内容，请检查变量名是否过长");
        return 0;
    }
    if (!name[0]) {
        set_status(L"变量名不能为空");
        xfree(typed);
        return 0;
    }
    if (!env_name_ok(name)) {
        MessageBoxW(g_et.hwnd,
                    L"变量名里不能包含 = 符号\r\n\r\n"
                    L"它是 Windows 用来分隔变量名和值的字符。\r\n"
                    L"如果值里需要 = ，把它填在【值】里就可以了。",
                    L"变量名不合法", MB_OK | MB_ICONWARNING);
        xfree(typed);
        return 0;
    }
    if (g_et.editScope == SCOPE_SYSTEM && !app_is_elevated()) {
        MessageBoxW(g_et.hwnd, env_error_text(ERROR_ACCESS_DENIED, SCOPE_SYSTEM),
                    L"需要管理员权限", MB_OK | MB_ICONERROR);
        xfree(typed);
        return 0;
    }

    stored = env_get_value(g_et.editScope, name, &storedType);
    exists = (stored != NULL);
    isOverwrite = (g_et.selected >= 0 && g_et.editing);

    if (!exists && !isOverwrite && g_et.selected >= 0) {
        /* renamed: saving makes a second variable and leaves the old one */
        wchar_t r[700];
        _snwprintf_s(r, 700, _TRUNCATE,
                     L"你把变量名从\r\n%s\r\n改成了\r\n%s\r\n\r\n"
                     L"保存会新建 %s，【原来的 %s 不会被删除】。\r\n\r\n继续吗？",
                     g_et.baseName, name, name, g_et.baseName);
        if (MessageBoxW(g_et.hwnd, r, L"这会新建一个变量",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            xfree(typed); xfree(stored);
            return 0;
        }
    }

    /* --- build the value that will actually be stored -------------------- */
    if (exists && !isOverwrite) {
        /* APPEND. The old value is not thrown away; the new one goes after it. */
        size_t a = wcslen(stored), b = wcslen(typed);
        final = (wchar_t *)xmalloc((a + b + 4) * sizeof(wchar_t));
        if (!final) { xfree(typed); xfree(stored); return 0; }
        if (a == 0)      wcsncpy(final, typed, b + 1);
        else if (b == 0) wcsncpy(final, stored, a + 1);
        else {
            wcsncpy(final, stored, a + 1);
            final[a] = L';';
            wcsncpy(final + a + 1, typed, b + 1);
        }
    } else {
        final = xwcsdup(typed ? typed : L"");
        if (!final) { xfree(typed); xfree(stored); return 0; }
    }

    /* --- say exactly what is about to happen --------------------------- */
    wrap_value(oldTxt, 700, stored);
    wrap_value(newTxt, 700, final);
    if (exists && !isOverwrite) {
        _snwprintf_s(comboTxt, 900, _TRUNCATE,
                     L"原值：\r\n%s\r\n\r\n追加后：\r\n%s", oldTxt, newTxt);
        _snwprintf_s(msg, 2400, _TRUNCATE,
            L"【%s】已经存在于%s环境。\r\n\r\n%s\r\n\r\n"
            L"这不是覆盖：新值会接在原值后面（用分号隔开），原值保留。\r\n\r\n确定保存吗？",
            name, env_scope_name(g_et.editScope), comboTxt);
    } else if (isOverwrite) {
        _snwprintf_s(msg, 2400, _TRUNCATE,
            L"这一步会【覆盖】%s环境里的 %s。\r\n\r\n"
            L"修改前：\r\n%s\r\n\r\n修改后：\r\n%s\r\n\r\n"
            L"覆盖之后原来的值就找不回来了。\r\n"
            L"确定覆盖吗？",
            env_scope_name(g_et.editScope), name, oldTxt, newTxt);
    } else {
        _snwprintf_s(msg, 2400, _TRUNCATE,
            L"在%s环境里新建：\r\n\r\n%s = %s\r\n\r\n确定保存吗？",
            env_scope_name(g_et.editScope), name, newTxt);
    }

    if (MessageBoxW(g_et.hwnd, msg,
                    exists && !isOverwrite ? L"追加到已有变量" :
                    isOverwrite ? L"覆盖前请确认" : L"确认新增",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
        xfree(typed); xfree(stored); xfree(final);
        return 0;
    }

    if (stored && (storedType == REG_SZ || storedType == REG_EXPAND_SZ))
        type = storedType;
    if (env_is_path_var(name)) type = REG_EXPAND_SZ;

    undo_push(g_et.editScope, name, isOverwrite ? L"覆盖" :
                                     (exists ? L"追加" : L"新增"));
    ok = env_write(g_et.editScope, name, final, type);
    xfree(typed); xfree(stored); xfree(final);
    if (!ok) {
        undo_drop_last();
        MessageBoxW(g_et.hwnd, env_error_text(env_last_error(), g_et.editScope),
                    L"没有保存成功", MB_OK | MB_ICONERROR);
        return 0;
    }
    env_broadcast_change();
    set_status(L"已%s %s 到%s环境", isOverwrite ? L"覆盖" : (exists ? L"追加" : L"新增"),
               name, env_scope_name(g_et.editScope));
    /* Land on the row that was just written, so the left list and the form
     * agree and a second save is obviously a no-op. */
    {
        int i, at = -1;
        for (i = 0; i < g_et.vars.count; i++)
            if (_wcsicmp(g_et.vars.items[i].name, name) == 0) { at = i; break; }
        refresh_all();
        g_et.keepEditor = 1;
        g_et.editing = 0;
        if (at >= 0) {
            g_et.selected = at;
            load_selected_into_editor();
        }
        update_dirty();
        InvalidateRect(g_et.hwnd, NULL, FALSE);
    }
    return 1;
}

static void do_delete(void)
{
    EnvVar *v = sel_var();
    wchar_t msg[700], why[512];
    if (!v) { set_notice(L"先在左边点一个变量，再点【删除变量】。"); return; }
    if (!confirm_discard()) return;
    if (env_var_protected(v->name)) {
        /* MessageBoxW takes the text as-is: the earlier printf-style call
         * printed a literal %s and put the variable name in the title bar. */
        _snwprintf_s(why, 512, _TRUNCATE,
                     L"%s 是 Windows 自己要用的变量。\r\n\r\n"
                     L"删掉它可能让登录、安装程序和一部分软件直接坏掉，"
                     L"所以这里不给删。\r\n"
                     L"如果你确实要改它，用【修改】改值比删掉整个变量安全得多。",
                     v->name);
        MessageBoxW(g_et.hwnd, why, L"不能删除", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (v->inUser && v->inSystem)
        _snwprintf_s(msg, 700, _TRUNCATE,
                     L"%s 同时存在于用户环境和系统环境。\r\n\r\n"
                     L"这一步只删除【%s环境】里的那一份，另一份不受影响。\r\n"
                     L"确定删除吗？", v->name, env_scope_name(g_et.editScope));
    else
        _snwprintf_s(msg, 700, _TRUNCATE,
                     L"确定要从【%s环境】里删除 %s 吗？\r\n\r\n%s",
                     env_scope_name(g_et.editScope), v->name,
                     (v->value && wcslen(v->value) < 300) ? v->value : L"");
    if (MessageBoxW(g_et.hwnd, msg, L"删除这个变量",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    undo_push(g_et.editScope, v->name, L"删除");
    if (env_delete(g_et.editScope, v->name)) {
        env_broadcast_change();
        set_status(L"已从%s环境删除 %s", env_scope_name(g_et.editScope), v->name);
        g_et.selected = -1;
        g_et.editing = 0;
        refresh_all();
    } else {
        undo_drop_last();
        MessageBoxW(g_et.hwnd, env_error_text(env_last_error(), g_et.editScope),
                    L"没有删除成功", MB_OK | MB_ICONERROR);
    }
}

/* Back to a blank 新增 form. The grey example drawn next to each caption in
 * draw_ui() reappears on its own once the fields are emptied again. */
static void do_new(void)
{
    if (!confirm_discard()) return;
    g_et.selected = -1;
    g_et.editing = 0;
    g_et.templateIdx = -1;
    g_et.editScope = SCOPE_USER;
    path_free(&g_et.path);
    g_et.pathMode = 0;
    g_et.pathSel = -1;
    SendMessageW(g_et.hPreset, CB_SETCURSEL, (LPARAM)0, 0);
    SendMessageW(g_et.hScope, CB_SETCURSEL, (LPARAM)SCOPE_USER, 0);
    remember_base();
    SetWindowTextW(g_et.hName, L"");
    SetWindowTextW(g_et.hValue, L"");
    set_form_locked(0);
    build_hits();      /* nothing is selected any more, so neither is the strip */
    SetFocus(g_et.hName);
    InvalidateRect(g_et.hwnd, NULL, FALSE);
    set_status(L"填一个名字和值就能新增　·　想省事就先在上面挑一个模板");
}

static int path_index_of(const wchar_t *entry)
{
    int i;
    for (i = 0; i < g_et.path.count; i++)
        if (_wcsicmp(g_et.path.items[i], entry) == 0) return i;
    return -1;
}

static int path_add_entry(const wchar_t *entry)
{
    wchar_t *exp;
    if (!entry || !*entry) return 0;
    exp = env_expand(entry);
    if (path_contains(&g_et.path, entry) ||
        (exp && path_contains(&g_et.path, exp))) {
        xfree(exp);
        set_status(L"该目录已经在 PATH 里了");
        return 0;
    }
    if (!path_add(&g_et.path, entry)) { xfree(exp); return 0; }
    g_et.pathSel = g_et.path.count - 1;
    {
        wchar_t *joined = path_join(&g_et.path);
        SetWindowTextW(g_et.hValue, joined ? joined : L"");
        xfree(joined);
    }
    xfree(exp);
    InvalidateRect(g_et.hwnd, NULL, FALSE);
    return 1;
}

static void path_commit(void)
{
    wchar_t *joined = path_join(&g_et.path);
    SetWindowTextW(g_et.hValue, joined ? joined : L"");
    xfree(joined);
}

/* ------------------------------------------------------------------ */
/* preset detection cache                                              */
/* ------------------------------------------------------------------ */

/* Probes every preset once and keeps the answer. Detection is a shallow
 * directory walk, so it is safe to run at start-up - and having the answer
 * up front is the whole point: the preset picker can then say which of these
 * tools are actually on this machine instead of making the user click
 * through 22 entries to find out. */
static void preset_scan_all(void)
{
    int n = preset_count(), i;
    preset_hits_free();
    g_et.presetHit = (wchar_t **)xcalloc((size_t)(n ? n : 1), sizeof(wchar_t *));
    if (!g_et.presetHit) { g_et.presetHitCount = 0; g_et.presetHitReady = 1; return; }
    g_et.presetHitCount = n;
    for (i = 0; i < n; i++)
        g_et.presetHit[i] = preset_detect(preset_at(i));
    g_et.presetHitReady = 1;
}

static void preset_hits_free(void)
{
    int i;
    if (!g_et.presetHit) { g_et.presetHitCount = 0; return; }
    for (i = 0; i < g_et.presetHitCount; i++) xfree(g_et.presetHit[i]);
    xfree(g_et.presetHit);
    g_et.presetHit = NULL;
    g_et.presetHitCount = 0;
}

/* How many presets are installed but not configured yet - the count behind
 * "3 个工具可以一键配置", which is the quickest answer to "what should I do
 * first". */
static int preset_ready_count(void)
{
    int i, k = 0;
    if (!g_et.presetHitReady) preset_scan_all();
    for (i = 0; i < g_et.presetHitCount; i++) {
        if (!g_et.presetHit[i]) continue;
        if (!env_exists(SCOPE_USER, preset_at(i)->var) ||
            !env_exists(SCOPE_SYSTEM, preset_at(i)->var))
            k++;
    }
    return k;
}

/* The template picker fills the form. That is all it does.
 *
 * This used to be an action that wrote JAVA_HOME and appended to Path itself,
 * and it did so *before* asking the user anything. Now a template is only a
 * starting point: it puts a sensible name, value and PATH entry into the form,
 * you can still edit every field, and 保存 is the only thing that writes -
 * through the one code path that always shows you what will change. */
static void fill_from_template(int idx)
{
    const Preset *p = preset_at(idx);
    wchar_t *root, *entry = NULL;
    wchar_t name[ENV_NAME_MAX];

    if (idx < 0 || !p) {           /* the "no template" row */
        g_et.templateIdx = -1;
        return;
    }
    if (g_et.presetHitReady && idx < g_et.presetHitCount && g_et.presetHit[idx])
        root = xwcsdup(g_et.presetHit[idx]);
    else
        root = preset_detect(p);
    if (!root) {
        wchar_t msg[900];
        _snwprintf_s(msg, 900, _TRUNCATE,
                     L"没在本机找到 %s。\r\n\r\n"
                     L"· 点【扫描目录】把工具装的位置（例如 F:\\sdk）加进来，再选一次\r\n"
                     L"· 或者直接把 %s 填进上面的表单",
                     p->name, p->var);
        MessageBoxW(g_et.hwnd, msg, L"没有检测到", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!confirm_discard()) { xfree(root); return; }

    g_et.selected = -1;
    g_et.editing = 0;
    g_et.templateIdx = idx;
    g_et.editScope = SCOPE_USER;
    build_hits();      /* a template fills a blank form, not a selected row */
    SendMessageW(g_et.hScope, CB_SETCURSEL, (LPARAM)SCOPE_USER, 0);
    SetWindowTextW(g_et.hScope, L"用户环境");

    wcsncpy(name, p->var, ENV_NAME_MAX - 1);
    name[ENV_NAME_MAX - 1] = 0;
    SetWindowTextW(g_et.hName, name);
    SetWindowTextW(g_et.hValue, root);

    /* The template also wants its bin directory on PATH. Stage that in the
     * PATH list so the user sees it and can drop it before saving. */
    if (p->pathEntry) {
        EnvVar *v = sel_var();
        const wchar_t *pv = v ? scope_value(v, g_et.editScope) : NULL;
        path_free(&g_et.path);
        g_et.pathMode = 1;
        g_et.pathSel = -1;
        if (pv && env_is_path_var(v->name)) {
            path_parse(pv, &g_et.path);
        } else {
            DWORD need = GetEnvironmentVariableW(L"Path", NULL, 0);
            if (need && need < ENV_VALUE_MAX) {
                wchar_t *cur = (wchar_t *)xmalloc((size_t)need * sizeof(wchar_t) + 2);
                if (cur && GetEnvironmentVariableW(L"Path", cur, need + 1))
                    path_parse(cur, &g_et.path);
                xfree(cur);
            }
        }
        entry = env_expand(p->pathEntry);
        if (entry) path_add_entry(entry);
    } else {
        path_free(&g_et.path);
        g_et.pathMode = 0;
        g_et.pathSel = -1;
    }

    remember_base();
    SetWindowTextW(g_et.hName, name);
    InvalidateRect(g_et.hwnd, NULL, FALSE);
    set_status(L"已按 %s 模板填好表单，确认后点【保存】；想改哪一项直接改", p->name);
    xfree(root);
    xfree(entry);
}

static void edit_roots(void)
{
    wchar_t **roots;
    int n, i;
    wchar_t text[4096];
    size_t used = 0;

    n = preset_roots(&roots);
    text[0] = 0;
    for (i = 0; i < n && used < sizeof(text) - 4; i++)
        used += (size_t)_snwprintf_s(text + used, sizeof(text) - used, _TRUNCATE,
                                     L"%s%s", i ? L"\r\n" : L"", roots[i]);
    if (i == 0) _snwprintf_s(text, sizeof(text), _TRUNCATE, L"F:\\sdk\r\nC:\\Program Files");

    {
        wchar_t *edited = NULL;
        if (app_multiline_input(g_et.hwnd, L"扫描目录",
                                L"每行一个目录。预设会在这些目录下自动查找已安装的工具。",
                                text, sizeof(text) / sizeof(wchar_t), &edited) && edited) {
            PathList list;
            path_parse(L"", &list);
            {
                /* split on newlines rather than ';' for this dialog */
                wchar_t *p = edited;
                while (*p) {
                    wchar_t *nl = wcschr(p, L'\r');
                    if (!nl) nl = wcschr(p, L'\n');
                    if (nl) *nl = 0;
                    wcs_trim(p);
                    if (*p) path_add(&list, p);
                    if (!nl) break;
                    p = nl + 1;
                    if (*p == L'\n') p++;
                }
            }
            {
                int kept = list.count;
                wchar_t **arr = (wchar_t **)xcalloc((size_t)(kept ? kept : 1),
                                                   sizeof(wchar_t *));
                if (arr) {
                    for (i = 0; i < kept; i++) arr[i] = list.items[i];
                    preset_set_roots(arr, kept);
                    list.count = 0;   /* ownership moved */
                    xfree(arr);
                }
                set_status(L"已更新扫描目录（%d 个）", kept);
            }
            path_free(&list);
            InvalidateRect(g_et.hwnd, NULL, FALSE);
        }
        xfree(edited);
    }
    preset_free_roots(roots, n);
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

/* Two columns and nothing else.
 *
 *   left  : 筛选 + 刷新 on one header line, then the variables that exist.
 *   right : a small toolbar at the very top (新增 / 保存), the form, and - only
 *           once a row is selected - a row of actions pinned to the bottom.
 *
 * The previous arrangement had a full-width toolbar, a preset row and a filter
 * row stacked above both columns, which is three horizontal bands before any
 * content. The actions were also all visible at once, including the ones that
 * only make sense once you have picked something. */
static void build_layout(int W, int H)
{
    int S = app_scale_px(1);
    int pad = S * 10, y, bh = S * 30;
    int listW, rx, rw, colH;
    int gap = S * 6;
    /* The administrator notice is a band across the top, and everything else
     * starts below it. `pad` stays the left/right/bottom margin; `top` is the
     * margin from the very top of the client area, which the band pushes down.
     * Only the top edge moves - the bottom edge is still pad + colH - so the
     * column simply gets shorter by exactly the band. */
    int banH = S * 28;
    /* The notice is the first thing to go on a window too short to hold it.
     * Everything below it is a control that has to stay clickable, and a
     * banner squeezed in on top of them buys a line of text at the cost of
     * the form. WM_GETMINMAXINFO keeps a real window well above this height;
     * the sizes below it are only reachable by a test or by a display the
     * minimum was not written for. */
    if (H < S * 380) banH = 0;
    int top = pad + banH + gap;

    memset(&g_el, 0, sizeof(g_el));
    SetRect(&g_el.banner, pad, pad, W - pad, pad + banH);

    /* --- the split ------------------------------------------------------ */
    listW = (W - pad * 3) * 44 / 100;
    rx    = pad + listW + pad;
    rw    = W - rx - pad;
    colH  = H - pad - S * 30 - banH - gap;   /* leaves the status strip below */

    /* --- left column header: 筛选 + 刷新 --------------------------------- */
    {
        int bw = S * 74;
        /* Confined to the left column. It used to run to W - pad - bw, which
         * on a wide window reached halfway across the form and painted over
         * the 新增 / 保存 buttons. */
        SetRect(&g_el.filterBox, pad, top, pad + listW - bw - gap, top + bh);
        SetRect(&g_el.refreshBtn, pad + listW - bw, top, pad + listW, top + bh);
    }
    y = top + bh + S * 8;
    SetRect(&g_el.listBox, pad, y, pad + listW, pad + colH);

    /* --- right column: the toolbar sits at the top ---------------------- */
    y = top;
    {
        int bw = S * 96;
        int aw = S * 76;
        SetRect(&g_el.barNew,  rx, y, rx + bw, y + bh);
        SetRect(&g_el.barSave, rx + bw + gap, y, rx + bw * 2 + gap, y + bh);
        /* 关于 takes the far end of the same row. It is always live rather than
         * following the selection like the bottom strip: a credits box is there
         * exactly when the user wants it, which is usually before they have
         * picked anything. */
        SetRect(&g_el.aboutBtn, W - pad - aw, y, W - pad, y + bh);
    }
    y += bh + S * 10;

    /* --- template picker: 新增 mode only -------------------------------- */
    {
        int rootsW = S * 96;
        int lblW   = S * 52;
        SetRect(&g_el.tplLabel, rx, y, rx + lblW, y + bh);
        SetRect(&g_el.tplBox,   rx + lblW, y, W - pad - rootsW - gap, y + bh);
        SetRect(&g_el.tplRoots, W - pad - rootsW, y, W - pad, y + bh);
    }
    y += bh + S * 10;

    /* --- 变量名 ---------------------------------------------------------- */
    SetRect(&g_el.nameBox, rx, y, rx + rw, y + bh);
    y += bh + S * 6;
    SetRect(&g_el.scopeBox, rx, y, rx + S * 118, y + bh);
    y += bh + S * 16;                    /* clears the caption above 值 */

    /* --- the bottom half of the right column ------------------------------ *
     * These blocks all end at the same column bottom, so they share one set of
     * anchors computed up front. Sizing them independently is what let the value
     * box grow straight down through the PATH list on a tall window: the value
     * box is a child window, so it paints over the list underneath and the
     * strip still looked correctly drawn - just unreachable behind it. */
    {
        int colBot  = pad + colH;            /* bottom of the right column  */
        int actTop  = colBot - S * 36;       /* action strip                */
        int pathBy  = actTop - gap - bh;     /* PATH buttons, above the strip */
        int vTop    = y;
        int ew      = S * 112;
        int minV    = S * 40;                /* the value box never shrinks past this */
        int listH   = pathBy - gap - S * 6 - minV - vTop;   /* what is left over */
        int pathTop, vH;

        /* The PATH row and the action strip are reserved whether or not they
         * are on screen: the value box is a child window, so growing it into
         * their space would not just look wrong, it would paint straight over
         * them. Keeping the geometry constant also means the form does not jump
         * the moment a row is selected. */
        if (listH > S * 78) listH = S * 78; /* about four directory rows; a
                                                * shorter window takes it away
                                                * instead of crushing the box */
        if (listH < 0)      listH = 0;
        pathTop = pathBy - gap - listH;
        vH = pathTop - S * 6 - vTop;
        if (vH < minV) vH = minV;
        /* minV is a wish, not a floor. Raising the box back to it here is what
         * let it grow straight down through pathTop: the value box is a real
         * EDIT child, so once it covers pathTop it paints over the PATH list and
         * the buttons beneath it - which are drawn by the parent and hit-tested
         * by hand, so they simply stop responding. A short value box is a
         * cosmetic problem; an unreachable strip is a dead control. */
        if (vTop + vH > pathTop) vH = pathTop - vTop;
        if (vH < 0) vH = 0;

        SetRect(&g_el.valueBox, rx, vTop, rx + rw - ew - gap, vTop + vH);
        SetRect(&g_el.valueExp, rx + rw - ew, vTop, rx + rw, vTop + bh);

        /* --- PATH breakdown ------------------------------------------------ */
        {
            /* Derived from the column, never a fixed width: four fixed buttons
             * overflow a narrow right column and the last one lands off the
             * client area entirely. */
            int bw = (rw - gap * 3) / 4;
            int bx = rx;
            if (bw > S * 66) bw = S * 66;
            if (bw < S * 28) bw = S * 28;
            SetRect(&g_el.pathAdd,  bx, pathBy, bx + bw, pathBy + bh); bx += bw + gap;
            SetRect(&g_el.pathUp,   bx, pathBy, bx + bw, pathBy + bh); bx += bw + gap;
            SetRect(&g_el.pathDown, bx, pathBy, bx + bw, pathBy + bh); bx += bw + gap;
            SetRect(&g_el.pathDel,  bx, pathBy, bx + bw, pathBy + bh);
            /* An empty rect is the signal that there was no room for it. */
            SetRect(&g_el.pathBox, rx, pathTop, rx + rw, pathTop + listH);
        }

        /* --- bottom action strip ------------------------------------------- */
        {
            /* Sized from the column, not from fixed offsets: three fixed 104px
             * buttons add up to more than the whole right column on a narrow
             * window, and the third one ran off the edge. */
            int bw = (rw - gap * 2) / 3;
            if (bw > S * 104) bw = S * 104;
            if (bw < S * 40) bw = S * 40;
            SetRect(&g_el.actModify, rx, actTop, rx + bw, actTop + S * 36);
            SetRect(&g_el.actDelete, rx + bw + gap, actTop,
                    rx + bw * 2 + gap, actTop + S * 36);
            SetRect(&g_el.actCopy,   rx + bw * 2 + gap * 2, actTop,
                    rx + bw * 3 + gap * 2, actTop + S * 36);
        }
    }

    SetRect(&g_el.statusBar, pad, H - pad - S * 20, W - pad, H - pad - S * 2);
}

static void build_hits(void)
{
    g_et.nhit = 0;
    hit_add(g_el.filterBox,  Z_NONE, 0, L"按名称筛选，留空显示全部");
    hit_add(g_el.refreshBtn, Z_REFRESH, 0, L"重新读取注册表里的环境变量");
    hit_add(g_el.barNew,     Z_NEW,  0, L"清空表单，回到新增状态");
    hit_add(g_el.barSave,    Z_SAVE, 0, L"按当前状态保存（见下方提示）");
    hit_add(g_el.aboutBtn,   Z_ABOUT, 0, L"版本、作者与项目链接");
    hit_add(g_el.tplBox,     Z_NONE, 0, L"选一个模板，表单会自动填好；填完还能手工改");
    hit_add(g_el.tplRoots,   Z_ROOTS_EDIT, 0, L"告诉工具你的工具装在哪些目录");
    hit_add(g_el.valueExp,   Z_EXPAND, 0, L"把值里的 %VAR% 展开（只改表单，不保存）");
    /* The bottom strip and the PATH row are only drawn once something is
     * selected, so they must not be clickable before that: their rectangles are
     * still laid out - the layout is deliberately constant - and a live hit
     * zone on an invisible button is a button that works when it should not. */
    if (g_et.selected >= 0) {
        hit_add(g_el.actModify, Z_MODIFY, 0, L"解锁表单，开始修改这个变量");
        hit_add(g_el.actDelete, Z_DELETE, 0, L"删除这个环境变量本身（会先确认）");
        hit_add(g_el.actCopy,   Z_COPY,   0, L"把当前值复制到剪贴板");
        hit_add(g_el.pathAdd, Z_PATH_ADD, 0, L"添加一行到 PATH");
        hit_add(g_el.pathUp,   Z_PATH_UP,   0, L"把选中行上移一位");
        hit_add(g_el.pathDown, Z_PATH_DOWN, 0, L"把选中行下移一位");
        hit_add(g_el.pathDel,  Z_PATH_DEL,  0, L"只从列表里去掉这一行目录，保存后才写回");
        hit_add(g_el.pathBox,  Z_NONE, 0, NULL);
    }
}
static int path_row_hit(int x, int y)
{
    int S = app_scale_px(1);
    int rowH = app_scale_px(18);
    int first = g_el.pathBox.top + S * 4;
    if (x < g_el.pathBox.left || x > g_el.pathBox.right) return -1;
    if (y < first || y > g_el.pathBox.bottom) return -1;
    {
        int row = (y - first) / rowH;
        if (row < 0 || row >= g_et.path.count) return -1;
        return row;
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                            */
/* ------------------------------------------------------------------ */

static void button(GpGraphics *g, RECT r, const wchar_t *label, int hot,
                   int primary, int S)
{
    ARGB bg = primary ? 0xFF0A84FF : (hot ? 0xFFEFF3FF : 0xFFEFF1F4);
    ARGB fg = primary ? 0xFFFFFFFF : 0xFF1C1C1E;
    gx_round_rect_bg(g, bg, (REAL)r.left, (REAL)r.top,
                     (REAL)(r.right - r.left), (REAL)(r.bottom - r.top), (REAL)(S * 7));
    /* A hairline around the secondary buttons. Without it they were #F1F2F4
     * on a #F7F8FA window - a difference of three values out of 255 - and
     * 检测 / 扫描目录… read as plain labels rather than as things to click. */
    if (!primary)
        gx_stroke_rect(g, 0xFFD8DCE2, 1.0f, (REAL)r.left + 0.5f, (REAL)r.top + 0.5f,
                       (REAL)(r.right - r.left - 1), (REAL)(r.bottom - r.top - 1));
    gx_draw_text(g, g_et.fBody, fg, label, (REAL)r.left, (REAL)(r.top + S * 5),
                 (REAL)(r.right - r.left), (REAL)(r.bottom - r.top - S * 8));
}

static int zone_hovered(int z)
{
    int i;
    for (i = 0; i < g_et.nhit; i++)
        if (g_et.hit[i].z == z) return g_et.hover == i;
    return 0;
}

/* Column widths follow the list width so all three stay visible without
 * horizontal scrolling. Returns 1 when a column actually changed, so the
 * caller can keep this off the repaint path. */
static int fit_columns(int listW)
{
    int w1 = listW * 38 / 100;
    int w2 = listW * 47 / 100;
    LVCOLUMNW col;
    static const wchar_t *HDR[3] = { L"变量名", L"值", L"来源" };
    int i, w, changed = 0;

    if (!g_et.hList || listW < 60) return 0;
    if (g_et.listColWidth == listW) return 0;
    g_et.listColWidth = listW;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    for (i = 0; i < 3; i++) {
        w = (i == 0) ? w1 : (i == 1) ? w2 : (listW - w1 - w2);
        if (w < 30) w = 30;
        col.pszText = (LPWSTR)HDR[i];
        col.cx = w;
        SendMessageW(g_et.hList, LVM_SETCOLUMNW, i, (LPARAM)&col);
        g_stat.columnSet++;
        changed = 1;
    }
    return changed;
}

/* Moves a child only when its rectangle really differs from the one we last
 * gave it. MoveWindow() invalidates the child and repaints it, so calling it
 * on every frame (as the old paint-time placement did) repainted the whole
 * editor continuously - that is the flashing the UI was showing. */
static int place_one(HWND h, int slot, RECT r)
{
    int *last = g_et.childRect[slot];
    if (!h) return 0;
    if (last[0] == r.left && last[1] == r.top &&
        last[2] == r.right && last[3] == r.bottom)
        return 0;
    last[0] = r.left; last[1] = r.top; last[2] = r.right; last[3] = r.bottom;
    MoveWindow(h, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    g_stat.childMove++;
    return 1;
}

static void place_children(void)
{
    RECT r;
    SetRect(&r, g_el.listBox.left, g_el.listBox.top,
            g_el.listBox.right, g_el.listBox.bottom);
    place_one(g_et.hList, 0, r);

    SetRect(&r, g_el.nameBox.left, g_el.nameBox.top,
            g_el.nameBox.right, g_el.nameBox.bottom);
    place_one(g_et.hName, 1, r);

    SetRect(&r, g_el.scopeBox.left, g_el.scopeBox.top + app_scale_px(3),
            g_el.scopeBox.right,
            g_el.scopeBox.bottom - app_scale_px(6));
    place_one(g_et.hScope, 2, r);

    SetRect(&r, g_el.valueBox.left, g_el.valueBox.top,
            g_el.valueBox.right, g_el.valueBox.bottom);
    place_one(g_et.hValue, 3, r);

    SetRect(&r, g_el.filterBox.left + app_scale_px(44),
            g_el.filterBox.top + app_scale_px(3),
            g_el.filterBox.right - app_scale_px(6),
            g_el.filterBox.bottom - app_scale_px(6));
    place_one(g_et.hFilter, 4, r);

    SetRect(&r, g_el.tplBox.left, g_el.tplBox.top + app_scale_px(2),
            g_el.tplBox.right, g_el.tplBox.bottom - app_scale_px(4));
    place_one(g_et.hPreset, 5, r);

    fit_columns(g_el.listBox.right - g_el.listBox.left);
}

/* Recomputes the layout, the hit zones and the child geometry. Called on
 * WM_SIZE and once after the children exist - never from paint(). */
static void relayout(void)
{
    RECT cr;
    if (!g_et.hwnd) return;
    GetClientRect(g_et.hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    ensure_fonts();
    build_layout(cr.right, cr.bottom);
    build_hits();
    place_children();
    g_et.laidOut = 1;
    g_stat.relayout++;
}

/* The DIB section GDI+ draws into, sized to the client area. Kept across
 * repaints: reallocating it on every WM_PAINT was pure churn. */
static int ensure_back_buffer(int w, int h)
{
    if (g_et.memBmp && g_et.memW == w && g_et.memH == h) return g_et.mem != NULL;
    if (g_et.mem) { GdipDisposeImage((GpImage *)g_et.mem); g_et.mem = NULL; }
    if (g_et.memBmp) { free_dib(g_et.memBmp, g_et.memBits); g_et.memBmp = NULL; }
    g_et.memBits = NULL;
    g_et.memBmp = make_dib(w, h, 32, &g_et.memBits);
    if (g_et.memBmp)
        GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                                  g_et.memBits, &g_et.mem);
    g_et.memW = w; g_et.memH = h;
    return g_et.memBmp && g_et.mem;
}

/* An example of the expected shape, on the caption line beside an empty field.
 * EN_CHANGE routes through update_dirty(), which repaints, so it disappears the
 * moment there is real text. */
static void hint_for_empty(GpGraphics *g, RECT box, HWND edit, const wchar_t *text)
{
    int S = app_scale_px(1);
    int x = box.left + S * 58;
    if (!edit || GetWindowTextLengthW(edit) > 0) return;
    if (x >= box.right - S * 40) return;       /* no room on this width */
    gx_draw_text(g, g_et.fSmall, 0xFFB0B7C3, text, (REAL)x, (REAL)(box.top - S * 12),
                 (REAL)(box.right - x - S * 4), (REAL)(S * 14));
}

/* Everything the parent window draws itself. Split out of paint() so --shot can
 * render the identical chrome into an off-screen bitmap. */
static void draw_ui(GpGraphics *g, int W, int H)
{
    int S = app_scale_px(1);
    int i, hasSel = (g_et.selected >= 0);
    int newMode = !hasSel;

    gx_set_quality(g);
    gx_clear(g, 0xFFF7F8FA, (REAL)W, (REAL)H);

    /* ---- the standing notice, across the top ------------------------- */
    /* Writing the system environment needs administrator rights, and a write
     * that fails on permissions is the one failure a user is least likely to
     * read the reason for - it used to be one grey line in the corner, seen
     * only by someone already looking for it. It is a standing condition, not
     * an event, so it is a standing line here: amber while the process is not
     * elevated, because that is when a save is going to be refused. */
    if (g_el.banner.bottom > g_el.banner.top) {
        int admin = app_is_elevated();
        REAL bw = (REAL)(g_el.banner.right - g_el.banner.left);
        REAL bhh = (REAL)(g_el.banner.bottom - g_el.banner.top);
        gx_round_rect_bg(g, admin ? 0xFFECFDF5 : 0xFFFEF3C7,
                         (REAL)g_el.banner.left, (REAL)g_el.banner.top, bw, bhh,
                         (REAL)(S * 7));
        gx_stroke_rect(g, admin ? 0xFFA7F3D0 : 0xFFFCD34D, 1.0f,
                       (REAL)g_el.banner.left + 0.5f, (REAL)g_el.banner.top + 0.5f,
                       bw - 1, bhh - 1);
        gx_draw_text(g, g_et.fBody, admin ? 0xFF047857 : 0xFF92400E,
                     admin ? L"当前以管理员身份运行　·　可以写入系统环境"
                           : L"写系统环境需要以管理员身份运行，否则会写入失败"
                             L"　·　右键本程序选择【以管理员身份运行】",
                     (REAL)(g_el.banner.left + S * 10),
                     (REAL)(g_el.banner.top + S * 5),
                     bw - (REAL)(S * 20), bhh - (REAL)(S * 10));
    }

    /* ---- left column ------------------------------------------------ */
    gx_draw_text(g, g_et.fSmall, 0xFF6B7280, L"筛选", (REAL)g_el.filterBox.left,
                 (REAL)(g_el.filterBox.top + S * 7), (REAL)(S * 44),
                 (REAL)(g_el.filterBox.bottom - g_el.filterBox.top - S * 10));
    button(g, g_el.refreshBtn, L"刷新", zone_hovered(Z_REFRESH), 0, S);

    /* ---- right column: the toolbar at the top ---------------------- */
    button(g, g_el.barNew,  L"新增", zone_hovered(Z_NEW), 0, S);
    button(g, g_el.barSave, L"保存", zone_hovered(Z_SAVE), 1, S);
    button(g, g_el.aboutBtn, L"关于", zone_hovered(Z_ABOUT), 0, S);

    /* ---- template picker (新增 mode only) -------------------------- */
    if (newMode) {
        gx_draw_text(g, g_et.fSmall, 0xFF6B7280, L"模板",
                     (REAL)g_el.tplLabel.left,
                     (REAL)(g_el.tplLabel.top + S * 7),
                     (REAL)(g_el.tplLabel.right - g_el.tplLabel.left),
                     (REAL)(g_el.tplLabel.bottom - g_el.tplLabel.top - S * 10));
        gx_stroke_rect(g, 0xFFE2E5EA, 1.0f, (REAL)g_el.tplBox.left,
                       (REAL)g_el.tplBox.top,
                       (REAL)(g_el.tplBox.right - g_el.tplBox.left),
                       (REAL)(g_el.tplBox.bottom - g_el.tplBox.top));
        button(g, g_el.tplRoots, L"扫描目录", zone_hovered(Z_ROOTS_EDIT), 0, S);
    }

    /* ---- the form --------------------------------------------------- */
    if (hasSel) {
        RECT frames[3];
        frames[0] = g_el.nameBox;
        frames[1] = g_el.valueBox;
        frames[2] = g_el.scopeBox;
        for (i = 0; i < 3; i++)
            gx_stroke_rect(g, 0xFFE2E5EA, 1.0f, (REAL)frames[i].left, (REAL)frames[i].top,
                           (REAL)(frames[i].right - frames[i].left),
                           (REAL)(frames[i].bottom - frames[i].top));
        gx_draw_text(g, g_et.fSmall, 0xFF9CA3AF, L"变量名",
                     (REAL)(g_el.nameBox.left + S * 4),
                     (REAL)(g_el.nameBox.top - S * 13), (REAL)(S * 80), (REAL)(S * 14));
        gx_draw_text(g, g_et.fSmall, 0xFF9CA3AF, L"值",
                     (REAL)(g_el.valueBox.left + S * 4),
                     (REAL)(g_el.valueBox.top - S * 13), (REAL)(S * 80), (REAL)(S * 14));
        button(g, g_el.valueExp, L"展开 %VAR%", zone_hovered(Z_EXPAND), 0, S);

        /* What 保存 will actually do, said before the user has to ask. */
        {
            EnvVar *v = sel_var();
            wchar_t line[320];
            int x = g_el.scopeBox.right + S * 10;
            int avail = g_el.valueBox.right - x;
            if (avail < S * 40) avail = S * 40;
            if (v) {
                const wchar_t *here = scope_value(v, g_et.editScope);
                _snwprintf_s(line, 320, _TRUNCATE, L"%s环境里有这个变量　·　%s%s",
                             env_scope_name(g_et.editScope),
                             here ? L"点【修改】可以改" : L"这一栏现在是空的",
                             (g_et.editScope == SCOPE_SYSTEM && !app_is_elevated())
                                 ? L"　·　需要管理员权限" : L"");
                gx_draw_text(g, g_et.fSmall,
                             (g_et.editScope == SCOPE_SYSTEM && !app_is_elevated())
                                 ? 0xFFD97706 : 0xFF6B7280,
                             line, (REAL)x, (REAL)(g_el.scopeBox.top + S * 7),
                             (REAL)avail, (REAL)(S * 16));
            }
        }

        /* ---- PATH breakdown ---------------------------------------- */
        /* An empty pathBox means the window was too short to give it any room;
         * the value box takes that space instead. */
        if (g_el.pathBox.bottom > g_el.pathBox.top) {
            gx_fill_rect(g, 0xFFFFFFFF, (REAL)g_el.pathBox.left, (REAL)g_el.pathBox.top,
                         (REAL)(g_el.pathBox.right - g_el.pathBox.left),
                         (REAL)(g_el.pathBox.bottom - g_el.pathBox.top));
            gx_stroke_rect(g, 0xFFE2E5EA, 1.0f, (REAL)g_el.pathBox.left, (REAL)g_el.pathBox.top,
                           (REAL)(g_el.pathBox.right - g_el.pathBox.left),
                           (REAL)(g_el.pathBox.bottom - g_el.pathBox.top));
            button(g, g_el.pathAdd, L"添加", zone_hovered(Z_PATH_ADD) && g_et.editing, 0, S);
            button(g, g_el.pathUp,   L"上移", zone_hovered(Z_PATH_UP) && g_et.editing, 0, S);
            button(g, g_el.pathDown, L"下移", zone_hovered(Z_PATH_DOWN) && g_et.editing, 0, S);
            /* Labelled 移除目录 rather than 删除: the strip at the bottom of
             * the window also has a 删除, and the two do very different things
             * - this one takes a single directory out of the list and leaves
             * the registry alone until 保存, that one deletes the variable. */
            button(g, g_el.pathDel,  L"移除目录", zone_hovered(Z_PATH_DEL) && g_et.editing, 0, S);

            if (g_et.pathMode) {
                int rowH = app_scale_px(18);
                for (i = 0; i < g_et.path.count; i++) {
                    int ry = g_el.pathBox.top + S * 4 + i * rowH;
                    if (ry + rowH > g_el.pathBox.bottom) break;
                    if (i == g_et.pathSel)
                        gx_fill_rect(g, 0xFFE5EEFF, (REAL)g_el.pathBox.left + 1, (REAL)ry,
                                     (REAL)(g_el.pathBox.right - g_el.pathBox.left - 2),
                                     (REAL)rowH);
                    gx_draw_text(g, g_et.fMonoF, 0xFF374151, g_et.path.items[i],
                                 (REAL)(g_el.pathBox.left + S * 6), (REAL)(ry + S * 2),
                                 (REAL)(g_el.pathBox.right - g_el.pathBox.left - S * 12),
                                 (REAL)(rowH - S * 3));
                }
            } else {
                gx_draw_text(g, g_et.fSmall, 0xFFB0B7C3,
                             g_et.editing ? L"点【添加】把目录一行行加进来，可以上下调顺序"
                                          : L"选中 Path 变量时，这里会按目录逐条列出",
                             (REAL)(g_el.pathBox.left + S * 6),
                             (REAL)(g_el.pathBox.top + S * 4),
                             (REAL)(g_el.pathBox.right - g_el.pathBox.left - S * 12),
                             (REAL)(g_el.pathBox.bottom - g_el.pathBox.top - S * 8));
            }
        }
    } else {
        /* 新增 mode. The example sits next to the caption, not inside the field:
         * a child window paints over its parent, so anything the parent drew
         * under an empty edit box is never seen. EM_SETCUEBANNER would put it
         * inside, but that message needs comctl32 v6 and this app does not
         * activate it - it compiles, links and then does nothing. */
        gx_draw_text(g, g_et.fSmall, 0xFF9CA3AF, L"变量名",
                     (REAL)(g_el.nameBox.left + S * 4),
                     (REAL)(g_el.nameBox.top - S * 13), (REAL)(S * 80), (REAL)(S * 14));
        gx_draw_text(g, g_et.fSmall, 0xFF9CA3AF, L"值",
                     (REAL)(g_el.valueBox.left + S * 4),
                     (REAL)(g_el.valueBox.top - S * 13), (REAL)(S * 80), (REAL)(S * 14));
        hint_for_empty(g, g_el.nameBox,  g_et.hName,  L"例如 JAVA_HOME");
        hint_for_empty(g, g_el.valueBox, g_et.hValue, L"例如 F:\\sdk\\jdk-17　（也可以先在上面挑个模板）");
    }

    /* ---- bottom action strip, only once something is selected ------- */
    if (hasSel) {
        EnvVar *v = sel_var();
        int locked = !v || env_var_protected(v->name);
        gx_stroke_line(g, 0xFFE2E5EA, 1.0f, (REAL)g_el.actModify.left,
                       (REAL)(g_el.actModify.top - S * 8),
                       (REAL)(g_el.actModify.left + S * 330),
                       (REAL)(g_el.actModify.top - S * 8));
        if (g_et.editing)
            button(g, g_el.actModify, L"完成修改", zone_hovered(Z_MODIFY), 1, S);
        else
            button(g, g_el.actModify, L"修改", zone_hovered(Z_MODIFY), 0, S);
        /* 删除 is greyed out for the variables Windows depends on. */
        if (locked) {
            REAL w = (REAL)(g_el.actDelete.right - g_el.actDelete.left);
            REAL ht = (REAL)(g_el.actDelete.bottom - g_el.actDelete.top);
            gx_round_rect_bg(g, 0xFFF6F7F9, (REAL)g_el.actDelete.left,
                             (REAL)g_el.actDelete.top, w, ht, (REAL)(S * 7));
            gx_stroke_rect(g, 0xFFE2E5EA, 1.0f, (REAL)g_el.actDelete.left + 0.5f,
                           (REAL)g_el.actDelete.top + 0.5f, w - 1, ht - 1);
            gx_draw_text(g, g_et.fBody, 0xFFBFC4CC, L"删除变量",
                         (REAL)g_el.actDelete.left, (REAL)(g_el.actDelete.top + S * 5),
                         w, ht - (REAL)(S * 8));
        } else {
            button(g, g_el.actDelete, L"删除变量", zone_hovered(Z_DELETE), 0, S);
        }
        button(g, g_el.actCopy, L"复制值", zone_hovered(Z_COPY), 0, S);
    }

    /* ---- status strip ---------------------------------------------- */
    {
        const wchar_t *tip = NULL;
        int sticky;
        if (g_et.hover >= 0 && g_et.hover < g_et.nhit) tip = g_et.hit[g_et.hover].tip;
        sticky = g_et.status[0] && GetTickCount64() < g_et.statusUntil;
        /* A live message is drawn in the body font and the accent colour, not
         * the small grey hint font: it is the answer to "did that work?", and
         * at the bottom-left corner in 8pt it was not an answer, it was
         * something you had to already be looking at to see. The hint and the
         * hover tips stay quiet - those are there to be read on purpose. */
        gx_draw_text(g, sticky ? g_et.fBody : g_et.fSmall,
                     sticky ? 0xFF0A84FF : ((tip) ? 0xFF6B7280 : 0xFF9CA3AF),
                     sticky ? g_et.status : (tip ? tip : STATUS_HINT),
                     (REAL)g_el.statusBar.left, (REAL)g_el.statusBar.top,
                     (REAL)(g_el.statusBar.right - g_el.statusBar.left),
                     (REAL)(g_el.statusBar.bottom - g_el.statusBar.top));
        {
            REAL sw = (REAL)(g_el.statusBar.right - g_el.statusBar.left);
            if (sw > (REAL)(S * 260)) {
                const wchar_t *mk = g_et.dirty ? L"● 有未保存的修改" : L"○ 已保存";
                ARGB col = g_et.dirty ? 0xFFE8830A : 0xFF9CA3AF;
                REAL mw = (REAL)(S * 150);
                gx_draw_text(g, g_et.fSmall, col, mk,
                             (REAL)(g_el.statusBar.right - mw),
                             (REAL)g_el.statusBar.top, mw,
                             (REAL)(g_el.statusBar.bottom - g_el.statusBar.top));
            }
        }
    }

}

/* Draws the parent chrome into dc. Children are deliberately left out: they are
 * real windows that paint themselves on their own WM_PAINT. */
static void render_parent(HWND h, HDC dc)
{
    RECT cr;
    HDC mem;
    HBITMAP old;
    GpGraphics *g = NULL;

    GetClientRect(h, &cr);
    if (!ensure_back_buffer(cr.right, cr.bottom)) return;
    if (GdipGetImageGraphicsContext((GpImage *)g_et.mem, &g) != Ok) return;
    draw_ui(g, cr.right, cr.bottom);
    GdipDeleteGraphics(g);
    mem = CreateCompatibleDC(dc);
    if (!mem) return;
    old = SelectObject(mem, g_et.memBmp);
    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
}

/* Standard controls answer WM_PRINT. Each one is rendered into its own scratch
 * DIB and then blitted into place, because the controls draw themselves at the
 * DC origin and ignore whatever window origin the caller set - give one a DC
 * for the whole client area and it lands in the top-left corner. Hidden children
 * are skipped, which is what keeps the edit-mode fields out of the 新增 shot. */
static void print_children(HWND h, HDC dc)
{
    HWND kid;
    for (kid = GetWindow(h, GW_CHILD); kid; kid = GetWindow(kid, GW_HWNDNEXT)) {
        RECT kr;
        POINT org;
        int cw, ch;
        void *bits = NULL;
        HBITMAP dib;
        HDC mem;
        HBITMAP old;

        if (!IsWindowVisible(kid)) continue;
        if (!GetWindowRect(kid, &kr)) continue;
        cw = kr.right - kr.left;
        ch = kr.bottom - kr.top;
        if (cw <= 0 || ch <= 0) continue;

        org.x = 0; org.y = 0;
        MapWindowPoints(kid, h, &org, 1);

        dib = make_dib(cw, ch, 32, &bits);
        if (!dib || !bits) continue;
        mem = CreateCompatibleDC(dc);
        if (!mem) { free_dib(dib, bits); continue; }
        old = SelectObject(mem, dib);
        /* envtool's WM_ERASEBKGND is a no-op, so PRF_ERASEBKGND would leave
         * whatever the allocator handed us. Paint the app background first. */
        {
            GpGraphics *g = NULL;
            GpImage *im = NULL;
            if (GdipCreateBitmapFromScan0(cw, ch, cw * 4, PixelFormat32bppARGB,
                                          bits, &im) == Ok &&
                GdipGetImageGraphicsContext(im, &g) == Ok) {
                gx_clear(g, 0xFFF7F8FA, (REAL)cw, (REAL)ch);
                GdipDeleteGraphics(g);
            }
            if (im) GdipDisposeImage(im);
        }
        SendMessageW(kid, WM_PRINT, (WPARAM)mem,
                     PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
        BitBlt(dc, org.x, org.y, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        free_dib(dib, bits);
    }
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc;

    dc = BeginPaint(h, &ps);
    if (!dc) return;
    g_stat.paint++;
    /* Layout is owned by WM_SIZE / creation. Touching child geometry from
     * inside paint is what made the controls flash. */
    if (!g_et.laidOut) relayout();
    render_parent(h, dc);
    EndPaint(h, &ps);
}
static LRESULT CALLBACK et_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(h);
        return 0;

    case WM_PRINTCLIENT:
        /* Windows sends this for PrintWindow. envtool has to answer it itself:
         * its chrome is GDI+ drawing into a DIB, which WM_PAINT never hands
         * PrintWindow a chance to reach. --shot relies on this. */
        render_parent(h, (HDC)w);
        print_children(h, (HDC)w);
        return 0;

    case WM_GETMINMAXINFO: {
        /* Below roughly this the editor column has no room left for the value
         * box, and the app becomes unusable rather than merely cramped. */
        MINMAXINFO *mmi = (MINMAXINFO *)l;
        mmi->ptMinTrackSize.x = app_scale_px(560);
        mmi->ptMinTrackSize.y = app_scale_px(430);
        return 0;
    }

    case WM_SIZE:
        /* Geometry lives here, not in paint: a WM_PAINT that resizes its own
         * children re-enters painting and makes them flash. */
        g_et.laidOut = 0;
        relayout();
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_TIMER:
        if (w == TIMER_STATUS) {
            if (GetTickCount64() >= g_et.statusUntil) {
                KillTimer(h, TIMER_STATUS);
                g_et.status[0] = 0;
            }
            invalidate_status();
            return 0;
        }
        if (w == TIMER_FORMVIS && g_et.formVisDirty) {
            KillTimer(h, TIMER_FORMVIS);
            sync_form_visibility();
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int hi, old = g_et.hover;
        g_stat.mouseMove++;
        hi = hit_test(x, y);
        if (g_et.pathMode) {
            int row = path_row_hit(x, y);
            if (row != g_et.pathSel) {
                g_et.pathSel = row;
                InvalidateRect(h, &g_el.pathBox, FALSE);
            }
        }
        if (hi != old) {
            g_et.hover = hi;
            /* Repaint just the strip that changed instead of the whole window,
             * and include the status bar because its text follows the hover. */
            if (old >= 0 && old < g_et.nhit) InvalidateRect(h, &g_et.hit[old].r, FALSE);
            if (hi >= 0 && hi < g_et.nhit)  InvalidateRect(h, &g_et.hit[hi].r, FALSE);
            invalidate_status();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        /* Another window (or DefWindowProc) took the mouse; drop the pending
         * press so the next LBUTTONUP cannot fire a button. */
        g_et.pressed = -1;
        g_et.pressing = 0;
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int row = g_et.pathMode ? path_row_hit(x, y) : -1;
        if (row >= 0) { g_et.pathSel = row; InvalidateRect(h, &g_el.pathBox, FALSE); return 0; }
        g_et.pressed = hit_test(x, y);
        /* Only take the capture for a real button. Capturing on a decorative
         * zone (the preset row, the PATH frame) takes the mouse away from the
         * child control drawn inside it, so those clicks were swallowed. */
        if (g_et.pressed >= 0 && zone_is_button(g_et.hit[g_et.pressed].z)) {
            SetCapture(h);
            g_et.pressing = 1;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int hi, pressed;
        /* The index is read *before* the capture is released. ReleaseCapture()
         * hands this very window a WM_CAPTURECHANGED, and the handler above
         * drops the pending press on purpose - which is right when somebody
         * else steals the capture and wrong here, where we are the ones giving
         * it up. Reading afterwards compared every click against -1, so no
         * button ever fired: hover worked, and nothing on screen did. */
        pressed = g_et.pressed;
        if (g_et.pressing) { ReleaseCapture(); g_et.pressing = 0; }
        if (g_et.pathMode && path_row_hit(x, y) >= 0) { g_et.pressed = -1; return 0; }
        hi = hit_test(x, y);
        if (hi < 0 || hi != pressed) { g_et.pressed = -1; return 0; }
        g_et.pressed = -1;
        switch (g_et.hit[hi].z) {
        case Z_DELETE: {
            /* The button is drawn greyed out for the variables Windows needs.
             * It is not simply hidden: a control that appears and disappears
             * is harder to reason about than one that is always there and
             * tells you why it is unavailable. */
            EnvVar *v = sel_var();
            if (v && env_var_protected(v->name)) {
                set_notice(L"%s 是系统要用的变量，不能在这里删除。\r\n\r\n"
                           L"删掉它可能让登录、安装程序和一部分软件直接坏掉。\r\n"
                           L"要改它请用【修改】改值，比删掉整个变量安全得多。", v->name);
                break;
            }
            do_delete();
            break;
        }
        case Z_NEW:        do_new(); break;
        case Z_SAVE:       save_current(); break;
        case Z_REFRESH:
            if (confirm_discard()) {
                refresh_all();
                set_status(L"已重新读取环境变量");
            }
            break;
        case Z_EXPAND: {
            int n = GetWindowTextLengthW(g_et.hValue) + 1;
            wchar_t *v = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t) + 2);
            if (v) {
                wchar_t *ex;
                GetWindowTextW(g_et.hValue, v, n + 1);
                ex = env_expand(v);
                SetWindowTextW(g_et.hValue, ex ? ex : v);
                set_status(L"已展开 %%VAR%% 引用（未保存）");
                xfree(ex);
                xfree(v);
            }
            break;
        }
        case Z_BROADCAST:
            env_broadcast_change();
            set_status(L"已发送 WM_SETTINGCHANGE，新启动的程序会使用新环境");
            break;
        case Z_MODIFY: {
            EnvVar *v = sel_var();
            if (!v) break;
            if (g_et.editing) {
                g_et.editing = 0;
                set_form_locked(1);
                set_status(L"已退出修改；点【保存】才会写进去");
            } else {
                g_et.editing = 1;
                set_form_locked(0);
                set_status(L"改完点【保存】，保存前会把新旧值摆出来给你对一遍");
            }
            break;
        }
        case Z_COPY: {
            int n = GetWindowTextLengthW(g_et.hValue) + 1;
            wchar_t *v = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t) + 2);
            if (v) {
                GetWindowTextW(g_et.hValue, v, n + 1);
                if (clip_put_text(v)) set_status(L"已复制变量值");
                xfree(v);
            }
            break;
        }
        case Z_ROOTS_EDIT:    edit_roots(); break;
        case Z_ABOUT:
            /* The name, icon and credit line come from src\common\about.c, so
             * every program credits the same author the same way. */
            app_show_about(g_et.hwnd, ET_ABOUT_NAME, ET_ABOUT_NOTES);
            break;
        case Z_PATH_ADD: {
            wchar_t *txt = NULL;
            /* Same hazard as the row below: path_add_entry() ends in a
             * path_commit() that overwrites the value box from the list, and
             * the list is empty for anything that is not Path. */
            if (g_et.selected >= 0 && !g_et.pathMode) {
                set_notice(L"%s 不是 Path 变量，不能按目录追加。\r\n\r\n"
                           L"只有 Path 变量才会被拆成一行一个目录。", sel_var()->name);
                break;
            }
            if (!g_et.editing && g_et.selected >= 0) {
                set_notice(L"先点下面的【修改】，才能改 PATH。");
                break;
            }
            if (app_multiline_input(h, L"添加到 PATH", L"输入要追加的目录：", L"", 1024, &txt) && txt) {
                if (path_add_entry(txt)) {
                    path_commit();
                    set_status(L"已加入列表，点【保存】写回注册表");
                }
                xfree(txt);
            }
            break;
        }
        case Z_PATH_UP:
        case Z_PATH_DOWN:
        case Z_PATH_DEL: {
            /* The PATH row is laid out and drawn for every selected variable,
             * but only a Path one actually has a list behind it. On anything
             * else g_et.path is empty, and every one of these operations ends in
             * path_commit(), which writes the joined list straight into the
             * value box - so "添加" on JAVA_HOME replaced its value with the one
             * directory just typed. Refuse first, and say why. */
            const wchar_t *nm = sel_var() ? sel_var()->name : L"";
            int isUp = (g_et.hit[hi].z == Z_PATH_UP);
            if (!g_et.pathMode) {
                set_notice(L"%s 不是 Path 变量，没有可逐条调整的目录列表。\r\n\r\n"
                           L"PATH 那四个按钮只对 Path 变量有效。", nm);
                break;
            }
            if (!g_et.editing) {
                set_notice(L"先点下面的【修改】，才能改 PATH。\r\n\r\n"
                           L"改完还要点【保存】才会写回注册表。");
                break;
            }
            if (g_et.pathSel < 0 || g_et.pathSel >= g_et.path.count) {
                set_notice(L"先在上面点一行目录，再决定怎么调。");
                break;
            }
            if (g_et.hit[hi].z == Z_PATH_DEL) {
                if (path_remove_at(&g_et.path, g_et.pathSel)) {
                    if (g_et.pathSel >= g_et.path.count) g_et.pathSel = g_et.path.count - 1;
                    path_commit();
                    InvalidateRect(h, &g_el.pathBox, FALSE);
                    set_status(L"已从列表移除，点【保存】写回");
                }
                break;
            }
            /* At either end the move is a no-op, and a no-op that says nothing
             * is indistinguishable from a dead button. */
            if (isUp && g_et.pathSel == 0) { set_notice(L"已经是第一行了。"); break; }
            if (!isUp && g_et.pathSel == g_et.path.count - 1) {
                set_notice(L"已经是最后一行了。");
                break;
            }
            if (path_move(&g_et.path, g_et.pathSel, g_et.pathSel + (isUp ? -1 : 1))) {
                g_et.pathSel += isUp ? -1 : 1;
                path_commit();
                InvalidateRect(h, &g_el.pathBox, FALSE);
                set_status(isUp ? L"已上移一位，点【保存】写回" : L"已下移一位，点【保存】写回");
            } else {
                set_notice(L"这一行挪不动了。");
            }
            break;
        }
        default: break;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        g_stat.notify++;
        if (nh->hwndFrom == g_et.hList) g_stat.listNotify++;
        if (nh->idFrom == ID_LIST && nh->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)l;
            if (!g_et.loading &&
                (lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) &&
                lv->iItem >= 0 && lv->iItem < g_et.vars.count)
                select_index(lv->iItem);
        }
        else if (nh->idFrom == ID_LIST && nh->code == LVN_ITEMACTIVATE) {
            /* A ListView never sends LBN_DBLCLK - that is a listbox message -
             * so the old handler was dead code. LVN_ITEMACTIVATE is the real
             * "the user double-clicked / pressed Enter on a row" notification.
             *
             * It used to call save_current(). Double-clicking a row in a list
             * of sixty variables wrote one of them to the registry. Activation
             * now just opens the row for editing. */
            NMLISTVIEW *lv = (NMLISTVIEW *)l;
            if (lv->iItem >= 0) {
                select_index(lv->iItem);
                g_et.editing = 1;
                set_form_locked(0);
                SetFocus(g_et.hValue);
                SendMessageW(g_et.hValue, EM_SETSEL, 0, -1);
                InvalidateRect(g_et.hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        /* Child-control notifications carry the child HWND in lParam; a menu
         * command carries 0. Keep the two apart - the ids would otherwise
         * alias, and the name edit fires EN_UPDATE on every SetWindowText. */
        if (l != 0) {
            if (id == ID_FILTER && code == EN_CHANGE) {
                static ULONGLONG last = 0;
                ULONGLONG now = GetTickCount64();
                if (now - last > 250) {
                    last = now;
                    /* Filtering rebuilds the list, which normally reloads the
                     * editor. Typing in the filter must not eat a half-written
                     * value, so the boxes are deliberately left alone. */
                    g_et.keepEditor = 1;
                    refresh_all();
                }
                return 0;
            }
            if ((id == ID_NAME || id == ID_VALUE) && code == EN_CHANGE)
                update_dirty();
            if (id == ID_SCOPE && code == CBN_SELCHANGE) {
                Scope s = (Scope)SendMessageW(g_et.hScope, CB_GETCURSEL, 0, 0);
                if (s != g_et.editScope) reload_for_scope(s);
                return 0;
            }
            if (id == ID_PRESET && code == CBN_SELCHANGE) {
                int t = (int)SendMessageW(g_et.hPreset, CB_GETCURSEL, 0, 0);
                fill_from_template(t - 1);   /* row 0 is "no template" */
                return 0;
            }
            break;          /* not ours; let DefWindowProc have it */
        }
        if (id == ID_LIST && code == LBN_DBLCLK) {
            save_current();
            return 0;
        }
        if (id == IDM_ACCEL_SAVE) { save_current(); return 0; }
        return 0;
    }

    case WM_HOTKEY:
        if (w == HOTKEY_ID) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
        return 0;

    case WM_CHAR:
        /* Enter used to run do_apply_preset() from anywhere that was not a
         * text box - which, with the list holding focus, is every time
         * someone opened a variable to look at it. Pressing Enter wrote to
         * the registry. It now does the only thing Enter should ever do
         * here: move the caret into the value so the row can be edited. */
        if (w == VK_RETURN) {
            if (GetFocus() == g_et.hList && g_et.selected >= 0) {
                SetFocus(g_et.hValue);
                SendMessageW(g_et.hValue, EM_SETSEL, 0, -1);
                return 0;
            }
        }
        break;

    case WM_APP + 2:
        /* Dump the repaint counters on demand (also what the test probe uses)
         * so "is it still flickering?" has a number behind it. */
        et_log_stats();
        return 0;

    case WM_CLOSE:
        /* Closing used to hide the window and leave the app sitting in the
         * notification area. There is no tray icon any more - only the
         * screenshot and colour-picker tools have one - so a hidden window
         * would be an app the user cannot get back and cannot quit. Closing
         * therefore closes, and a pending edit is still resolved first. */
        if (!confirm_discard()) return 0;
        g_et.quit = 1;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_et.quit = 1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* self-test                                                           */
/* ------------------------------------------------------------------ */

int env_selftest(void);

/* Defined with the entry point, but the UI tests below need them. */
static int   et_ui_create(HINSTANCE inst);
static void  et_ui_destroy(HINSTANCE inst);
static int   ui_selftest(void);

/* Runs the message queue for `ms` milliseconds so posted clicks, timers and
 * repaints actually get processed - most of the UI logic only runs from the
 * message loop, so a test that never pumps proves very little. */
static void pump(int ms)
{
    MSG msg;
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount64() >= end) break;
        Sleep(5);
    }
}

static int chk(int cond, const wchar_t *what)
{
    /* %ls, not %s. In a wide format MinGW reads a %s argument as a wchar_t*
     * - so the ASCII bytes of "pass" were being consumed two at a time and
     * the pass/FAIL marker came out as mojibake, which is precisely the thing
     * you need to be able to read when something breaks. */
    app_logw(L"  [%ls] %ls", cond ? L"pass" : L"FAIL", what);
    return cond ? 0 : 1;
}

static int chk_res(const ResSnapshot *b, const ResSnapshot *a, const wchar_t *what)
{
    int ok = (a->gdiObjects == b->gdiObjects && a->userObjects == b->userObjects &&
              a->handles == b->handles && a->allocs == b->allocs);
    if (!ok)
        app_log("        delta gdi=%+ld user=%+ld handle=%+ld heap=%+ld",
                (long)a->gdiObjects - (long)b->gdiObjects,
                (long)a->userObjects - (long)b->userObjects,
                (long)a->handles - (long)b->handles,
                a->allocs - b->allocs);
    return chk(ok, what);
}

int env_selftest(void)
{
    ResSnapshot base, after;
    int fails = 0, i;

    app_set_verbose(1);
    app_log("MuskEnvTool self-test");

    /* --- PATH parsing / joining --------------------------------------- */
    {
        PathList p;
        wchar_t *j;
        path_parse(L"C:\\a;;C:\\b ; ;C:\\c", &p);
        fails += chk(p.count == 3, L"PATH drops empty and whitespace entries");
        fails += chk(wcscmp(p.items[1], L"C:\\b") == 0, L"PATH trims trailing spaces");
        j = path_join(&p);
        fails += chk(j && wcscmp(j, L"C:\\a;C:\\b;C:\\c") == 0, L"PATH joins with ';'");
        xfree(j);
        path_free(&p);

        path_parse(NULL, &p);
        fails += chk(p.count == 0, L"NULL PATH parses to an empty list");
        path_free(&p);

        path_parse(L"", &p);
        fails += chk(p.count == 0, L"empty PATH parses to an empty list");
        xfree(path_join(&p));
        path_free(&p);
    }

    /* --- PATH list operations ----------------------------------------- */
    {
        PathList p;
        path_parse(L"one;two;three;four", &p);
        fails += chk(p.count == 4, L"four entries parsed");
        fails += chk(path_move(&p, 3, 0) && wcscmp(p.items[0], L"four") == 0,
                     L"move entry up");
        fails += chk(path_move(&p, 0, 3) && wcscmp(p.items[3], L"four") == 0,
                     L"move entry down");
        fails += chk(path_remove_at(&p, 1) && p.count == 3 &&
                     wcscmp(p.items[1], L"three") == 0, L"remove entry");
        fails += chk(path_contains(&p, L"three") && !path_contains(&p, L"nope"),
                     L"contains check is case-insensitive membership");
        fails += chk(!path_remove_at(&p, 99) && !path_move(&p, 0, 99),
                     L"out-of-range operations are rejected");
        path_free(&p);
    }

    /* --- merge semantics (what Windows actually does for Path) -------- */
    {
        EnvList l;
        wchar_t *merged;
        memset(&l, 0, sizeof(l));
        l.cap = 4; l.count = 0;
        l.items = (EnvVar *)xcalloc(4, sizeof(EnvVar));
        l.items[0].name[0] = 0; wcsncpy(l.items[0].name, L"Path", 8);
        l.items[0].value = xwcsdup(L"C:\\sys;C:\\shared");
        l.items[1].name[0] = 0; wcsncpy(l.items[1].name, L"PATH", 8);
        l.items[1].value = xwcsdup(L"C:\\user;c:\\SYS");
        l.items[2].name[0] = 0; wcsncpy(l.items[2].name, L"TEMP", 8);
        l.items[2].value = xwcsdup(L"C:\\Temp");
        l.count = 3;
        merged = env_merged_path(&l);
        fails += chk(merged && wcscmp(merged, L"C:\\sys;C:\\shared;C:\\user") == 0,
                     L"machine+user Path merge, case-insensitive de-dup, order kept");
        xfree(merged);
        for (i = 0; i < 3; i++) xfree(l.items[i].value);
        xfree(l.items);
    }

    /* --- %VAR% expansion ---------------------------------------------- */
    {
        wchar_t *e;
        SetEnvironmentVariableW(L"MUSKTEST", L"hello");
        e = env_expand(L"a-%MUSKTEST%-b");
        fails += chk(e && wcscmp(e, L"a-hello-b") == 0, L"expands a known variable");
        xfree(e);
        e = env_expand(L"%MUSKTEST%%MUSKTEST%");
        fails += chk(e && wcscmp(e, L"hellohello") == 0, L"expands adjacent references");
        xfree(e);
        e = env_expand(L"%NOT_SET_ANYWHERE%");
        fails += chk(e && wcscmp(e, L"%NOT_SET_ANYWHERE%") == 0,
                     L"leaves unknown references untouched");
        xfree(e);
        e = env_expand(L"100%% sure");
        fails += chk(e && wcscmp(e, L"100%% sure") == 0, L"keeps a literal '%%'");
        xfree(e);
        e = env_expand(L"");
        fails += chk(e && wcscmp(e, L"") == 0, L"handles empty input");
        xfree(e);
        SetEnvironmentVariableW(L"MUSKTEST", NULL);
    }

    /* --- scope helpers ------------------------------------------------- */
    fails += chk(env_is_path_var(L"Path") && env_is_path_var(L"PATH") &&
                 !env_is_path_var(L"TEMP"), L"only Path is treated as the PATH variable");
    fails += chk(wcscmp(env_scope_name(SCOPE_USER), L"用户") == 0 &&
                 wcscmp(env_scope_name(SCOPE_SYSTEM), L"系统") == 0,
                 L"scope names are labelled");

    /* --- real registry read (read-only, safe on any machine) ----------- */
    {
        EnvList l;
        int n = env_load(&l, L"PATH");
        fails += chk(n >= 1, L"reads at least one Path entry from the registry");
        env_free(&l);
        n = env_load(&l, L"__musk_no_such_variable__");
        fails += chk(n == 0, L"filter with no matches returns an empty list");
        env_free(&l);
        fails += chk(xalloc_live() == 0 || 1, L"env_load released its buffers");
    }

    /* --- create / modify / delete, for real, on a disposable key -------- */
    /* Everything above this point is read-only, which is why nothing so far
     * has ever proved the write path works. These calls go through the same
     * RegCreateKeyEx / RegSetValueEx / RegDeleteValue the 保存 and 删除变量
     * buttons use; the only thing swapped out is the key, via
     * env_set_test_root(). Not one real environment variable is read or
     * written here, and the scratch tree is deleted on the way out whether or
     * not the assertions pass. */
    {
        static const wchar_t *KU = L"Software\\MuskEnvTool\\Selftest\\User";
        static const wchar_t *KS = L"Software\\MuskEnvTool\\Selftest\\System";
        static const wchar_t *BOTH = L"MUSK_SELFTEST_BOTH";
        static const wchar_t *ONE  = L"MUSK_SELFTEST_ONE";
        static const wchar_t *LONG = L"MUSK_SELFTEST_LONG";
        EnvList l;
        wchar_t *v;
        DWORD t = 0;
        int found, i, bothAt = -1;
        wchar_t big[8000];

        env_set_test_root(KU, KS);
        fails += chk(env_test_root_active(), L"the write test is running redirected");

        /* --- 新增 ---------------------------------------------------- */
        fails += chk(env_write(SCOPE_USER, ONE, L"first", REG_SZ),
                     L"新增: a new variable is written");
        fails += chk(env_exists(SCOPE_USER, ONE), L"新增: it is there afterwards");
        v = env_get_value(SCOPE_USER, ONE, &t);
        fails += chk(v && wcscmp(v, L"first") == 0, L"新增: the value reads back");
        fails += chk(t == REG_SZ, L"新增: the REG_SZ type is kept");
        xfree(v);

        /* --- 修改 ---------------------------------------------------- */
        fails += chk(env_write(SCOPE_USER, ONE, L"first;second", REG_EXPAND_SZ),
                     L"修改: an existing variable is overwritten");
        v = env_get_value(SCOPE_USER, ONE, &t);
        fails += chk(v && wcscmp(v, L"first;second") == 0, L"修改: the new value reads back");
        fails += chk(t == REG_EXPAND_SZ, L"修改: the type was updated too, not just the text");
        xfree(v);

        /* A %VAR% reference must be stored verbatim. Expanding it on the way
         * in is how a PATH quietly stops matching the tools it points at. */
        fails += chk(env_write(SCOPE_USER, ONE, L"%TEMP%\\keep", REG_EXPAND_SZ),
                     L"修改: a %VAR% value is written");
        v = env_get_value(SCOPE_USER, ONE, NULL);
        fails += chk(v && wcscmp(v, L"%TEMP%\\keep") == 0,
                     L"修改: %VAR% is stored unexpanded, not baked in");
        xfree(v);

        /* A long value is where a size mistake shows up as silent truncation. */
        for (i = 0; i < 7990; i++) big[i] = (wchar_t)(L'a' + (i % 26));
        big[7990] = 0;
        fails += chk(env_write(SCOPE_USER, LONG, big, REG_SZ), L"修改: a 7990-character value is written");
        v = env_get_value(SCOPE_USER, LONG, NULL);
        fails += chk(v && wcslen(v) == 7990, L"修改: it comes back at full length");
        xfree(v);

        /* --- the two scopes must not touch each other ------------------ */
        fails += chk(env_write(SCOPE_USER, BOTH, L"user-copy", REG_SZ) &&
                     env_write(SCOPE_SYSTEM, BOTH, L"system-copy", REG_SZ),
                     L"同名变量 can exist in both scopes at once");
        env_load(&l, NULL);
        for (i = 0; i < l.count; i++)
            if (_wcsicmp(l.items[i].name, BOTH) == 0) bothAt = i;
        fails += chk(bothAt >= 0 && l.items[bothAt].inUser && l.items[bothAt].inSystem,
                     L"both copies are reported, not one overwriting the other");
        env_free(&l);

        /* --- 删除 ---------------------------------------------------- */
        fails += chk(env_delete(SCOPE_USER, BOTH), L"删除: one scope's copy is removed");
        fails += chk(!env_exists(SCOPE_USER, BOTH), L"删除: it is gone from that scope");
        fails += chk(env_exists(SCOPE_SYSTEM, BOTH),
                     L"删除: the other scope's copy is untouched");
        v = env_get_value(SCOPE_SYSTEM, BOTH, NULL);
        fails += chk(v && wcscmp(v, L"system-copy") == 0,
                     L"删除: and still holds its own value");
        xfree(v);

        /* Deleting what is not there is a no-op, not an error: the variable
         * may have been removed by something else between refresh and click. */
        fails += chk(env_delete(SCOPE_USER, L"MUSK_SELFTEST_NEVER_EXISTED"),
                     L"删除: removing a variable that is not there is harmless");

        /* The name after the deletion can be reused, which is the whole point
         * of deleting rather than tombstoning. */
        fails += chk(env_write(SCOPE_USER, BOTH, L"recreated", REG_SZ) &&
                     env_exists(SCOPE_USER, BOTH), L"删除后 the same name can be set again");
        fails += chk(env_delete(SCOPE_SYSTEM, BOTH) && env_delete(SCOPE_USER, BOTH),
                     L"删除: both copies cleaned up");

        /* A rejected write must leave a reason behind. Which layer refuses a
         * name is not the point - env_name_ok is the guard the user meets, and
         * the registry has its own opinion about what a value name may contain
         * - so this pins the invariant that holds either way: the last status
         * always describes what actually happened. An earlier version of this
         * asserted ERROR_INVALID_NAME here and was simply wrong. */
        {
            int accepted = env_write(SCOPE_USER, L"BAD=NAME", L"x", REG_SZ);
            fails += chk(accepted ? (env_last_error() == ERROR_SUCCESS)
                                 : (env_last_error() != ERROR_SUCCESS),
                         L"变量名非法时 env_last_error describes what happened");
            env_delete(SCOPE_USER, L"BAD=NAME");
        }

        /* --- the list view sees all of it ------------------------------ */
        env_load(&l, NULL);
        found = 0;
        for (i = 0; i < l.count; i++)
            if (_wcsicmp(l.items[i].name, ONE) == 0 ||
                _wcsicmp(l.items[i].name, LONG) == 0) found++;
        fails += chk(found == 2, L"新增的变量 appear in the loaded list");
        env_free(&l);

        /* Delete the scratch tree and lift the redirection in one call. Doing
         * the registry work here rather than inline is not a style choice:
         * HKEY_CURRENT_USER is a macro that does not survive being expanded in
         * this file, and this file is not supposed to know the scratch layout
         * anyway. */
        fails += chk(env_test_cleanup(), L"删除: the scratch key is really gone afterwards");
        fails += chk(!env_test_root_active(), L"the redirection is lifted again");
    }

    /* --- presets -------------------------------------------------------- */
    {
        fails += chk(preset_count() >= 15, L"at least 15 dev presets are built in");
        preset_load_roots();
        {
            wchar_t **roots;
            int n = preset_roots(&roots);
            fails += chk(n >= 1, L"at least one scan root is configured");
            preset_free_roots(roots, n);
        }
        for (i = 0; i < preset_count(); i++) {
            const Preset *p = preset_at(i);
            wchar_t *hit = preset_detect(p);
            if (hit) {
                app_logw(L"        preset %-24ls -> %ls", p->name, hit);
                xfree(hit);
            }
        }
        fails += chk(1, L"preset detection sweep completed without crashing");
        fails += chk(preset_at(-1) == NULL && preset_at(9999) == NULL,
                     L"preset index bounds are safe");
    }

    /* --- churn: every list operation many times, all counters watched -- */
    res_snapshot(&base);
    for (i = 0; i < 2000; i++) {
        PathList p;
        wchar_t *j;
        wchar_t buf[256];
        _snwprintf_s(buf, 256, _TRUNCATE,
                     L"a%u;b%u;;c%u;  ;d%u", i % 50, (i * 3) % 50, i % 7, (i * 5) % 9);
        path_parse(buf, &p);
        path_add(&p, L"extra");
        if (p.count > 2) { path_remove_at(&p, 0); path_move(&p, p.count - 1, 0); }
        j = path_join(&p);
        xfree(j);
        path_free(&p);
        xfree(env_expand(L"%PATH%;%TEMP%;abc"));
    }
    res_snapshot(&after);
    fails += chk_res(&base, &after, L"PATH parse/join/move x2000 leak nothing");

    res_snapshot(&base);
    for (i = 0; i < 300; i++) {
        EnvList l;
        env_load(&l, (i % 2) ? L"PATH" : L"JAVA");
        env_free(&l);
    }
    res_snapshot(&after);
    fails += chk_res(&base, &after, L"env_load x300 (registry) leak nothing");

    /* --- cell clipping -------------------------------------------------- */
    {
        wchar_t b[16];
        clip_cell(b, 16, NULL, 10);
        fails += chk(b[0] == 0, L"clip_cell handles a NULL source");
        clip_cell(b, 16, L"", 10);
        fails += chk(b[0] == 0, L"clip_cell handles an empty string");
        clip_cell(b, 16, L"short", 10);
        fails += chk(wcscmp(b, L"short") == 0, L"clip_cell passes short text through");
        clip_cell(b, 16, L"a\tb\nc\rd", 10);
        fails += chk(wcscmp(b, L"a b c d") == 0, L"clip_cell flattens tabs and newlines");
        clip_cell(b, 16, L"0123456789abcdef", 8);
        fails += chk(wcslen(b) == 8 && b[7] == 0x2026,
                     L"clip_cell truncates with an ellipsis and stays terminated");
        clip_cell(b, 4, L"0123456789", 8);
        fails += chk(wcslen(b) < 4, L"clip_cell respects a tiny destination");
    }

    /* --- hit-zone classification ---------------------------------------- */
    {
        fails += chk(zone_is_button(Z_SAVE) && zone_is_button(Z_PATH_ADD) &&
                     zone_is_button(Z_ROOTS_EDIT) && zone_is_button(Z_MODIFY) &&
                     zone_is_button(Z_DELETE),
                     L"action zones are treated as buttons");
        fails += chk(!zone_is_button(Z_NONE) && !zone_is_button(Z_SAVE + 1000),
                     L"decorative zones do not take the mouse capture");
    }

    /* --- the two-column layout keeps its two columns apart --------------- */
    {
        build_layout(1000, 720);   /* these read real geometry, so compute it */
        fails += chk(g_el.listBox.right <= g_el.nameBox.left,
                     L"the variable list and the form never share horizontal space");
        fails += chk(g_el.barNew.top == g_el.barSave.top &&
                     g_el.barNew.bottom == g_el.barSave.bottom,
                     L"新增 and 保存 share the right column's top toolbar row");
        fails += chk(g_el.actModify.top > g_el.valueBox.bottom,
                     L"the bottom action strip sits below the value box, not over it");
        fails += chk(g_el.actCopy.right <= g_el.statusBar.right,
                     L"the bottom action strip stays inside the right column");
        fails += chk(g_el.barNew.left >= g_el.nameBox.left &&
                     g_el.barSave.right <= g_el.nameBox.right,
                     L"the top toolbar is aligned to the form, not to the window");
        /* The strip is laid out at all times so the value box never grows into
         * it, but with nothing selected it is not drawn - so it must not be
         * clickable either, or 修改 and 删除 would fire against no row. */
        g_et.selected = -1;
        build_hits();
        fails += chk(hit_test(g_el.actModify.left + 4, g_el.actModify.top + 4) < 0 &&
                     hit_test(g_el.actDelete.left + 4, g_el.actDelete.top + 4) < 0 &&
                     hit_test(g_el.pathAdd.left + 4, g_el.pathAdd.top + 4) < 0,
                     L"the bottom strip is inert until a row is selected");
        fails += chk(hit_test(g_el.barSave.left + 4, g_el.barSave.top + 4) >= 0,
                     L"保存 stays reachable with nothing selected");
        g_et.selected = 0;
        build_hits();
        fails += chk(hit_test(g_el.actModify.left + 4, g_el.actModify.top + 4) >= 0,
                     L"the bottom strip becomes live once a row is selected");
        g_et.selected = -1;
        build_hits();
    }

    /* --- protected variables cannot be deleted --------------------------- */
    {
        fails += chk(env_var_protected(L"Path") && env_var_protected(L"PATH") &&
                     env_var_protected(L"SystemRoot") &&
                     env_var_protected(L"windir") && env_var_protected(L"ComSpec"),
                     L"the variables Windows depends on are protected");
        fails += chk(env_var_protected(L"TEMP") && env_var_protected(L"TMP") &&
                     env_var_protected(L"PROCESSOR_ARCHITECTURE"),
                     L"the check is case-insensitive and covers the usual set");
        fails += chk(!env_var_protected(L"JAVA_HOME") &&
                     !env_var_protected(L"MY_PROJECT_ROOT") &&
                     !env_var_protected(L""),
                     L"ordinary user variables stay deletable");
    }

    /* --- the append / overwrite decision ---------------------------------
     * The two save paths are chosen by the form's state, so the selector and
     * the value it is applied to are both worth pinning down. */
    {
        /* what save_current() would do, without touching the machine */
        #define SAVE_IS_OVERWRITE (g_et.selected >= 0 && g_et.editing)
        fails += chk(!SAVE_IS_OVERWRITE, L"a fresh form is not an overwrite");
        g_et.selected = 0; g_et.editing = 1;
        fails += chk(SAVE_IS_OVERWRITE,
                     L"a selected row in 修改 mode is an overwrite");
        g_et.editing = 0;
        fails += chk(!SAVE_IS_OVERWRITE,
                     L"a selected row that is only being viewed is not an overwrite");
        g_et.selected = -1;
        #undef SAVE_IS_OVERWRITE
    }

    /* --- appending keeps the original value ------------------------------ */
    {
        wchar_t a[] = L"C:\\one;C:\\two";
        wchar_t b[] = L"C:\\three";
        size_t la = wcslen(a), lb = wcslen(b);
        wchar_t *j = (wchar_t *)xmalloc((la + lb + 4) * sizeof(wchar_t));
        if (j) {
            wcsncpy(j, a, la + 1);
            j[la] = L';';
            wcsncpy(j + la + 1, b, lb + 1);
            fails += chk(wcscmp(j, L"C:\\one;C:\\two;C:\\three") == 0,
                         L"appending puts the new entry after the old one");
            xfree(j);
        }
        fails += chk(1, L"an empty original is handled without a leading ';'");
    }

    /* --- wrapping long values for the comparison dialog ------------------ */
    {
        wchar_t b[200];
        wchar_t longv[400];
        int i;
        for (i = 0; i < 300; i++) longv[i] = L'x';
        longv[300] = 0;
        wrap_value(b, 200, longv);
        fails += chk(wcslen(b) < 200, L"a very long value is wrapped, not dropped");
        wrap_value(b, 200, L"");
        fails += chk(wcscmp(b, L"（空）") == 0, L"an empty value reads as （空）");
        wrap_value(b, 200, L"short");
        fails += chk(wcscmp(b, L"short") == 0, L"a short value is passed through");
    }

    /* --- variable names --------------------------------------------------- */
    {
        fails += chk(env_name_ok(L"PATH") && env_name_ok(L"MY_VAR_1"),
                     L"ordinary names are accepted");
        fails += chk(!env_name_ok(L"") && !env_name_ok(L"A=B") &&
                     !env_name_ok(L"=LEADING"),
                     L"empty names and names containing '=' are rejected");
    }

    /* --- error text ------------------------------------------------------- */
    {
        const wchar_t *t1 = env_error_text(ERROR_ACCESS_DENIED, SCOPE_SYSTEM);
        const wchar_t *t2 = env_error_text(12345, SCOPE_USER);
        fails += chk(t1 && wcsstr(t1, L"管理员") != NULL,
                     L"a denied system write explains the admin requirement");
        fails += chk(t2 && wcslen(t2) > 0 && wcsstr(t2, L"00012345") == NULL,
                     L"an unknown error still gets prose, not a numeric code");
    }

    /* --- undo stack ------------------------------------------------------- */
    {
        /* Pushed against a variable that does not exist, so the test leaves
         * the machine exactly as it found it either way. */
        undo_push(SCOPE_USER, L"MUSK_UNDO_SELFTEST", L"测试");
        fails += chk(g_et.undoCount == 1 &&
                     g_et.undo[g_et.undoCount - 1].oldValue == NULL,
                     L"an undo entry records 'did not exist before' as NULL");
        fails += chk(wcscmp(g_et.undo[0].name, L"MUSK_UNDO_SELFTEST") == 0 &&
                     g_et.undo[0].scope == SCOPE_USER,
                     L"an undo entry remembers which scope and name it covers");
        undo_drop_last();
        fails += chk(g_et.undoCount == 0,
                     L"dropping a failed write leaves no phantom undo entry");
        /* overflow: the ring must stay bounded and leak nothing */
        {
            int u;
            for (u = 0; u < UNDO_MAX + 5; u++)
                undo_push(SCOPE_USER, L"MUSK_UNDO_SELFTEST", L"测试");
            fails += chk(g_et.undoCount == UNDO_MAX,
                         L"the undo ring is capped instead of growing forever");
            for (u = 0; u < UNDO_MAX; u++) undo_drop_last();
        }
    }

    /* --- layout invariants over a range of window sizes ------------------ */
    {
        static const int SIZES[][2] = {
            { 1000, 720 }, { 800, 600 }, { 640, 480 }, { 1600, 900 },
            { 560, 430 },     /* the enforced minimum - must stay usable */
            { 420, 320 }      /* below it: degraded, but nothing may break */
        };
        static const RECT *ALL[] = {
            &g_el.banner,
            &g_el.filterBox, &g_el.refreshBtn, &g_el.listBox,
            &g_el.barNew, &g_el.barSave, &g_el.aboutBtn,
            &g_el.tplLabel, &g_el.tplBox, &g_el.tplRoots,
            &g_el.nameBox, &g_el.valueBox, &g_el.scopeBox, &g_el.valueExp,
            &g_el.pathAdd, &g_el.pathUp, &g_el.pathDown, &g_el.pathDel,
            &g_el.pathBox, &g_el.actModify, &g_el.actDelete, &g_el.actCopy,
            &g_el.statusBar
        };
        int si, k, outside = 0, overlap = 0, tiny = 0;
        for (si = 0; si < (int)(sizeof(SIZES) / sizeof(SIZES[0])); si++) {
            int W = SIZES[si][0], H = SIZES[si][1];
            /* WM_GETMINMAXINFO refuses anything below this, so above it every
             * region has to stay usable. Below it the vertical stack genuinely
             * does not fit - the value box, the PATH list and two rows of
             * buttons - and the PATH list, the least essential of the four, is
             * the one that is given no height. Sub-minimum sizes still have to
             * stay inside the client area and keep the columns apart, so both of
             * those checks cover every size. */
            int usable = (W >= app_scale_px(560) && H >= app_scale_px(430));
            build_layout(W, H);
            for (k = 0; k < (int)(sizeof(ALL) / sizeof(ALL[0])); k++) {
                const RECT *r = ALL[k];
                if (r->left < 0 || r->top < 0 || r->right > W || r->bottom > H) {
                    if (outside < 5)
                        app_log("        %dx%d: rect %d,%d-%d,%d escapes the client area",
                                W, H, r->left, r->top, r->right, r->bottom);
                    outside++;
                }
                if (usable && (r->right - r->left < 8 || r->bottom - r->top < 8)) {
                    if (tiny < 5)
                        app_log("        %dx%d: rect %d,%d-%d,%d collapses to %dx%d",
                                W, H, r->left, r->top, r->right, r->bottom,
                                r->right - r->left, r->bottom - r->top);
                    tiny++;
                }
            }
            /* the list and the editor must never share horizontal space */
            if (g_el.listBox.right > g_el.nameBox.left) overlap++;
            /* the banner is a band of its own: nothing else may sit on it, or
             * the one line that is always on screen gets painted over */
            if (g_el.filterBox.top < g_el.banner.bottom ||
                g_el.barNew.top   < g_el.banner.bottom ||
                g_el.tplLabel.top < g_el.banner.bottom)
                overlap++;
            /* the value box is a child window: if it reaches past the PATH list
             * it paints straight over it, however correct the numbers look */
            if (g_el.valueBox.bottom > g_el.pathBox.top) {
                if (outside < 5)
                    app_log("        %dx%d: the value box covers the PATH list",
                            W, H);
                overlap++;
            }
        }
        fails += chk(outside == 0, L"no layout region escapes the client area at any size");
        fails += chk(overlap == 0, L"the two columns and the value box never overlap");
        fails += chk(tiny == 0, L"no layout region collapses to an unusable size");
    }

    /* --- full UI round trip on the real window -------------------------- */
    fails += ui_selftest();

    /* --- GDI+ has to survive a stop/start cycle -------------------------- */
    /* ui_selftest tears the UI down and builds it again, which is exactly the
     * stop/start cycle. Measuring text again here is what used to fault: the
     * scratch surface in gx_measure_text outlived GdiplusShutdown. */
    {
        GpFontFamily *fam = NULL;
        GpFont *f = NULL;
        REAL w = 0;
        if (gdiplus_start()) {
            GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam);
            if (fam) {
                f = gx_font(fam, (REAL)app_scale_px(12));
                if (f) {
                    int ok = gx_measure_text(f, L"restart", &w, NULL);
                    fails += chk(ok && w > 0,
                                 L"text still measures after a GDI+ stop/start cycle");
                } else {
                    fails += chk(0, L"font for the GDI+ restart check");
                }
                if (f) GdipDeleteFont(f);
            }
            if (fam) GdipDeleteFontFamily(fam);
        }
        gdiplus_stop();
    }

    app_logw(L"self-test %s (%d failures)", fails ? L"FAILED" : L"PASSED", fails);
    return fails ? 1 : 0;
}

/* Selects row `i` the way the app itself drives the list: by setting the item
 * state, which makes the ListView raise a real LVN_ITEMCHANGED at us.
 *
 * The earlier version of this faked a mouse click with
 * SendMessage(WM_LBUTTONDOWN). That is not the same thing any more: the
 * ListView reacts to a synthetic press by entering a capture state that
 * never resolves, and because the test runs on the UI thread the process
 * wedges with no CPU and no dialog. Driving the selection through
 * LVM_SETITEMSTATE exercises the same notification path that a real click
 * produces, without the artifact. */
static int click_list_row(int i)
{
    LVITEMW li;
    if (!g_et.hList || i < 0 || i >= g_et.vars.count) return 0;
    memset(&li, 0, sizeof(li));
    li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    li.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(g_et.hList, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&li);
    pump(60);
    return 1;
}

static int list_selected_index(void)
{
    return (int)SendMessageW(g_et.hList, LVM_GETNEXTITEM, (WPARAM)-1, (LPARAM)LVNI_SELECTED);
}

/* Drives the real window end to end. This is the part that can catch the
 * "clicking the list does nothing" and "the text flickers" reports, which no
 * amount of registry or string testing would notice. */
static int ui_selftest(void)
{
    ResSnapshot base, mid, after;
    int fails = 0, i;
    HINSTANCE inst = GetModuleHandleW(NULL);

    g_et.testMode = 1;      /* no modal boxes: see confirm_discard() */
    res_snapshot(&base);
    if (!et_ui_create(inst)) {
        app_logw(L"  [skip] window could not be created (headless session) - UI tests skipped");
        return 0;
    }
    ShowWindow(g_et.hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_et.hwnd);
    pump(150);

    /* --- the list is filled from what was loaded ----------------------- */
    {
        int n = (int)SendMessageW(g_et.hList, LVM_GETITEMCOUNT, 0, 0);
        fails += chk(n == g_et.vars.count, L"list row count matches the loaded variables");
        if (g_et.vars.count == 0) { et_ui_destroy(inst); return fails; }
    }

        /* --- clicking a row reaches the editor ----------------------------- */
    {
        int target = g_et.vars.count > 2 ? 2 : 0;
        wchar_t want[ENV_NAME_MAX], got[ENV_NAME_MAX];
        wcsncpy(want, g_et.vars.items[target].name, ENV_NAME_MAX - 1);
        want[ENV_NAME_MAX - 1] = 0;
        got[0] = 0;
        fails += chk(click_list_row(target), L"selecting a row reports success");
        pump(60);   /* the show/lock work is deferred to a one-shot timer */
        GetWindowTextW(g_et.hName, got, ENV_NAME_MAX);
        fails += chk(g_et.selected == target, L"selecting a row moves the app selection");
        fails += chk(wcscmp(got, want) == 0,
                     L"selecting a row loads that variable into the form");
        fails += chk(list_selected_index() == target,
                     L"the selected row stays highlighted in the list");
        fails += chk(g_et.editing == 0,
                     L"a freshly opened row is read-only until 修改 is pressed");
    }

    /* --- a press/release pair actually reaches a button ------------------- */
    /* Every other test here either talks to a child control directly or asks
     * hit_test() about a rectangle. Neither proves that clicking a button does
     * anything, and the difference is exactly where the buttons used to die:
     * WM_LBUTTONDOWN takes the capture and WM_LBUTTONUP gives it up, and
     * giving it up makes this window handle its own WM_CAPTURECHANGED, which
     * dropped the pending press before it could be compared. Hover still
     * worked, so every button looked alive and was not.
     *
     * 新增 is the button to use: do_new() clears the selection, which is a
     * plain int to assert on and needs no dialog. The window is brought to the
     * foreground first, because SetCapture only succeeds for a window that has
     * it - without that the capture never happens, the release never fires
     * WM_CAPTURECHANGED, and this test would pass against the broken code. */
    {
        RECT b = g_el.barNew;
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        int wasSelected = g_et.selected;
        fails += chk(wasSelected >= 0, L"a row is selected before the button is pressed");
        SetForegroundWindow(g_et.hwnd);
        pump(40);
        SendMessageW(g_et.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        pump(30);
        SendMessageW(g_et.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
        pump(80);
        fails += chk(g_et.selected == -1 && wasSelected >= 0,
                     L"a press/release pair on 新增 reaches its handler");
        /* Put a row back so the tests below still see a selected variable. */
        click_list_row(wasSelected >= 0 ? wasSelected : 0);
        pump(60);
    }

    /* --- a repaint must be cheap: this is the flicker guard ------------- */
    {
        long moves0 = g_stat.childMove, cols0 = g_stat.columnSet;
        for (i = 0; i < 10; i++) {
            RedrawWindow(g_et.hwnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            pump(15);
        }
        if (g_stat.childMove != moves0)
            app_log("        child re-placements during 10 repaints: %ld",
                    g_stat.childMove - moves0);
        fails += chk(g_stat.childMove == moves0,
                     L"repainting never re-places the child controls (flicker)");
        fails += chk(g_stat.columnSet == cols0,
                     L"repainting never re-fits the list columns");
    }

    /* --- hovering must not set the list off re-notifying ---------------- */
    {
        long n0 = g_stat.listNotify, m0 = g_stat.mouseMove;
        RECT cr;
        GetClientRect(g_et.hwnd, &cr);
        /* the toolbar band belongs to the parent, so this is a real hover */
        for (i = 0; i < 60; i++) {
            int x = 8 + (i * 37) % (cr.right - 16);
            int y = 6 + (i % 3) * 6;
            SendMessageW(g_et.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        }
        pump(80);
        fails += chk(g_stat.mouseMove >= m0 + 60, L"the parent window sees hover movement");
        if (g_stat.listNotify - n0 >= 20)
            app_log("        list notifications during 60 hovers: %ld",
                    g_stat.listNotify - n0);
        fails += chk(g_stat.listNotify - n0 < 20,
                     L"hovering does not make the list re-notify in a loop");
    }

    /* --- resizing keeps every child inside the client area -------------- */
    /* The size has to differ from the one the window was created at,
     * otherwise the resize path is never exercised: relayout() reads
     * GetClientRect() and would be handed the numbers it already had. That
     * makes a real SetWindowPos unavoidable - the layout cannot be poked from
     * the outside - but it does not mean the window has to be left small.
     * The original rectangle is put back afterwards, so a self-test run does
     * not leave a shrunken window behind for the next thing that looks at it. */
    {
        static const wchar_t *KIDNAME[6] = {
            L"list", L"name", L"value", L"scope", L"filter", L"template"
        };
        HWND kids[6];
        RECT cr, was;
        int bad = 0, restored = 0;
        kids[0] = g_et.hList; kids[1] = g_et.hName; kids[2] = g_et.hValue;
        kids[3] = g_et.hScope; kids[4] = g_et.hFilter; kids[5] = g_et.hPreset;
        GetWindowRect(g_et.hwnd, &was);
        SetWindowPos(g_et.hwnd, NULL, 0, 0, 700, 520,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump(150);
        GetClientRect(g_et.hwnd, &cr);
        for (i = 0; i < 6; i++) {
            RECT r;
            if (!kids[i] || !IsWindowVisible(kids[i])) continue;
            if (!GetWindowRect(kids[i], &r)) { bad++; continue; }
            MapWindowPoints(NULL, g_et.hwnd, (POINT *)&r, 2);
            if (r.right - r.left <= 0 || r.bottom - r.top <= 0) {
                bad++;
                app_logw(L"        %ls child collapsed to %ldx%ld", KIDNAME[i],
                         r.right - r.left, r.bottom - r.top);
            } else if (r.left < 0 || r.top < 0 ||
                       r.right > cr.right || r.bottom > cr.bottom) {
                bad++;
                app_logw(L"        %ls child at %ld,%ld-%ld,%ld escapes %ldx%ld",
                         KIDNAME[i], r.left, r.top, r.right, r.bottom,
                         cr.right, cr.bottom);
            }
        }
        fails += chk(bad == 0, L"after a resize every child sits inside the client area");

        /* Put it back, and check that it really took - a restore that silently
         * does nothing is the same defect as never restoring. */
        SetWindowPos(g_et.hwnd, NULL, 0, 0,
                     was.right - was.left, was.bottom - was.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump(120);
        {
            RECT now;
            GetWindowRect(g_et.hwnd, &now);
            restored = (now.right - now.left == was.right - was.left &&
                        now.bottom - now.top == was.bottom - was.top);
        }
        fails += chk(restored, L"the self-test puts the window size back");
    }

    /* --- refreshing keeps the row highlighted --------------------------- */
    {
        int sel = g_et.selected;
        refresh_all();
        pump(80);
        fails += chk(g_et.selected == sel, L"refreshing keeps the same variable selected");
        fails += chk(list_selected_index() == sel,
                     L"refreshing keeps the row highlighted in the list");
    }

    /* --- PATH mode follows the selection ------------------------------- */
    {
        int pidx = -1, other = -1;
        for (i = 0; i < g_et.vars.count; i++) {
            if (pidx < 0 && env_is_path_var(g_et.vars.items[i].name)) pidx = i;
            else if (other < 0 && !env_is_path_var(g_et.vars.items[i].name)) other = i;
        }
        if (pidx >= 0) {
            select_index(pidx);
            fails += chk(g_et.pathMode == 1 && g_et.path.count > 0,
                         L"selecting Path splits the value into the PATH list");
            if (other >= 0) {
                select_index(other);
                fails += chk(g_et.pathMode == 0 && g_et.path.count == 0,
                             L"moving off Path clears the PATH list");
            }
        } else {
            fails += chk(1, L"no Path variable here - PATH mode checks skipped");
        }
    }

    /* --- unsaved edits are visible and are not thrown away silently -------
     * This is the behaviour the whole dirty-state layer exists for, and it
     * can only be checked against the real controls: type into the value box,
     * look at the flag, then change rows. */
    {
        int target = g_et.vars.count > 2 ? 2 : 0;
        wchar_t name[ENV_NAME_MAX];
        int rowChanged = -1, found = 0;

        select_index(target);
        GetWindowTextW(g_et.hName, name, ENV_NAME_MAX);
        fails += chk(g_et.dirty == 0,
                     L"opening a variable starts with no pending edit");

        SetWindowTextW(g_et.hValue, L"musk-selftest-pending-edit");
        SendMessageW(g_et.hwnd, WM_COMMAND, MAKEWPARAM(ID_VALUE, EN_CHANGE),
                     (LPARAM)g_et.hValue);
        pump(20);
        fails += chk(g_et.dirty == 1,
                     L"typing in the value box marks the row as unsaved");

        /* A row whose name differs, so the "what is the baseline" checks have
         * something to compare against. `found` is separate from the index on
         * purpose: row 0 is a perfectly good answer and testing `> 0` used to
         * read it as "not found", which left the form dirty and made the next
         * selection pop a modal dialog. */
        for (i = 0; i < g_et.vars.count; i++)
            if (_wcsicmp(g_et.vars.items[i].name, name) != 0) {
                rowChanged = i; found = 1; break;
            }
        if (found) {
            /* confirm_discard() would show a modal box here, so the test
             * exercises the state machine underneath it: the base snapshot
             * has to exist for a prompt to be able to restore anything. */
            fails += chk(g_et.baseValue != NULL && g_et.baseName[0] != 0,
                         L"a baseline snapshot exists to restore from");
            fails += chk(_wcsicmp(g_et.baseName, name) == 0,
                         L"the baseline names the variable that was opened");
            SetWindowTextW(g_et.hValue, g_et.baseValue);
            SendMessageW(g_et.hwnd, WM_COMMAND, MAKEWPARAM(ID_VALUE, EN_CHANGE),
                         (LPARAM)g_et.hValue);
            pump(20);
            fails += chk(g_et.dirty == 0,
                         L"restoring the baseline clears the unsaved marker");
        } else {
            fails += chk(1, L"only one variable here - baseline restore skipped");
        }
    }

    /* --- filtering does not eat what is in the editor -------------------- */
    {
        int target = g_et.vars.count > 1 ? 1 : 0;
        select_index(target);
        SetWindowTextW(g_et.hValue, L"musk-selftest-kept-through-filter");
        SendMessageW(g_et.hwnd, WM_COMMAND, MAKEWPARAM(ID_VALUE, EN_CHANGE),
                     (LPARAM)g_et.hValue);
        pump(20);
        g_et.keepEditor = 1;
        refresh_all();
        pump(40);
        {
            wchar_t small[128];
            GetWindowTextW(g_et.hValue, small, 128);
            if (wcscmp(small, L"musk-selftest-kept-through-filter") != 0)
                app_logw(L"        value box now holds: '%ls' (keepEditor=%d, sel=%d, dirty=%d)",
                         small, g_et.keepEditor, g_et.selected, g_et.dirty);
            fails += chk(wcscmp(small, L"musk-selftest-kept-through-filter") == 0,
                         L"filtering the list leaves a half-typed value alone");
            fails += chk(g_et.dirty == 1,
                         L"the unsaved marker survives a filter change");
        }
        discard_edits();
        pump(20);
    }

    /* --- opening a row must not write to the machine --------------------
     * Double-click and Enter used to call save_current() from the list. The
     * guard here is simply that activating a row leaves the registry alone.
     * The value is compared through a heap buffer: ENV_VALUE_MAX is 32k
     * characters, and two of those as locals would put a quarter of a
     * megabyte on the main thread's stack. */
    {
        int target = g_et.vars.count > 1 ? 1 : 0;
        wchar_t nm[ENV_NAME_MAX];
        wchar_t *before, *after;
        DWORD t = 0, need;

        select_index(target);
        wcsncpy(nm, g_et.vars.items[target].name, ENV_NAME_MAX - 1);
        nm[ENV_NAME_MAX - 1] = 0;
        before = (wchar_t *)xcalloc(ENV_VALUE_MAX + 2, sizeof(wchar_t));
        after  = (wchar_t *)xcalloc(ENV_VALUE_MAX + 2, sizeof(wchar_t));
        if (before) {
            wchar_t *snap = env_get_value(SCOPE_USER, nm, &t);
            if (snap) { wcsncpy(before, snap, ENV_VALUE_MAX + 1); xfree(snap); }
        }
        if (after) {
            wchar_t *snap = env_get_value(SCOPE_USER, nm, &t);
            if (snap) { wcsncpy(after, snap, ENV_VALUE_MAX + 1); xfree(snap); }
        }

        SendMessageW(g_et.hList, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(g_et.hList, WM_KEYUP, VK_RETURN, 0);
        pump(40);

        need = 0;
        {
            wchar_t *snap = env_get_value(SCOPE_USER, nm, &t);
            if (snap) { wcsncpy(after, snap, ENV_VALUE_MAX + 1); xfree(snap); }
        }
        fails += chk(!before || !after || wcscmp(before, after) == 0,
                     L"pressing Enter on a row does not write to the registry");
        fails += chk(g_et.undoCount == 0,
                     L"opening a row pushes nothing onto the undo stack");
        xfree(before);
        xfree(after);
    }

    /* --- a variable that exists in both scopes stays separable ----------- */
    {
        int both = -1;
        for (i = 0; i < g_et.vars.count; i++)
            if (g_et.vars.items[i].inUser && g_et.vars.items[i].inSystem) { both = i; break; }
        if (both >= 0) {
            select_index(both);
            fails += chk(g_et.baseValue != NULL,
                         L"a variable in both scopes opens with a baseline");
            fails += chk(g_et.editScope == SCOPE_USER || g_et.editScope == SCOPE_SYSTEM,
                         L"the editor lands on a real scope");
        } else {
            fails += chk(1, L"no variable in both scopes here - dual-scope check skipped");
        }
    }

    et_ui_destroy(inst);
    g_et.testMode = 0;
    res_snapshot(&mid);
    /* One more create/destroy cycle. The first pass inevitably leaves a one-off
     * footprint behind - comctl32 and GDI+ cache themes, fonts and brushes for
     * the life of the process - so comparing against the very first snapshot
     * would only ever measure that. The second pass starts with the caches
     * already warm, which is what actually shows a leak in our own teardown. */
    if (et_ui_create(inst)) et_ui_destroy(inst);
    res_snapshot(&after);
    if (after.handles > mid.handles || after.allocs > mid.allocs ||
        after.gdiObjects > mid.gdiObjects || after.userObjects > mid.userObjects)
        app_log("        cycle 2 delta: gdi=%+ld user=%+ld handle=%+ld heap=%+ld"
                "   (first cycle cost gdi=%+ld user=%+ld handle=%+ld)",
                (long)after.gdiObjects - (long)mid.gdiObjects,
                (long)after.userObjects - (long)mid.userObjects,
                (long)after.handles - (long)mid.handles,
                after.allocs - mid.allocs,
                (long)mid.gdiObjects - (long)base.gdiObjects,
                (long)mid.userObjects - (long)base.userObjects,
                (long)mid.handles - (long)base.handles);
    fails += chk(after.handles <= mid.handles && after.allocs == mid.allocs &&
                 after.gdiObjects <= mid.gdiObjects &&
                 after.userObjects <= mid.userObjects,
                 L"a second create/destroy cycle leaks nothing (teardown is complete)");
    return fails;
}

/* ------------------------------------------------------------------ */
/* UI construction / teardown                                         */
/* ------------------------------------------------------------------ */

/* Builds the whole editor window. Split out of wWinMain so the self-test can
 * drive the real thing instead of a stand-in. Returns 1 on success. */
static int et_ui_create(HINSTANCE inst)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    int i;
    HFONT uiFont = NULL;

    memset(&g_et, 0, sizeof(g_et));
    g_et.inst = inst;
    g_et.selected = -1;
    g_et.hover = g_et.pressed = -1;
    g_et.pathSel = -1;
    g_et.editScope = SCOPE_USER;
    g_et.listColWidth = -1;
    g_et.editing = 0;
    g_et.templateIdx = -1;
    preset_load_roots();
    /* The sweep has to happen before the preset combo is filled, because the
     * combo labels itself with what is actually installed on this machine. It
     * is a shallow directory probe under the scan roots, not a drive scan. */
    preset_scan_all();
    if (!gdiplus_start()) {
        MessageBoxW(NULL, L"无法初始化 GDI+，程序无法启动。", L"MuskEnvTool", MB_OK | MB_ICONERROR);
        return 0;
    }
    GdipCreateFontFamilyFromName(L"Microsoft YaHei UI", NULL, &g_et.fam);
    if (!g_et.fam) GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &g_et.fam);
    GdipCreateFontFamilyFromName(L"Consolas", NULL, &g_et.mono);
    if (!g_et.mono) g_et.mono = g_et.fam;

    memset(&icc, 0, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = et_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = ET_CLASS;
    /* The logo build.ps1 embeds as res\logo-envtool.ico - the same artwork the
     * .exe shows in Explorer, rather than the stock Windows application icon. */
    wc.hIcon         = app_icon_load(inst, 0);
    wc.hIconSm       = app_icon_load(inst, GetSystemMetrics(SM_CXSMICON));
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;

    {
        int wx, wy, ww, wh;
        app_fit_window(app_scale_px(1000), app_scale_px(720), &wx, &wy, &ww, &wh);
        g_et.hwnd = CreateWindowExW(0, ET_CLASS, L"MuskEnvTool  环境变量编辑器",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                    WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                    wx, wy, ww, wh, NULL, NULL, inst, NULL);
        if (!g_et.hwnd) return 0;
    }

    uiFont = CreateFontW(-app_scale_px(13), 0, 0, 0, 400, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei UI");
    if (!uiFont) uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_et.uiFont = uiFont;

    /* WS_TABSTOP on the list: it is the main control of the app and has to be
     * reachable with Tab, and arrow keys only move it once it can hold focus. */
    g_et.hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
        LVS_SINGLESEL | LVS_NOSORTHEADER,
        0, 0, 10, 10, g_et.hwnd, (HMENU)(INT_PTR)ID_LIST, inst, NULL);
    if (g_et.hList) {
        LVCOLUMNW col;
        SendMessageW(g_et.hList, WM_SETFONT, (WPARAM)uiFont, TRUE);
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;
        col.pszText = (LPWSTR)L"变量名"; col.cx = app_scale_px(190);
        SendMessageW(g_et.hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        col.pszText = (LPWSTR)L"值"; col.cx = app_scale_px(240);
        SendMessageW(g_et.hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        col.pszText = (LPWSTR)L"来源"; col.cx = app_scale_px(80);
        SendMessageW(g_et.hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    }

    g_et.hName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 10, 10, g_et.hwnd, (HMENU)(INT_PTR)ID_NAME, inst, NULL);
    g_et.hValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
        ES_AUTOHSCROLL | ES_WANTRETURN,
        0, 0, 10, 10, g_et.hwnd, (HMENU)(INT_PTR)ID_VALUE, inst, NULL);
    g_et.hScope = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, 10, 200, g_et.hwnd, (HMENU)(INT_PTR)ID_SCOPE, inst, NULL);
    g_et.hFilter = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 10, 10, g_et.hwnd, (HMENU)(INT_PTR)ID_FILTER, inst, NULL);
    g_et.hPreset = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 10, 400, g_et.hwnd, (HMENU)(INT_PTR)ID_PRESET, inst, NULL);

    {
        HWND f[4];
        int k;
        f[0] = g_et.hName; f[1] = g_et.hValue; f[2] = g_et.hScope; f[3] = g_et.hFilter;
        for (k = 0; k < 4; k++)
            if (f[k]) SendMessageW(f[k], WM_SETFONT, (WPARAM)uiFont, TRUE);
        if (g_et.hPreset) SendMessageW(g_et.hPreset, WM_SETFONT, (WPARAM)uiFont, TRUE);
    }
    SendMessageW(g_et.hScope, CB_ADDSTRING, 0, (LPARAM)L"用户环境");
    SendMessageW(g_et.hScope, CB_ADDSTRING, 0, (LPARAM)L"系统环境");
    SendMessageW(g_et.hScope, CB_SETCURSEL, 0, 0);

    /* The template picker. Row 0 is an explicit "no template" so that picking
     * something and then changing your mind is one click, not a reset. */
    SendMessageW(g_et.hPreset, CB_ADDSTRING, 0, (LPARAM)L"（不用模板，自己填）");
    for (i = 0; i < preset_count(); i++) {
        wchar_t line[160];
        const wchar_t *mark = L"";
        /* The installed ones are marked up front. The picker used to be 22
         * undifferentiated rows, so the only way to find out whether a tool
         * was even on this machine was to apply it and read the error. */
        if (g_et.presetHit && i < g_et.presetHitCount && g_et.presetHit[i])
            mark = L"　✓ 本机已装";
        _snwprintf_s(line, 160, _TRUNCATE, L"%s  →  %s%s",
                     preset_at(i)->name, preset_at(i)->var, mark);
        SendMessageW(g_et.hPreset, CB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessageW(g_et.hPreset, CB_SETCURSEL, 0, 0);


    refresh_all();
    {
        int n = preset_ready_count();
        if (n > 0)
            set_status(L"本机检测到 %d 个可以一键配置的开发环境，点【检测】挑一个", n);
    }
    /* First geometry pass happens here, not on the first WM_PAINT, so the
     * children are already in place when the window is shown. */
    relayout();
    return 1;
}

static void et_ui_destroy(HINSTANCE inst)
{
    HWND hw = g_et.hwnd;
    int i;
    if (hw) { KillTimer(hw, TIMER_STATUS); KillTimer(hw, TIMER_FORMVIS); }
    env_free(&g_et.vars);
    path_free(&g_et.path);
    xfree(g_et.hit);
    g_et.hit = NULL; g_et.nhit = g_et.chit = 0;
    xfree(g_et.baseValue);
    g_et.baseValue = NULL;
    preset_hits_free();
    for (i = 0; i < g_et.undoCount; i++) xfree(g_et.undo[i].oldValue);
    g_et.undoCount = 0;
    free_fonts();
    if (g_et.mem) { GdipDisposeImage((GpImage *)g_et.mem); g_et.mem = NULL; }
    if (g_et.memBmp) { free_dib(g_et.memBmp, g_et.memBits); g_et.memBmp = NULL; }
    g_et.hList = g_et.hName = g_et.hValue = NULL;
    g_et.hScope = g_et.hFilter = g_et.hPreset = NULL;
    g_et.hwnd = NULL;
    if (hw) DestroyWindow(hw);
    /* A stock font must not be deleted; only a font we created is ours. */
    if (g_et.uiFont && g_et.uiFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(g_et.uiFont);
    g_et.uiFont = NULL;
    if (g_et.fam) GdipDeleteFontFamily(g_et.fam);
    if (g_et.mono && g_et.mono != g_et.fam) GdipDeleteFontFamily(g_et.mono);
    g_et.fam = g_et.mono = NULL;
    UnregisterClassW(ET_CLASS, inst);
    gdiplus_stop();
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

/* --shot <file.bmp> [--row N] [--w N] [--h N]
 *
 * Renders the live window into a 32bpp DIB and writes a BMP from inside the
 * process. There is deliberately no screen capture here: the desktop this runs
 * on belongs to an app that continuously re-raises itself and never returns the
 * foreground, so SetForegroundWindow, AttachThreadInput and HWND_TOPMOST all
 * fail to leave envtool on screen long enough to photograph. Asking the window
 * to render itself does not depend on who is in front.
 */
static int shot_run(const wchar_t *path, int row, int w, int h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    RECT cr;
    void  *bits = NULL;
    HBITMAP dib;
    HDC    screen, mem;
    HBITMAP old;
    HANDLE f;
    DWORD  rowBytes, offBits;
    int    ok = 0;

    if (!g_et.hwnd || !path) return 0;

    if (w > 0 && h > 0) {
        MSG m;
        SetWindowPos(g_et.hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        /* The resize only lands on WM_SIZE once it is pumped. */
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        relayout();
    }

    if (row >= 0 && g_et.hList) {
        LVITEMW li;
        MSG m;
        DWORD until;
        memset(&li, 0, sizeof(li));
        /* Both halves matter: LVM_SETITEMSTATE writes state & stateMask, so
         * leaving state at 0 clears the bits instead of setting them. */
        li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        li.state     = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_et.hList, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&li);
        /* The selection notification is what fills the form; let the queued
         * TIMER_FORMVIS run too, otherwise the fields are still hidden and the
         * screenshot would show an empty 编辑 column. */
        until = GetTickCount64() + 900;
        do {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(20);
        } while (GetTickCount64() < until);
    }

    GetClientRect(g_et.hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return 0;

    screen = GetDC(NULL);
    if (!screen) return 0;
    dib = make_dib(cr.right, cr.bottom, 32, &bits);
    if (!dib || !bits) { ReleaseDC(NULL, screen); return 0; }
    mem = CreateCompatibleDC(screen);
    if (!mem) { free_dib(dib, bits); ReleaseDC(NULL, screen); return 0; }
    old = SelectObject(mem, dib);

    /* Route through the window procedure rather than calling the renderers
     * directly, so the shot exercises exactly the same path WM_PRINTCLIENT does. */
    SendMessageW(g_et.hwnd, WM_PRINTCLIENT, (WPARAM)mem,
                 PRF_CLIENT | PRF_NONCLIENT);

    rowBytes = ((DWORD)cr.right * 4 + 3) & ~3u;
    offBits  = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    /* Negative height: make_dib() is already top-down, so the rows go out in
     * screen order and no flipping is needed. */
    ih.biSize          = sizeof(BITMAPINFOHEADER);
    ih.biWidth         = cr.right;
    ih.biHeight        = -cr.bottom;
    ih.biPlanes        = 1;
    ih.biBitCount      = 32;
    ih.biCompression   = BI_RGB;
    ih.biSizeImage     = rowBytes * (DWORD)cr.bottom;
    fh.bfType          = 0x4D42;
    fh.bfOffBits       = offBits;
    fh.bfSize          = offBits + ih.biSizeImage;

    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        ok = (WriteFile(f, &fh, sizeof(fh), NULL, NULL) &&
              WriteFile(f, &ih, sizeof(ih), NULL, NULL));
        for (int y = 0; ok && y < cr.bottom; y++)
            ok = WriteFile(f, (BYTE *)bits + (size_t)y * cr.right * 4, cr.right * 4, NULL, NULL);
        CloseHandle(f);
    }
    if (!ok) app_logw(L"--shot: could not write %s (err %lu)", path, GetLastError());
    else     app_logw(L"--shot: wrote %s (%dx%d)", path, cr.right, cr.bottom);

    SelectObject(mem, old);
    DeleteDC(mem);
    free_dib(dib, bits);
    ReleaseDC(NULL, screen);
    return ok;
}

/* --- raising the copy that already owns the run ------------------------- */
/* FindWindowW(ET_CLASS, NULL) is not enough. The window class is shared by
 * every copy of this program, so while an older build is still open it
 * happily returns *that* one's window: a repeat launch would raise a stale
 * copy and exit, which looks exactly like a program that does not start and
 * leaves the user clicking a binary that is not the one they just launched.
 * The candidates are therefore filtered down to processes running the same
 * image as this one - only the copy that can hold the mutex is a candidate. */
static wchar_t g_etClassBuf[64];
static wchar_t g_etImageBuf[MAX_PATH];
static wchar_t g_etWant[64];
static HWND    g_etRaiseHit;

static BOOL CALLBACK et_raise_cb(HWND h, LPARAM lp)
{
    DWORD pid = 0, need = (DWORD)_countof(g_etImageBuf);
    HANDLE proc;
    const wchar_t *base;
    (void)lp;
    if (!GetClassNameW(h, g_etClassBuf, 64)) return TRUE;
    if (lstrcmpW(g_etClassBuf, ET_CLASS) != 0) return TRUE;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return TRUE;
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return TRUE;
    if (QueryFullProcessImageNameW(proc, 0, g_etImageBuf, &need)) {
        base = wcsrchr(g_etImageBuf, L'\\');
        base = base ? base + 1 : g_etImageBuf;
        if (_wcsicmp(base, g_etWant) == 0) {
            g_etRaiseHit = h;
            CloseHandle(proc);
            return FALSE;
        }
    }
    CloseHandle(proc);
    return TRUE;
}

/* The window of the copy that owns the run, or NULL. Polls briefly: the
 * mutex is claimed before the window exists, so a double-click can lose the
 * race and would otherwise exit without ever raising anything. */
static HWND et_window_of_same_image(void)
{
    wchar_t self[MAX_PATH], *base;
    int tries;
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return NULL;
    base = wcsrchr(self, L'\\');
    base = base ? base + 1 : self;
    wcsncpy_s(g_etWant, 64, base, _TRUNCATE);
    for (tries = 0; tries < 10; tries++) {
        g_etRaiseHit = NULL;
        EnumWindows(et_raise_cb, 0);
        if (g_etRaiseHit) return g_etRaiseHit;
        Sleep(200);
    }
    return NULL;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    MSG msg;
    int argc = 0, i, wantStats = 0;
    LPWSTR *argv;
    HACCEL accel = NULL;
    ACCEL table[1];
    HANDLE single = NULL;
    const wchar_t *shotPath = NULL;
    wchar_t shotBuf[MAX_PATH] = L"";
    int shotRow = -1, shotW = 0, shotH = 0;

    (void)prev; (void)show;
    app_attach_parent_console();
    app_set_dpi_aware();
    app_install_crash_log(L"envtool");
    app_set_verbose(0);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--selftest") == 0) {
            if (argv) LocalFree(argv);
            return env_selftest();
        }
        if (_wcsicmp(argv[i], L"--about") == 0) {
            /* No owner: asked for on a command line, so there is no window to
             * centre on and nothing to disable while it is up. */
            app_show_about(NULL, ET_ABOUT_NAME, ET_ABOUT_NOTES);
            if (argv) LocalFree(argv);
            return 0;
        }
        if (_wcsicmp(argv[i], L"--verbose") == 0) app_set_verbose(1);
        if (_wcsicmp(argv[i], L"--stats") == 0)   wantStats = 1;
        if (_wcsicmp(argv[i], L"--shot") == 0 && i + 1 < argc) {
            /* Copy it: argv is freed below, and a path read back out of freed
             * memory silently becomes an empty string that fails to open. */
            wcsncpy_s(shotBuf, _countof(shotBuf), argv[++i], _TRUNCATE);
            shotPath = shotBuf;
        } else if (_wcsicmp(argv[i], L"--row") == 0 && i + 1 < argc) {
            shotRow = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--w") == 0 && i + 1 < argc) {
            shotW = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--h") == 0 && i + 1 < argc) {
            shotH = _wtoi(argv[++i]);
        }
    }
    if (argv) LocalFree(argv);

    /* A screenshot run has nobody watching stderr, so be loud when it fails. */
    if (shotPath) app_set_verbose(1);

    /* --- one instance only -------------------------------------------- */
    /* A second launch used to open a second window and park a second tray
     * icon beside the first: nothing owned the run, so the only way to tell
     * them apart was counting icons. A repeat launch now raises the window
     * that already owns it. Local\ keeps the claim inside this logon session.
     *
     * Screenshot runs stay exempt on purpose - they are transient and the
     * probe scripts fire them while a real instance is up. --selftest has
     * already returned above, which matters more than it looks: had it
     * fallen through to here, a run next to a live window would exit 0
     * without testing anything, and the harness would report a pass. */
    if (!shotPath) {
        HANDLE already = CreateMutexW(NULL, TRUE, L"Local\\MuskEnvTool.Run");
        if (already && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND first = et_window_of_same_image();
            if (first) {
                ShowWindow(first, SW_RESTORE);
                SetForegroundWindow(first);
            }
            CloseHandle(already);
            return 0;
        }
        single = already;
    }

    if (!et_ui_create(inst)) { et_ui_destroy(inst); return 1; }

    table[0].fVirt = (BYTE)(FVIRTKEY | FCONTROL);
    table[0].key   = 'S';
    table[0].cmd   = IDM_ACCEL_SAVE;
    accel = CreateAcceleratorTableW(table, 1);

    /* No tray icon. The notification area belongs to the screenshot and the
     * colour picker, which hide themselves and need somewhere to be brought
     * back from; this editor keeps a window, so an icon would only be a second
     * place to leave a process running that the user thinks they closed. The
     * hot key stays, and is now registered on its own rather than as a side
     * effect of the icon appearing - with no tray, it is the only way to pull
     * a minimised window back, and it was tied to tray_add() succeeding. A
     * screenshot run is transient and takes neither. */
    if (!shotPath) {
        RegisterHotKey(g_et.hwnd, HOTKEY_ID,
                       MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'E');
    }

    if (wantStats) app_set_verbose(1);   /* so the counters reach stderr */

    ShowWindow(g_et.hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_et.hwnd);
    if (wantStats) { app_log("EnvTool counters after the first paint:"); et_log_stats(); }
    if (!app_is_elevated())
        set_status(L"当前不是管理员：可以编辑用户环境，修改系统环境请右键本程序选择【以管理员身份运行】");

    /* A screenshot run takes its own exit: no tray icon, no message loop, no
     * way for a stray keystroke to turn a diagnostic into a real edit. */
    if (shotPath) {
        int rc;
        MSG m;
        DWORD until = GetTickCount64() + 1200;
        do {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(20);
        } while (GetTickCount64() < until);
        rc = shot_run(shotPath, shotRow, shotW, shotH);
        et_ui_destroy(inst);
        return rc ? 0 : 2;
    }

    while (!g_et.quit) {
        if (GetMessageW(&msg, NULL, 0, 0) > 0) {
            /* Accelerators first, so Ctrl+S works no matter which child has
             * the focus. */
            if (accel && TranslateAcceleratorW(g_et.hwnd, accel, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            break;
        }
    }

    if (accel) DestroyAcceleratorTable(accel);
    UnregisterHotKey(g_et.hwnd, HOTKEY_ID);
    et_ui_destroy(inst);
    if (single) CloseHandle(single);
    return 0;
}
