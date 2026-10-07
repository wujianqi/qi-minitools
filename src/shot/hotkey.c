/* hotkey.c - the global capture shortcut, and what to do when some other
 * program already owns the combination the user wants.
 *
 * Registering a global hot key can fail, and when it does there is no way to
 * ask the shell who won it. So this module never treats one combination as
 * *the* shortcut:
 *
 *   1. Whatever the user last used successfully is tried first, and is
 *      remembered in HKCU, so a working shortcut stays working across restarts.
 *   2. Failing that, the default is Ctrl+Shift+A - the owner's choice, set
 *      by hand, and not open for relitigating by tooling.
 *   3. If *that* is taken, the app does not go looking for a substitute. It
 *      hands the problem to the user: main.c opens the shortcut dialog at
 *      startup, because a combination somebody else owns is a decision only
 *      they can make, and silently landing on a different one is how a user
 *      ends up pressing Ctrl+Shift+A forever and wondering why nothing happens.
 *
 * The settings dialog closes the loop: it lets the user press a combination
 * and reports *before* they commit whether the system will actually grant it,
 * by asking the OS with a throwaway RegisterHotKey on a NULL window.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"

/* One default, on purpose, and it is this one: Ctrl+Shift+A is the default
 * the owner of this tool set, by hand, and it is not open for relitigating -
 * nobody ships a different keybinding "because another program uses it".
 * If a future change is tempted to touch this line, don't. */
static const Hotkey HOTKEY_DEFAULT = { MOD_CONTROL | MOD_SHIFT, (UINT)'A' };

/* Read access for the self-test, which pins the default against drift. */
void hotkey_default_of(Hotkey *out)
{
    if (out) *out = HOTKEY_DEFAULT;
}

/* Combinations an earlier build ever defaulted to. A machine that ran one of
 * those builds has one written to the registry, and honouring it would
 * quietly keep the user on a shortcut nobody asked for. They are read once,
 * ignored, and overwritten by the default - unless the value is the default
 * itself, or something no chain ever produced, which is a combination the
 * user chose and is therefore theirs to keep. Ctrl+Shift+A being moved INTO
 * this list would be a bug: it is the default now. */
static int is_retired_default(const Hotkey *hk)
{
    static const Hotkey retired[] = {
        { MOD_CONTROL | MOD_ALT,   (UINT)'A' },
        { MOD_CONTROL | MOD_ALT,   (UINT)'S' },
        { MOD_CONTROL | MOD_SHIFT, (UINT)'S' },
        { MOD_CONTROL | MOD_ALT,   (UINT)'Q' },
        { MOD_CONTROL | MOD_SHIFT, (UINT)'Q' },
    };
    int i;
    if (!hk) return 0;
    for (i = 0; i < (int)(sizeof(retired) / sizeof(retired[0])); i++)
        if (retired[i].mods == hk->mods && retired[i].vk == hk->vk) return 1;
    return 0;
}

#define HK_VALUE   L"Capture"
#define HK_SUBKEY  L"Software\\MuskTools\\Shot"

/* What this process is currently holding, so hotkey_available() can tell its
 * own claim apart from somebody else's. Kept next to the registry settings
 * because it is the same fact: the combination in force right now. */
static struct {
    HWND   hwnd;
    int    id;
    UINT   mods;
    UINT   vk;
} g_live;

/* ------------------------------------------------------------------ */
/* text form                                                            */
/* ------------------------------------------------------------------ */

/* "Ctrl+Alt+A" - the same string the registry holds, so it can be read back
 * by hand if a shortcut ever needs to be rescued from the outside. */
