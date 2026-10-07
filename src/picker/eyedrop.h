/* eyedrop.h - screen freeze + magnified colour picking. */
#ifndef PICKER_EYEDROP_H
#define PICKER_EYEDROP_H

#include "../common/winutil.h"
#include "../common/uikit.h"
#include "../common/tray.h"
#include "color.h"
#include "palette.h"
#include <gdiplus.h>

/* Runs the magnifier picker. The screen is captured up front, so the loupe
 * can never contaminate the pixel it reports.
 * Returns 1 and writes *out when the user picks, 0 on cancel. */
int eyedrop_begin(HWND owner, Rgb *out);

/* Is a magnifier picker up right now? It runs a nested message loop, so the
 * tray command, the hotkey and the D key can all arrive again while it is
 * still open; without this a second call freezes the screen a second time and
 * stacks a loupe on the first. The caller skips the command instead. */
int eyedrop_active(void);

#endif /* PICKER_EYEDROP_H */
