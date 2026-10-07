/* tray.c - notification-area icon with a context menu. */
#include "tray.h"

/* Fills in everything both tray_init flavours share. The icon is passed in
 * rather than loaded here because the two entry points differ only in where
 * the HICON comes from and who owns it. */
static void tray_common(Tray *t, HWND owner, UINT callbackMsg,
                        HICON icon, int ownsIcon, const wchar_t *tip)
{
    memset(t, 0, sizeof(*t));
    t->nid.cbSize           = sizeof(NOTIFYICONDATAW);
    t->nid.hWnd             = owner;
    t->nid.uID              = 1;
    t->nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    t->nid.uCallbackMessage = callbackMsg;
    t->nid.hIcon            = icon;
    t->ownsIcon             = ownsIcon;
    wcsncpy(t->nid.szTip, tip ? tip : L"", 127);
    t->nid.szTip[127] = 0;
    t->nid.uVersion          = NOTIFYICON_VERSION_4;
    t->taskbarRebuilt = RegisterWindowMessageW(L"TaskbarCreated");
}

void tray_init(Tray *t, HINSTANCE inst, HWND owner, UINT callbackMsg,
               int iconResId, const wchar_t *tip)
{
    /* LoadIcon hands back a shared system icon: destroying it is a no-op at
     * best, so this one is not ours to free. */
    HICON icon = LoadIconW(inst, (LPCWSTR)(ULONG_PTR)(UINT)iconResId);
    tray_common(t, owner, callbackMsg, icon, 0, tip);
}

void tray_init_icon(Tray *t, HWND owner, UINT callbackMsg,
                    HICON icon, const wchar_t *tip)
{
    if (!icon) {
        tray_init(t, GetModuleHandleW(NULL), owner, callbackMsg,
                  32512 /*IDI_APPLICATION*/, tip);
        return;
    }
    tray_common(t, owner, callbackMsg, icon, 1, tip);
}

int tray_add(Tray *t)
{
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &t->nid);
    if (!ok) return 0;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &t->nid)) {
        /* version unsupported; icon is still shown, ignore */
    }
    t->visible = 1;
    return 1;
}

int tray_remove(Tray *t)
{
    if (!t->visible) return 1;
    Shell_NotifyIconW(NIM_DELETE, &t->nid);
    t->visible = 0;
    return 1;
}

int tray_recreate(Tray *t)
{
    tray_remove(t);
    return tray_add(t);
}

/* The tooltip doubles as the only always-visible statement of which shortcut
 * is in force, so it has to be updatable after the icon exists. */
void tray_set_tip(Tray *t, const wchar_t *tip)
{
    if (!t->visible) return;
    wcsncpy(t->nid.szTip, tip ? tip : L"", 127);
    t->nid.szTip[127] = 0;
    t->nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &t->nid);
    t->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

HMENU tray_build_menu(Tray *t, int count, const wchar_t *const *items)
{
    HMENU m;
    int i;
    /* A rebuild while a menu from this tray is tracking would destroy the
     * menu under TrackPopupMenu's feet. The callers guard with menuUp before
     * they get here; this is the last line of defence. */
    if (t->menuUp) return t->menu;
    if (t->menu) { DestroyMenu(t->menu); t->menu = NULL; }
    m = CreatePopupMenu();
    if (!m) return NULL;
    for (i = 0; i < count; i++) {
        const wchar_t *s = items[i];
        const wchar_t *tab = wcschr(s, L'\t');
        wchar_t *label;
        wchar_t *p;
        if (!tab) {
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            continue;
        }
        /* A "-" (or empty) label is how the callers spell "separator", even
         * though the format always carries a command-id field. Without this
         * the row comes out as a clickable item labelled "-" that silently
         * does nothing, because its id is 0. */
        {
            size_t n = (size_t)(tab - s);
            int sep = (n == 0 || (n == 1 && s[0] == L'-'));
            if (sep) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); continue; }
        }
        {
            UINT id = (UINT)wcstoul(tab + 1, NULL, 10);
            if (!id) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); continue; }
            label = (wchar_t *)xwcsdup(s);
            if (!label) break;
            p = wcschr(label, L'\t');
            if (p) *p = 0;
            AppendMenuW(m, MF_STRING, id, label);
            xfree(label);
        }
    }
    t->menu = m;
    return m;
}

int tray_menu_up(const Tray *t)
{
    return t ? t->menuUp : 0;
}

void tray_menu_set_up(Tray *t, int up)
{
    if (t) t->menuUp = up ? 1 : 0;
}

void tray_balloon(Tray *t, const wchar_t *title, const wchar_t *msg,
                  DWORD iconInfo)
{
    NOTIFYICONDATAW n;
    memcpy(&n, &t->nid, sizeof(n));
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = iconInfo;
    wcsncpy(n.szInfoTitle, title ? title : L"", 63); n.szInfoTitle[63] = 0;
    wcsncpy(n.szInfo, msg ? msg : L"", 127); n.szInfo[127] = 0;
    if (Shell_NotifyIconW(NIM_MODIFY, &n) && msg && *msg)
        Shell_NotifyIconW(NIM_MODIFY, &t->nid);
}

void tray_free(Tray *t)
{
    tray_remove(t);
    if (t->menu) { DestroyMenu(t->menu); t->menu = NULL; }
    /* Only an icon we built ourselves may be destroyed; the ones LoadIcon
     * returns belong to the system and must be left alone. */
    if (t->ownsIcon && t->nid.hIcon) { DestroyIcon(t->nid.hIcon); t->nid.hIcon = NULL; }
}