wchar_t *hotkey_text(const Hotkey *hk)
{
    wchar_t buf[64];
    size_t n = 0;
    buf[0] = 0;
    if (!hk) return xwcsdup(L"(未设置)");
    if (hk->mods & MOD_CONTROL) { buf[n++] = L'C'; buf[n++] = L't'; buf[n++] = L'r'; buf[n++] = L'l'; buf[n++] = L'+'; }
    if (hk->mods & MOD_ALT)     { buf[n++] = L'A'; buf[n++] = L'l'; buf[n++] = L't'; buf[n++] = L'+'; }
    if (hk->mods & MOD_SHIFT)   { buf[n++] = L'S'; buf[n++] = L'h'; buf[n++] = L'i'; buf[n++] = L'f'; buf[n++] = L't'; buf[n++] = L'+'; }
    if (hk->mods & MOD_WIN)     { buf[n++] = L'W'; buf[n++] = L'i'; buf[n++] = L'n'; buf[n++] = L'+'; }

    if ((hk->vk >= 'A' && hk->vk <= 'Z') || (hk->vk >= '0' && hk->vk <= '9')) {
        buf[n++] = (wchar_t)hk->vk;
    } else if (hk->vk >= VK_F1 && hk->vk <= VK_F24) {
        _snwprintf_s(buf + n, 64 - n, _TRUNCATE, L"F%u", hk->vk - VK_F1 + 1u);
        n = wcslen(buf);
    } else {
        const wchar_t *nm = NULL;
        switch (hk->vk) {
        case VK_SPACE: nm = L"Space"; break;
        case VK_RETURN: nm = L"Enter"; break;
        case VK_TAB:   nm = L"Tab";   break;
        case VK_ESCAPE:nm = L"Esc";   break;
        case VK_PRIOR: nm = L"PgUp";  break;
        case VK_NEXT:  nm = L"PgDn";  break;
        case VK_LEFT:  nm = L"Left";  break;
        case VK_RIGHT: nm = L"Right"; break;
        case VK_UP:    nm = L"Up";    break;
        case VK_DOWN:  nm = L"Down";  break;
        default:       nm = L"?";     break;
        }
        wcsncpy_s(buf + n, 64 - n, nm, _TRUNCATE);
        n = wcslen(buf);
    }
    buf[n] = 0;
    return xwcsdup(buf);
}

static int token_is(const wchar_t *tok, const wchar_t *word)
{
    size_t i;
    for (i = 0; tok[i] && word[i]; i++) {
        wchar_t a = tok[i], b = word[i];
        if (a >= L'a' && a <= L'z') a = (wchar_t)(a - 32);
        if (b >= L'a' && b <= L'z') b = (wchar_t)(b - 32);
        if (a != b) return 0;
    }
    return tok[i] == 0 && word[i] == 0;
}

/* Splits on '+' by hand rather than with wcstok_s, which is not part of the
 * C library this toolchain links against. */
static int split_plus(const wchar_t *s, wchar_t parts[][24], int maxParts)
{
    const wchar_t *p = s;
    int n = 0;
    while (*p && n < maxParts) {
        int k = 0;
        while (*p && *p != L'+' && k < 23) parts[n][k++] = *p++;
        parts[n][k] = 0;
        n++;
        if (*p == L'+') p++;
        else break;
    }
    return n;
}

/* Accepts what hotkey_text writes, and nothing surprising: it refuses a bare
 * key with no modifier, because that combination would swallow an ordinary
 * keystroke system-wide. */
