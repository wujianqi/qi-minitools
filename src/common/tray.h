/* tray.h - notification-area icon with a context menu. */
#ifndef TRAY_H
#define TRAY_H

#include "winutil.h"

typedef struct {
    NOTIFYICONDATAW nid;
    HMENU   menu;
    UINT    taskbarRebuilt;   /* registered on taskbar-created */
    int     visible;
    int     ownsIcon;         /* DestroyIcon on free? LoadIcon icons are shared */
    int     menuUp;           /* a menu is tracking right now; see tray_menu_up */
} Tray;

void tray_init(Tray *t, HINSTANCE inst, HWND owner, UINT callbackMsg,
               int iconResId, const wchar_t *tip);
/* Same, but with a runtime-built icon. The Tray takes ownership: it destroys
 * `icon` in tray_free(), so the caller must not DestroyIcon it itself. Falls
 * back to the stock system icon if `icon` is NULL. */
void tray_init_icon(Tray *t, HWND owner, UINT callbackMsg,
                    HICON icon, const wchar_t *tip);
int  tray_add(Tray *t);
int  tray_remove(Tray *t);
/* Build a menu from "Label\tCmdId" strings; returns the created HMENU. */
HMENU tray_build_menu(Tray *t, int count, const wchar_t *const *items);
/* While one of this tray's menus is tracking, further tray callbacks must be
 * dropped: TrackPopupMenu runs its own message loop that keeps dispatching,
 * so the shell's next callback would otherwise re-enter the handler, destroy
 * the menu being tracked (tray_build_menu frees the old one) and rebuild -
 * which leaves the tracker walking freed memory. */
int  tray_menu_up(const Tray *t);
void tray_menu_set_up(Tray *t, int up);
void tray_balloon(Tray *t, const wchar_t *title, const wchar_t *msg,
                  DWORD iconInfo);
int  tray_recreate(Tray *t);
/* Replace the hover text on the live icon. */
void tray_set_tip(Tray *t, const wchar_t *tip);
void tray_free(Tray *t);

#endif /* TRAY_H */
