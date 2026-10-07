/* trayicon.h - the application icon, rendered from logo.svg's geometry. */
#ifndef TRAYICON_H
#define TRAYICON_H

#include "uikit.h"   /* winutil.h + gdiplus.h, for ARGB and the gx_ wrappers */

/* One fill per program, so the four .exe files are told apart at a glance in
 * Explorer, on the taskbar and in Alt+Tab instead of being four copies of the
 * same purple. logo.svg ships a dark slate fill (#3F4E62), which all but
 * disappears against a dark taskbar, so every colour here is bright enough to
 * still read at 16px.
 *
 * The four step across violet -> fuchsia -> rose -> orange at one lightness, so
 * they still read as a family rather than as four unrelated stickers - and they
 * deliberately skip green and blue, which are held back for whatever programs
 * come next. Every pair is more than 28 apart on at least one channel, which is
 * what lets build.ps1 tell one exe's icon from another's.
 *
 * A program claims one with TRAY_ICON_COLOR (src\shot\main.c,
 * src\picker\picker.c). slice and envtool have no tray icon, so they only need
 * it for the .ico build.ps1 embeds. build.ps1 lists the same four values and
 * checks the icon it just embedded against them, so a colour changed on one
 * side only fails the build instead of shipping. */
#define APP_ICON_COLOR_SHOT    0xFF7C3AEDu  /* violet  */
#define APP_ICON_COLOR_PICKER  0xFFC026D3u  /* fuchsia */
#define APP_ICON_COLOR_SLICE   0xFFE11D48u  /* rose    */
#define APP_ICON_COLOR_ENVTOOL 0xFFEA580Cu  /* orange  */

/* The resource id each program's own .ico is embedded under. build.ps1 writes
 * the per-program resource script and hard-codes the same number - keep the two
 * in step. */
#define APP_ICON_RES 1

/* Builds the notification-area icon from logo.svg's path data, filled with
 * `color`, at every size the shell asks for. Returns an HICON the caller owns
 * (DestroyIcon), or NULL if the icon could not be built - callers are expected
 * to fall back to a stock system icon in that case. */
HICON trayicon_from_logo(ARGB color);

/* The same artwork as embedded in the executable, at `cx` pixels (0 = the
 * system large-icon size). Falls back to the stock application icon if the
 * resource is missing, so a build without the .rc still runs and still has an
 * icon. The window class keeps it for the lifetime of the process - do not
 * DestroyIcon it. */
HICON app_icon_load(HINSTANCE inst, int cx);

/* Same artwork as a standalone PNG, for taskbar pins / shortcut icons.
 * `size` is the pixel square to render. Returns 1 on success. */
int trayicon_export_png(const wchar_t *path, ARGB color, int size);

/* Same artwork as a multi-size .ico - what build.ps1 embeds into the exes,
 * one file per program (res\logo-<program>.ico) because the four fills differ.
 * Regenerate them with `shot.exe --ico res\logo-shot.ico #7C3AED`, or let
 * `build.ps1 -RegenIcon` do it for all four. Returns 1 on success. */
int trayicon_write_ico(const wchar_t *path, ARGB color);

#endif /* TRAYICON_H */