int hotkey_parse(const wchar_t *s, Hotkey *out)
{
    wchar_t parts[8][24];
    int nparts, i, sawMod = 0;
    UINT mods = 0, vk = 0;

    if (!s || !out) return 0;
    nparts = split_plus(s, parts, 8);
    if (nparts < 2) return 0;

    for (i = 0; i < nparts; i++) {
        if (token_is(parts[i], L"Ctrl") || token_is(parts[i], L"Control")) {
            mods |= MOD_CONTROL; sawMod = 1;
        } else if (token_is(parts[i], L"Alt")) {
            mods |= MOD_ALT; sawMod = 1;
        } else if (token_is(parts[i], L"Shift")) {
            mods |= MOD_SHIFT; sawMod = 1;
        } else if (token_is(parts[i], L"Win") || token_is(parts[i], L"Windows")) {
            mods |= MOD_WIN; sawMod = 1;
        } else if (i == nparts - 1) {
            wchar_t c = parts[i][0];
            if (parts[i][1] == 0 && c >= 0 && c < 128) {
                if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
                vk = (UINT)c;
            } else if ((parts[i][0] == L'F' || parts[i][0] == L'f') &&
                       parts[i][1] != 0 && parts[i][2] == 0 &&
                       parts[i][1] >= L'1' && parts[i][1] <= L'9') {
                vk = VK_F1 + (UINT)(parts[i][1] - L'1');
            } else {
                return 0;
            }
        } else {
            return 0;   /* a modifier we do not know */
        }
    }
    if (!sawMod || !vk) return 0;
    out->mods = mods;
    out->vk = vk;
    return 1;
}

/* ------------------------------------------------------------------ */
/* availability probe                                                   */
/* ------------------------------------------------------------------ */

/* Asks the OS whether this combination is free, without keeping it. A NULL
 * window makes the registration thread-global, which is exactly the same kind
 * of claim the real one makes - so a "yes" here means a real one will succeed
 * too, and a "no" means somebody else already has it.
 *
 * "Somebody else" has to mean somebody else. The app is holding its own
 * shortcut while this runs - it registered one at startup, before the dialog
 * ever opened - so a throwaway registration of the combination the user is
 * currently using fails against this very process. Left alone that reports the
 * shortcut in force as taken, paints 已被占用 next to it, and makes OK refuse
 * with a beep: the user opens the dialog to look at the shortcut they have been
 * using all day and is told it is in use, with no way to accept it. The claim is
 * therefore given up for the length of the probe and taken straight back, which
 * is the only moment at which "is anybody *else* holding this?" can be asked
 * honestly.
 */
int hotkey_available(const Hotkey *hk)
{
    int ok, ours = 0;

    if (!hk || !hk->vk) return 0;
    if (g_live.hwnd && g_live.id && g_live.mods == hk->mods && g_live.vk == hk->vk) {
        UnregisterHotKey(g_live.hwnd, g_live.id);
        ours = 1;
    }
    ok = RegisterHotKey(NULL, 0xBEEF, hk->mods | MOD_NOREPEAT, hk->vk) ? 1 : 0;
    if (ok) UnregisterHotKey(NULL, 0xBEEF);
    if (ours)
        RegisterHotKey(g_live.hwnd, g_live.id, g_live.mods | MOD_NOREPEAT, g_live.vk);
    return ok;
}

/* ------------------------------------------------------------------ */
/* registration                                                         */
/* ------------------------------------------------------------------ */

