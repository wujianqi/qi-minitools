/* about.c - the About box every program shows.
 *
 * One implementation, four programs: each passes its own name and its own usage
 * notes, and the box itself draws the icon, the author and the link. Sharing it
 * is the point - the credit cannot drift, because there is exactly one place in
 * the tree that says who wrote these and it is the same in all four .exe files.
 *
 * MessageBoxW would have been a line of code per program, but it cannot make a
 * URL clickable, and a link the reader has to select and copy by hand is not
 * really a link. So this is a small window: the app's own icon and name, the
 * notes, the credit line, and the URL drawn the way a link should look - blue,
 * underlined, a hand cursor on hover - opening in the default browser.
 *
 * The notes live in a read-only EDIT rather than in painted text on purpose.
 * The box has a fixed layout, so it cannot run out of room: a program with
 * thirteen lines of notes scrolls them and the box stays the same size. Sizing
 * the window to the text instead was tried first and is not worth it - that
 * means a wrapped-text height measured in one place and painted in another,
 * and when the two disagree by one line the link and the button end up below
 * the bottom edge.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "winutil.h"
#include "trayicon.h"   /* app_icon_load, for the icon in the header */

/* The one place these two strings live. */
static const wchar_t *ABOUT_AUTHOR = L"wujianqi";
static const wchar_t *ABOUT_URL    = L"https://github.com/wujianqi/";

/* Same face the windows are drawn in elsewhere. GDI wants 0x00BBGGRR, which is
 * the reverse of the ARGB the GDI+ helpers take, so these are spelled out
 * rather than converted - a silent swap here would only show up as "why is the
 * background blue". */
#define AB_BG      RGB(0xF7, 0xF8, 0xFA)
#define AB_TITLE   RGB(0x1F, 0x24, 0x30)
#define AB_TEXT    RGB(0x37, 0x41, 0x51)
#define AB_DIM     RGB(0x6B, 0x72, 0x80)
#define AB_LINK    RGB(0x25, 0x63, 0xEB)
#define AB_LINK_HI RGB(0x1D, 0x4E, 0xD8)

#define ABOUT_CLASS    L"MuskAboutDlgCls"
#define ID_ABOUT_OK    1
#define ID_ABOUT_NOTES 100

typedef struct {
    HWND           hwnd;
    HWND           notesBox;   /* the read-only EDIT the notes go in */
    const wchar_t *name;
    const wchar_t *notes;      /* the text itself */
    RECT           link;       /* where the URL landed, for hit-testing */
    int            hover;
    HFONT          fTitle, fBody;
} AboutDlg;

/* Modal, so one at a time is one - the same reason the prompt dialog in
 * winutil.c keeps its state in a single static. */
static AboutDlg g_about;