static void store_hotkey(const Hotkey *hk)
{
    HKEY k;
    wchar_t *txt = hotkey_text(hk);
    if (!txt) return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, HK_SUBKEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, HK_VALUE, 0, REG_SZ,
                       (const BYTE *)txt,
                       (DWORD)((wcslen(txt) + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
    xfree(txt);
}

/* Persist a combination the user just chose, so the next launch tries it
 * before anything else. Without this a shortcut set through the dialog works
 * until the app is restarted, and then silently reverts. */
void hotkey_store(const Hotkey *hk)
{
    if (hk && hk->vk) store_hotkey(hk);
}

/* Registers the shortcut in force and reports whether it worked, so the caller
 * can tell the user instead of leaving them wondering why nothing happens when
 * they press the keys.
 *
 * Two combinations are ever tried: what the user last had working, and the
 * default. There is no third guess - see the note at the top of the file.
 *
 * Returns the id the hotkey was registered under, or 0 if it could not be
 * registered at all. *out receives the combination actually in force; *refused,
 * when asked for, receives the one the system turned down, so the caller can
 * name it rather than saying "something is in the way". */
int hotkey_register(HWND hwnd, Hotkey *out, Hotkey *refused)
{
    Hotkey hk;
    HKEY k;
    int id;

    if (refused) memset(refused, 0, sizeof(*refused));

    /* 1. last time's winner, so a working shortcut stays working. */
    memset(&hk, 0, sizeof(hk));
    if (RegOpenKeyExW(HKEY_CURRENT_USER, HK_SUBKEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[64];
        DWORD cb = sizeof(buf), type = 0;
        int parsed =
            RegQueryValueExW(k, HK_VALUE, NULL, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && hotkey_parse(buf, &hk);
        RegCloseKey(k);
        if (parsed && is_retired_default(&hk)) parsed = 0;
        if (parsed) {
            for (id = 1; id <= HOTKEY_SLOTS; id++) {
                if (RegisterHotKey(hwnd, id, hk.mods | MOD_NOREPEAT, hk.vk)) {
                    if (out) *out = hk;
                    g_live.hwnd = hwnd; g_live.id = id;
                    g_live.mods = hk.mods; g_live.vk = hk.vk;
                    return id;
                }
            }
            if (refused) *refused = hk;
        }
    }

    /* 2. the default, and nothing after it. */
    for (id = 1; id <= HOTKEY_SLOTS; id++) {
        if (RegisterHotKey(hwnd, id, HOTKEY_DEFAULT.mods | MOD_NOREPEAT,
                           HOTKEY_DEFAULT.vk)) {
            if (out) *out = HOTKEY_DEFAULT;
            /* Written on the way past, so the next launch does not read a
             * retired combination out of the registry and try it first. */
            store_hotkey(&HOTKEY_DEFAULT);
            g_live.hwnd = hwnd; g_live.id = id;
            g_live.mods = HOTKEY_DEFAULT.mods;
            g_live.vk = HOTKEY_DEFAULT.vk;
            return id;
        }
    }
    if (refused) *refused = HOTKEY_DEFAULT;
    return 0;
}

void hotkey_unregister(HWND hwnd)
{
    int id;
    for (id = 1; id <= HOTKEY_SLOTS; id++) UnregisterHotKey(hwnd, id);
    memset(&g_live, 0, sizeof(g_live));
}

/* Records a claim made by hand rather than through hotkey_register, so the
 * availability probe can still recognise it as ours. edit_hotkey() in
 * main.c registers the combination the user just chose itself - it has to, so
 * that the old one is already free - and without this it would leave the
 * availability probe treating the app's own new shortcut as somebody else's,
 * which is the same refusal to accept the shortcut in force all over again.
 */
void hotkey_note_claim(HWND hwnd, int id, const Hotkey *hk)
{
    if (!hk || !hk->vk) { memset(&g_live, 0, sizeof(g_live)); return; }
    g_live.hwnd = hwnd;
    g_live.id   = id;
    g_live.mods = hk->mods;
    g_live.vk   = hk->vk;
}

/* ------------------------------------------------------------------ */
/* the conflict text, in one place                                      */
/* ------------------------------------------------------------------ */

void hotkey_conflict_hint(const Hotkey *want, wchar_t *out, int cch)
{
    wchar_t *a = hotkey_text(want);
    if (out && cch > 0)
        _snwprintf_s(out, cch, _TRUNCATE,
                     L"%s 已被其他程序占用，换一个吧", a ? a : L"这个组合");
    if (a) xfree(a);
}

/* ------------------------------------------------------------------ */
/* settings dialog                                                      */
/* ------------------------------------------------------------------ */

#define DLG_CAPTURE  101     /* the box that records the next combination */
#define DLG_OK       102
#define DLG_DEFAULT  103
#define DLG_CANCEL   104

typedef struct {
    HWND     hwnd;
    HWND     capture;
    Hotkey   cur;
    Hotkey   got;
    int      have;
    int      fresh;      /* the user pressed something this session */
    int      accepted;   /* OK was pressed on something the OS will grant */
    int      btnY;       /* button row, measured off the real client rect */
    int      S;
} HotkeyDlg;

static HotkeyDlg g_hkd;

static void paint_capture(HotkeyDlg *d)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(d->capture, &ps);
    RECT rc;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    wchar_t *txt;

    GetClientRect(d->capture, &rc);
    SetDCBrushColor(dc, RGB(38, 38, 38));
    FillRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    SelectObject(dc, font);

    txt = d->have ? hotkey_text(&d->got) : xwcsdup(L"点这里，然后按下想要的快捷键");
    if (txt) {
        RECT t = rc;
        if (!d->have) SetTextColor(dc, RGB(170, 170, 170));
        DrawTextW(dc, txt, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        xfree(txt);
    }

    if (d->have) {
        int okFree = hotkey_available(&d->got);
        wchar_t note[32];
        RECT t = rc;
        _snwprintf_s(note, 32, _TRUNCATE, okFree ? L"可用" : L"已被占用");
        SetTextColor(dc, okFree ? RGB(120, 230, 150) : RGB(255, 130, 120));
        t.left = (int)((double)rc.right * 0.66);
        DrawTextW(dc, note, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    EndPaint(d->capture, &ps);
}

/* Turns a key-down into a combination. Modifier keys on their own are ignored
 * so the box fills in once the real key arrives, not when Shift is pressed.
 *
 * The live key state is the authority, because that is what a user pressing
 * the combination actually holds. It is not the only source though: a key
 * message also carries the modifier state in the high byte of lParam, and
 * that is the only thing present for a synthesised message - so it is used as
 * the fallback, which is what lets the outside probes drive this box. */
static void record_combo(HotkeyDlg *d, WPARAM key, LPARAM lp)
{
    UINT mods = 0, vk = (UINT)key, st;
    Hotkey hk;

    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU)     & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT)    & 0x8000) mods |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) & 0x8000 || GetKeyState(VK_RWIN) & 0x8000) mods |= MOD_WIN;

    if (!mods) {
        st = (UINT)((lp >> 16) & 0xFF);   /* Shift, Ctrl, Alt, in that order */
        if (st & 0x02) mods |= MOD_CONTROL;
        if (st & 0x04) mods |= MOD_ALT;
        if (st & 0x01) mods |= MOD_SHIFT;
    }

    /* A bare modifier is not a shortcut; wait for the key that follows it. */
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
        vk == VK_LWIN || vk == VK_RWIN || vk == 0)
        return;
    if (!mods) {
        d->have = 0;                 /* no modifier: not a global shortcut */
        d->fresh = 1;
        InvalidateRect(d->capture, NULL, FALSE);
        return;
    }
    /* AltGr arrives as Ctrl+Alt and is a dead end for a global hot key. */
    if (mods == (MOD_CONTROL | MOD_ALT)) return;

    hk.mods = mods;
    hk.vk = vk;
    d->got = hk;
    d->have = 1;
    d->fresh = 1;
    InvalidateRect(d->capture, NULL, FALSE);
}

static LRESULT CALLBACK hkd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    HotkeyDlg *d = &g_hkd;

    switch (m) {
    case WM_PAINT:
        if (h == d->capture) { paint_capture(d); return 0; }
        break;

    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED) {
            int id = LOWORD(w);
            if (id == DLG_OK) {
                /* Refuse to save something the system will not hand over -
                 * that is the whole point of showing the availability. */
                if (d->have && d->fresh && hotkey_available(&d->got)) {
                    d->accepted = 1;
                    DestroyWindow(h);
                } else {
                    MessageBeep(MB_ICONWARNING);
                }
                return 0;
            }
            if (id == DLG_DEFAULT) {
                d->got = HOTKEY_DEFAULT;
                d->have = 1;
                d->fresh = 1;
                InvalidateRect(d->capture, NULL, FALSE);
                return 0;
            }
            if (id == DLG_CANCEL) { DestroyWindow(h); return 0; }
        }
        break;

    case WM_KEYDOWN:
        if (GetFocus() == d->capture) {
            record_combo(d, w, l);
            return 0;
        }
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (w == VK_RETURN) { SendMessageW(h, WM_COMMAND, (WPARAM)DLG_OK, 0); return 0; }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w;
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    }
    return DefWindowProcW(h, m, w, l);
}