static int pt_in(const RECT *r, int x, int y)
{
    return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

/* Opens the URL, and says so when it could not.
 *
 * The fallback is not decoration: a machine with no browser registered (a
 * stripped Windows image, a locked-down box) makes ShellExecuteW fail, and
 * "nothing happened" is the worst possible answer to a click on a link. So the
 * URL goes to the clipboard instead and the reader is told what happened. */
static int open_url(HWND hwnd)
{
    if ((INT_PTR)ShellExecuteW(hwnd, L"open", ABOUT_URL, NULL, NULL,
                               SW_SHOWNORMAL) > 32)
        return 1;
    if (OpenClipboard(hwnd)) {
        size_t n = wcslen(ABOUT_URL) + 1;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
        EmptyClipboard();
        if (h) {
            wchar_t *p = (wchar_t *)GlobalLock(h);
            if (p) {
                memcpy(p, ABOUT_URL, n * sizeof(wchar_t));
                GlobalUnlock(h);
                if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }
    {
        /* ABOUT_URL is a variable, not a literal, so it cannot simply be
         * appended to the message text - that is what the compiler says when
         * the two are written side by side. Format it instead. */
        wchar_t msg[256];
        _snwprintf_s(msg, 256, _TRUNCATE,
                     L"没能打开浏览器，链接已经复制到剪贴板：\r\n%s", ABOUT_URL);
        MessageBoxW(hwnd, msg, L"关于", MB_OK | MB_ICONWARNING);
    }
    return 0;
}

/* Everything the box is made of, in one place, so the painter and the child
 * controls cannot disagree about where anything is. */
typedef struct {
    RECT notes;    /* the read-only EDIT */
    RECT link;     /* the painted URL    */
    RECT ok;       /* the 确定 button    */
} AboutParts;

static void about_parts(int cw, AboutParts *out)
{
    int S   = app_scale_px(1);
    int pad = S * 20;
    int ico = GetSystemMetrics(SM_CXICON);
    /* The header is two lines tall, not one: the icon beside the name, and the
     * 作者 line under the name. Sizing the stack off the icon alone put the
     * notes box on top of that second line and pushed the link and the button
     * off the bottom of the box. */
    int y   = pad + ico + S * 20 + S * 12;

    SetRect(&out->notes, pad, y, cw - pad, y + S * 210);
    y = out->notes.bottom + S * 10;
    SetRect(&out->link, pad, y, pad + S * 240, y + S * 18);
    y = out->link.bottom + S * 8;
    SetRect(&out->ok, cw - pad - S * 80, y, cw - pad, y + S * 28);
}

static void about_paint(AboutDlg *a, HDC hdc, int cw)
{
    int   S = app_scale_px(1);
    int   pad = S * 20;
    int   ico = GetSystemMetrics(SM_CXICON);
    RECT  rc, r;
    SIZE  ext;
    HICON hIcon;
    AboutParts pt;

    GetClientRect(a->hwnd, &rc);
    /* AB_BG, not the system dialog face: the four windows this box belongs to
     * are all painted #F7F8FA, and a stock grey box in front of them reads as
     * something bolted on from another program. */
    {
        HBRUSH bg = CreateSolidBrush(AB_BG);
        if (bg) { FillRect(hdc, &rc, bg); DeleteObject(bg); }
    }
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, a->fBody);
    about_parts(cw, &pt);

    /* --- header: the app's own icon, its name, the credit --------------- */
    hIcon = app_icon_load(GetModuleHandleW(NULL), 0);
    if (hIcon) DrawIconEx(hdc, pad, pad, hIcon, ico, ico, 0, NULL, DI_NORMAL);

    SelectObject(hdc, a->fTitle);
    SetTextColor(hdc, AB_TITLE);
    r.left = pad + ico + S * 14; r.right = cw - pad;
    r.top = pad - S * 2; r.bottom = pad + ico;
    DrawTextW(hdc, a->name ? a->name : L"", -1, &r,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(hdc, a->fBody);
    SetTextColor(hdc, AB_DIM);
    r.top = pad + ico; r.bottom = r.top + S * 20;
    DrawTextW(hdc, L"作者", -1, &r, DT_SINGLELINE | DT_NOPREFIX);
    GetTextExtentPoint32W(hdc, L"作者", 2, &ext);
    r.left += ext.cx + S * 8;
    SetTextColor(hdc, AB_TEXT);
    DrawTextW(hdc, ABOUT_AUTHOR, -1, &r, DT_SINGLELINE | DT_NOPREFIX);

    /* A hairline under the header, so the icon block reads as a header rather
     * than as a stray icon in a corner. */
    SetRect(&r, pad, pt.notes.top - S * 8, cw - pad, pt.notes.top - S * 8 + 1);
    FillRect(hdc, &r, (HBRUSH)GetStockObject(LTGRAY_BRUSH));

    /* --- the link: painted, because the affordance is the point ---------- */
    SetTextColor(hdc, a->hover ? AB_LINK_HI : AB_LINK);
    r = pt.link;
    GetTextExtentPoint32W(hdc, ABOUT_URL, (int)wcslen(ABOUT_URL), &ext);
    a->link = r;
    a->link.right = a->link.left + ext.cx;
    DrawTextW(hdc, ABOUT_URL, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
    /* Underlined by hand rather than with SS_UNDERLINE: this is painted text,
     * not a static control, and painting it also lets the underline brighten on
     * hover instead of only appearing. */
    r.left = a->link.left;
    r.right = a->link.right;
    r.top = a->link.bottom - S * 4;
    r.bottom = r.top + 1;
    FillRect(hdc, &r, (HBRUSH)GetStockObject(a->hover ? BLACK_BRUSH : DC_BRUSH));
}

static LRESULT CALLBACK about_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    AboutDlg *a = &g_about;
    int       hover;

    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == ID_ABOUT_OK) { DestroyWindow(h); return 0; }
        break;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        /* Paint the notes area the colour of the box instead of leaving the
         * stock white edit field in the middle of it. */
        HDC dc = (HDC)w;
        SetTextColor(dc, AB_TEXT);
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        about_paint(a, dc, rc.right);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                    /* every pixel of the client is painted */

    case WM_LBUTTONDOWN:
        if (pt_in(&a->link, GET_X_LPARAM(l), GET_Y_LPARAM(l))) {
            open_url(h);
            return 0;
        }
        SetCapture(h);
        return 0;

    case WM_LBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        hover = pt_in(&a->link, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hover != a->hover) {
            TRACKMOUSEEVENT t;
            a->hover = hover;
            InvalidateRect(h, NULL, FALSE);
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            t.dwHoverTime = 0;
            TrackMouseEvent(&t);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (a->hover) { a->hover = 0; InvalidateRect(h, NULL, FALSE); }
        return 0;

    case WM_SETCURSOR:
        /* HTCLIENT is the only condition that matters: wParam carries a cursor
         * handle, and comparing it against a MAKEINTRESOURCE value means
         * nothing. lParam is no better as a position source - its low word is
         * the hit-test code and its high word a message number. The cursor is
         * read live with GetCursorPos: WM_SETCURSOR arrives as a sent message
         * where GetMessagePos reports the last posted one, which is not
         * necessarily where this mouse move landed. */
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            if (GetCursorPos(&pt)) {
                ScreenToClient(h, &pt);
                if (pt_in(&a->link, pt.x, pt.y)) {
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                    return 1;
                }
            }
        }
        break;                        /* anything else: the class's arrow */

    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void app_show_about(HWND owner, const wchar_t *name, const wchar_t *notes)
{
    static const wchar_t *cls = NULL;
    WNDCLASSEXW wc;
    AboutDlg   *a = &g_about;
    MSG        msg;
    RECT       own, wr;
    MONITORINFO mi;
    AboutParts pt;
    int        S = app_scale_px(1);
    int        cw = S * 520, W, H, x, y;
    wchar_t    title[128];

    cls = ABOUT_CLASS;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = about_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;      /* every pixel is painted; see about_paint */
    wc.lpszClassName = cls;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    memset(a, 0, sizeof(*a));
    a->name  = name;
    a->notes = notes;
    a->fBody  = CreateFontW(-S * 13, 0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei UI");
    a->fTitle = CreateFontW(-S * 16, 0, 0, 0, 700, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            L"Microsoft YaHei UI");
    /* The name line already carries the app name in full; the caption gets the
     * part before the separator so the title bar is not the whole sentence. */
    {
        wchar_t *dot;
        _snwprintf_s(title, 128, _TRUNCATE, L"关于 %s", name ? name : L"");
        for (dot = title; *dot && *dot != L'·' && *dot != L'-'; dot++) ;
        if (*dot) *dot = 0;
    }

    /* Fixed layout: the client width first, then the frame around it. The notes
     * area has a fixed height and scrolls, so no wrapped-text measurement is
     * involved and nothing can be pushed off the bottom. */
    about_parts(cw, &pt);
    SetRect(&wr, 0, 0, cw, pt.ok.bottom + S * 20);
    AdjustWindowRectEx(&wr, WS_POPUPWINDOW | WS_CAPTION, FALSE,
                       WS_EX_DLGMODALFRAME);
    W = wr.right - wr.left;
    H = wr.bottom - wr.top;

    /* Centre on the owner when there is one - an About belongs in front of the
     * window that opened it. `slice.exe --about` has no window, so that lands
     * in the middle of the screen instead. */
    if (owner && GetWindowRect(owner, &own)) {
        x = own.left + ((own.right - own.left) - W) / 2;
        y = own.top  + ((own.bottom - own.top) - H) / 2;
    } else {
        /* cbSize first, or the call fails and rcWork is never filled in -
         * reading it afterwards would be reading stack that was never written. */
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(MonitorFromWindow(GetDesktopWindow(),
                                               MONITOR_DEFAULTTOPRIMARY), &mi)) {
            mi.rcWork.left = 0; mi.rcWork.top = 0;
            mi.rcWork.right  = GetSystemMetrics(SM_CXSCREEN);
            mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
        y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
    }

    a->hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title,
                              WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE,
                              x, y, W, H, owner, NULL, wc.hInstance, NULL);
    if (!a->hwnd) {
        if (a->fTitle) DeleteObject(a->fTitle);
        if (a->fBody)  DeleteObject(a->fBody);
        return;
    }
    a->notesBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                                  notes ? notes : L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                  ES_MULTILINE | ES_READONLY,
                                  pt.notes.left, pt.notes.top,
                                  pt.notes.right - pt.notes.left,
                                  pt.notes.bottom - pt.notes.top,
                                  a->hwnd, (HMENU)(INT_PTR)ID_ABOUT_NOTES,
                                  wc.hInstance, NULL);
    if (a->notesBox) SendMessageW(a->notesBox, WM_SETFONT, (WPARAM)a->fBody, TRUE);
    CreateWindowExW(0, L"BUTTON", L"确定",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                    pt.ok.left, pt.ok.top,
                    pt.ok.right - pt.ok.left, pt.ok.bottom - pt.ok.top,
                    a->hwnd, (HMENU)(INT_PTR)ID_ABOUT_OK, wc.hInstance, NULL);

    /* Modal the way MessageBox is: the owner stops responding until this
     * closes, so a second About cannot open on top of the first. */
    if (owner) EnableWindow(owner, FALSE);
    SetForegroundWindow(a->hwnd);

    while (IsWindow(a->hwnd)) {
        if (GetMessageW(&msg, NULL, 0, 0) <= 0) break;
        if (msg.message == WM_QUIT) break;
        /* Esc closes the box even with the notes focused: the key message
         * stops at the focused child and would never reach about_proc. */
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            DestroyWindow(a->hwnd);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(a->hwnd)) DestroyWindow(a->hwnd);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    UnregisterClassW(cls, wc.hInstance);
    if (a->fTitle) DeleteObject(a->fTitle);
    if (a->fBody)  DeleteObject(a->fBody);
    a->fTitle = a->fBody = NULL;
    a->hwnd = a->notesBox = NULL;
}