/* Modal. Returns 1 and fills *out with a combination the OS will grant, or 0
 * if the user backed out. */
int hotkey_dialog(HWND owner, const Hotkey *cur, Hotkey *out)
{
    static const wchar_t *CLASS = L"MuskShotHotkeyDlg";
    WNDCLASSEXW wc;
    HotkeyDlg *d = &g_hkd;
    MSG msg;
    wchar_t *curTxt;
    RECT o;
    int S = app_scale_px(1);
    int W = app_scale_px(430), H = app_scale_px(230);
    int ok = 0;

    curTxt = hotkey_text(cur);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = hkd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (curTxt) xfree(curTxt);
        return 0;
    }

    memset(d, 0, sizeof(*d));
    d->S = S;
    if (cur) d->cur = *cur;

    GetWindowRect(owner ? owner : NULL, &o);
    d->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, CLASS,
                              L"设置截图快捷键",
                              WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
                              o.left + 40, o.top + 40, W, H,
                              owner, NULL, wc.hInstance, NULL);
    if (!d->hwnd) {
        UnregisterClassW(CLASS, wc.hInstance);
        if (curTxt) xfree(curTxt);
        return 0;
    }

    /* The buttons are laid out from the real client rectangle. Placing them
     * relative to the window height instead puts them under the caption, off
     * the bottom of the client area - which is exactly how "确定" and "取消"
     * ended up invisible. */
    {
        RECT cr;
        int ch;
        GetClientRect(d->hwnd, &cr);
        ch = cr.bottom - cr.top;
        d->btnY = ch - S * 40;
    }

    {
        wchar_t cap[128];
        _snwprintf_s(cap, 128, _TRUNCATE, L"当前：%s", curTxt ? curTxt : L"(未设置)");
        CreateWindowExW(0, L"STATIC", cap, WS_CHILD | WS_VISIBLE,
                        S * 16, S * 12, W - S * 32, S * 18,
                        d->hwnd, (HMENU)(INT_PTR)200, wc.hInstance, NULL);
    }
    d->capture = CreateWindowExW(WS_EX_CLIENTEDGE, L"BUTTON",
                                 L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 S * 16, S * 34, W - S * 32, S * 34,
                                 d->hwnd, (HMENU)(INT_PTR)DLG_CAPTURE, wc.hInstance, NULL);

    CreateWindowExW(0, L"STATIC",
                    L"必须带 Ctrl / Alt / Shift / Win 中的至少一个修饰键。\r\n"
                    L"右侧会实时检测这个组合有没有被别的程序占用。",
                    WS_CHILD | WS_VISIBLE, S * 16, S * 74, W - S * 32, S * 34,
                    d->hwnd, (HMENU)(INT_PTR)201, wc.hInstance, NULL);

    CreateWindowExW(0, L"BUTTON", L"恢复默认", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    W - S * 310, d->btnY, S * 70, S * 26,
                    d->hwnd, (HMENU)(INT_PTR)DLG_DEFAULT, wc.hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    W - S * 106, d->btnY, S * 44, S * 26,
                    d->hwnd, (HMENU)(INT_PTR)DLG_CANCEL, wc.hInstance, NULL);
    CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    W - S * 52, d->btnY, S * 36, S * 26,
                    d->hwnd, (HMENU)(INT_PTR)DLG_OK, wc.hInstance, NULL);

    SetFocus(d->capture);
    SetForegroundWindow(d->hwnd);

    while (IsWindow(d->hwnd) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(d->hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (d->accepted) {
        *out = d->got;
        ok = 1;
    }
    UnregisterClassW(CLASS, wc.hInstance);
    if (curTxt) xfree(curTxt);
    return ok;
}
