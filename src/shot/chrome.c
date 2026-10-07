/* chrome.c - toolbar layout, toolbar painting, tool glyphs and the magnifier. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shot.h"
#include <gdiplus.h>

/* The palette is deliberately loud: these are meant for fast, confident
 * "look here" scribbles. */
static const ARGB PALETTE[10] = {
    0xFFFF3B30, /* red    */
    0xFFFF9500, /* orange */
    0xFFFFCC00, /* yellow */
    0xFF34C759, /* green  */
    0xFF00C7BE, /* teal   */
    0xFF007AFF, /* blue   */
    0xFF5856D6, /* indigo */
    0xFFFF2D55, /* pink   */
    0xFF000000, /* black  */
    0xFFFFFFFF  /* white  */
};
#define PAL_N ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

ARGB shot_palette(int index)
{
    if (index < 0 || index >= PAL_N) return 0;
    return PALETTE[index];
}

int shot_palette_count(void) { return PAL_N; }

/* The 50% black wash applied outside the selection. */
ARGB shot_dim_color(void) { return 0x80000000; }

int shot_text_width(ShotApp *a, const wchar_t *s, int px)
{
    GpFont *f = gx_font(a->fontFamily, (REAL)px);
    REAL w = 0;
    if (!f) return px * 4;
    gx_measure_text(f, s, &w, NULL);
    GdipDeleteFont(f);
    if (w < 1.0f) w = (REAL)px * wcslen(s) * 0.62f;   /* measurement fallback */
    return (int)w;
}

void draw_hud_text(ShotApp *a, GpGraphics *g, const wchar_t *s,
                   int x, int y, int w, int h, ARGB color, int px)
{
    GpFont *f = gx_font(a->fontFamily, (REAL)px);
    if (!f) return;
    gx_draw_text(g, f, color, s, (REAL)x, (REAL)y, (REAL)w, (REAL)h);
    GdipDeleteFont(f);
}

/* ------------------------------------------------------------------ */
/* tool glyphs - drawn on a 24x24 design grid                          */
/* ------------------------------------------------------------------ */

static void draw_glyph(GpGraphics *g, int id, RECT hit, ARGB tint, REAL thick)
{
    GpPen *pen;
    REAL x0 = (REAL)hit.left + 4, y0 = (REAL)hit.top + 4;
    REAL x1 = (REAL)hit.right - 4, y1 = (REAL)hit.bottom - 4;
    REAL cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
    REAL u = (x1 - x0) / 24.0f;

#define X(v) (x0 + (v) * u)
#define Y(v) (y0 + (v) * u)

    pen = gx_pen(tint, thick, LineCapRound, LineJoinRound);
    if (!pen) return;

    switch (id) {
    case TB_PEN: {   /* a nib: shaft plus a triangular tip */
        GpPointF tri[3];
        gx_stroke_line_f(g, pen, X(5), Y(19), X(16), Y(8));
        tri[0].X = X(15); tri[0].Y = Y(6);
        tri[1].X = X(20); tri[1].Y = Y(5);
        tri[2].X = X(18); tri[2].Y = Y(10);
        gx_fill_poly(g, tint, tri, 3);
        break;
    }
    case TB_ERASER: { /* a slanted block */
        GpPointF p[4];
        p[0].X = X(6);  p[0].Y = Y(18);
        p[1].X = X(12); p[1].Y = Y(6);
        p[2].X = X(20); p[2].Y = Y(10);
        p[3].X = X(14); p[3].Y = Y(22);
        gx_fill_poly(g, tint, p, 4);
        break;
    }
    case TB_LINE:
        gx_stroke_line_f(g, pen, X(5), Y(19), X(19), Y(5));
        break;
    case TB_RECT:
        gx_stroke_rect(g, tint, thick, X(5), Y(7), X(19) - X(5), Y(17) - Y(7));
        break;
    case TB_ELLIPSE:
        gx_stroke_ellipse(g, tint, thick, X(5), Y(6), X(19) - X(5), Y(18) - Y(6));
        break;
    case TB_ARROW:
        gx_stroke_line_f(g, pen, X(4), Y(20), X(20), Y(4));
        gx_stroke_line_f(g, pen, X(20), Y(4), X(12), Y(6));
        gx_stroke_line_f(g, pen, X(20), Y(4), X(18), Y(12));
        break;
    case TB_TEXT:    /* a "T" */
        gx_stroke_line_f(g, pen, X(5), Y(6), X(19), Y(6));
        gx_stroke_line_f(g, pen, X(12), Y(6), X(12), Y(19));
        break;
    case TB_MOSAIC: { /* a 3x3 checker of solid cells */
        int i, j;
        for (j = 0; j < 3; j++)
            for (i = 0; i < 3; i++)
                gx_fill_rect(g, tint, X(4 + i * 6), Y(5 + j * 6), 4 * u, 4 * u);
        break;
    }
    case TB_BLUR:    /* two softening ellipses */
        gx_stroke_ellipse(g, tint, thick, X(8), Y(4), X(16) - X(8), Y(20) - Y(4));
        gx_stroke_ellipse(g, tint, thick, X(3), Y(8), X(18) - X(3), Y(12) - Y(8));
        break;
    case TB_FILL:
        gx_fill_rect(g, tint, X(5), Y(7), X(19) - X(5), Y(17) - Y(7));
        break;
    case TB_UNDO:
    case TB_REDO: {
        REAL dir = (id == TB_REDO) ? -1.0f : 1.0f;
        GpPointF tri[3];
        gx_stroke_line_f(g, pen, cx - dir * 8 * u, cy + 5 * u, cx + dir * 8 * u, cy + 5 * u);
        if (GdipDrawArc(g, pen, cx - 8 * u, cy - 8 * u, 16 * u, 16 * u,
                        200.0f, 160.0f) != Ok) { /* arc is cosmetic */ }
        tri[0].X = cx - dir * 13 * u; tri[0].Y = cy + 5 * u;
        tri[1].X = cx - dir * 6 * u;  tri[1].Y = cy + 1 * u;
        tri[2].X = cx - dir * 6 * u;  tri[2].Y = cy + 9 * u;
        gx_fill_poly(g, tint, tri, 3);
        break;
    }
    case TB_FILLMODE: /* outline with the lower half filled */
        gx_fill_rect(g, tint, X(5), Y(12), X(19) - X(5), Y(18) - Y(12));
        gx_stroke_rect(g, tint, thick, X(5), Y(6), X(19) - X(5), Y(18) - Y(6));
        break;
    case TB_THICKER:
    case TB_THINNER: {
        REAL w2 = (id == TB_THICKER) ? thick * 3.0f : thick * 0.9f;
        GpPen *p2 = gx_pen(tint, w2, LineCapRound, LineJoinRound);
        if (p2) {
            gx_stroke_line_f(g, p2, X(4), Y(12), X(20), Y(12));
            GdipDeletePen(p2);
        }
        break;
    }
    case TB_COPY:
        gx_stroke_rect(g, tint, thick, X(4), Y(4), X(14) - X(4), Y(16) - Y(4));
        gx_stroke_rect(g, tint, thick, X(9), Y(8), X(20) - X(9), Y(20) - Y(8));
        break;
    case TB_SAVE:
        gx_stroke_rect(g, tint, thick, X(4), Y(5), X(20) - X(4), Y(19) - Y(5));
        gx_stroke_rect(g, tint, thick, X(8), Y(5), X(16) - X(8), Y(11) - Y(5));
        gx_fill_rect(g, tint, X(7), Y(14), X(17) - X(7), Y(19) - Y(14));
        break;
    case TB_DONE: {
        GpPointF tick[3];
        GpPen *p2 = gx_pen(0xFF32D74B, thick * 1.5f, LineCapRound, LineJoinRound);
        if (p2) {
            tick[0].X = X(5);  tick[0].Y = Y(13);
            tick[1].X = X(10); tick[1].Y = Y(18);
            tick[2].X = X(19); tick[2].Y = Y(6);
            if (GdipDrawLines(g, p2, tick, 3) != Ok) { /* cosmetic */ }
            GdipDeletePen(p2);
        }
        break;
    }
    case TB_CANCEL: {
        GpPen *p2 = gx_pen(0xFFFF453A, thick * 1.5f, LineCapRound, LineJoinRound);
        if (p2) {
            gx_stroke_line_f(g, p2, X(6), Y(6), X(18), Y(18));
            gx_stroke_line_f(g, p2, X(18), Y(6), X(6), Y(18));
            GdipDeletePen(p2);
        }
        break;
    }
    case -1:          /* dimmed placeholder (disabled button) */
        gx_stroke_ellipse(g, tint, thick, X(7), Y(7), X(17) - X(7), X(17) - X(7));
        break;
    default:
        gx_stroke_ellipse(g, tint, thick, X(7), Y(7), X(17) - X(7), X(17) - X(7));
        break;
    }
    GdipDeletePen(pen);
#undef X
#undef Y
}

/* ------------------------------------------------------------------ */
/* toolbar layout                                                      */
/* ------------------------------------------------------------------ */

typedef struct { int id; int w; int kind; } LayoutItem;
/* kind: 0 button, 1 swatch, 2 separator */

/* Grouped so the redaction tools sit together and mosaic leads them: it is
 * the one people reach for most, and burying it in the middle of the row is
 * what makes a toolbar feel like it is hiding things from you. */
static const int TOOL_ORDER[] = {
    TB_PEN, TB_ERASER, -1,
    TB_MOSAIC, TB_BLUR, TB_FILL, -1,
    TB_LINE, TB_RECT, TB_ELLIPSE, TB_ARROW, TB_TEXT
};

/* The hover bubble. These say what the button *does*, not just what it is
 * called - a user who has never seen the toolbar should be able to hover the
 * row once and know what every tool is for. */
static const wchar_t *tool_tip(int id)
{
    switch (id) {
    case TB_PEN:      return L"画笔 — 随手涂画圈重点 (P)";
    case TB_ERASER:   return L"橡皮 — 擦掉已画的标注 (E)";
    case TB_LINE:     return L"直线 — 拉一条指示线 (L)";
    case TB_RECT:     return L"矩形 — 框出一块区域 (R)";
    case TB_ELLIPSE:  return L"椭圆 — 圈出重点 (O)";
    case TB_ARROW:    return L"箭头 — 指向要看的位置 (A)";
    case TB_TEXT:     return L"文字 — 加一段说明文字 (T)";
    case TB_MOSAIC:   return L"马赛克 — 打码敏感信息，挡人眼也挡搜索 (M)";
    case TB_BLUR:     return L"模糊 — 柔化背景，弱化但不遮死 (B)";
    case TB_FILL:     return L"遮挡块 — 实心色块彻底盖住 (F)";
    case TB_UNDO:     return L"撤销 — 退回上一步 (Ctrl+Z)";
    case TB_REDO:     return L"重做 — 找回撤掉的 (Ctrl+Y)";
    case TB_FILLMODE: return L"填充/描边 — 切换图形是否实心";
    case TB_THICKER:  return L"加粗 — 马赛克则是加大格子 (Ctrl+滚轮)";
    case TB_THINNER:  return L"变细 — 马赛克则是减小格子 (Ctrl+滚轮)";
    case TB_COPY:     return L"复制 — 复制到剪贴板后关闭 (Ctrl+C)";
    case TB_SAVE:     return L"保存 — 存成 PNG 后关闭 (Ctrl+S)";
    case TB_DONE:     return L"完成 — 复制到剪贴板并关闭 (Enter)";
    case TB_CANCEL:   return L"取消 — 放弃这次截图 (Esc)";
    default:          return L"";
    }
}

static const wchar_t *swatch_tip(int index)
{
    static const wchar_t *NAMES[10] = {
        L"红色 (1)", L"橙色 (2)", L"黄色 (3)", L"绿色 (4)", L"青色 (5)",
        L"蓝色 (6)", L"靛蓝 (7)", L"粉色 (8)", L"黑色 (9)", L"白色 (0)"
    };
    if (index < 0 || index >= 10) return L"";
    return NAMES[index];
}

void layout_toolbar(ShotApp *a)
{
    LayoutItem items[64];
    int n = 0, i, total = 0, x, y;
    int btn = app_scale_px(30), sw = app_scale_px(24), sep = app_scale_px(11);
    int pad = app_scale_px(7), barH = app_scale_px(38);
    int nsw = PAL_N;
    int avail = a->vsw - app_scale_px(24);
    int selW = a->haveSel ? (a->sel.right - a->sel.left) : a->vsw;

    if (selW < avail) avail = selW;
    if (avail < btn * 10) avail = btn * 10;

    /* drop swatches first if the row cannot fit the selection */
    for (;;) {
        n = 0;
        for (i = 0; i < nsw; i++) { items[n].id = TB_SWATCH0 + i; items[n].w = sw; items[n].kind = 1; n++; }
        items[n].id = -1; items[n].w = sep; items[n].kind = 2; n++;
        for (i = 0; i < (int)(sizeof(TOOL_ORDER) / sizeof(TOOL_ORDER[0])); i++) {
            if (TOOL_ORDER[i] < 0) { items[n].id = -1; items[n].w = sep; items[n].kind = 2; n++; }
            else { items[n].id = TOOL_ORDER[i]; items[n].w = btn; items[n].kind = 0; n++; }
        }
        items[n].id = -1; items[n].w = sep; items[n].kind = 2; n++;
        items[n].id = TB_THINNER; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = TB_THICKER; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = TB_FILLMODE; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = -1; items[n].w = sep; items[n].kind = 2; n++;
        items[n].id = TB_UNDO; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = TB_REDO; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = -1; items[n].w = sep; items[n].kind = 2; n++;
        items[n].id = TB_COPY; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = TB_SAVE; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = TB_DONE; items[n].w = btn; items[n].kind = 0; n++;
        items[n].id = TB_CANCEL; items[n].w = btn; items[n].kind = 0; n++;

        total = pad * 2;
        for (i = 0; i < n; i++) total += items[i].w;
        if (total <= avail || nsw <= 3) break;
        nsw--;
    }

    if (a->ctb < n) {
        TbItem *nt = (TbItem *)xrealloc(a->tb, (size_t)n * sizeof(TbItem));
        if (!nt) return;
        a->tb = nt;
        a->ctb = n;
    }
    a->ntb = n;

    /* centre on the selection, then clamp inside the virtual screen */
    x = a->vsx + (selW - total) / 2;
    if (x + total > a->vsx + a->vsw) x = a->vsx + a->vsw - total;
    if (x < a->vsx) x = a->vsx;
    y = a->sel.top - barH - app_scale_px(6);
    if (y < a->vsy + app_scale_px(4)) y = a->sel.bottom + app_scale_px(6);
    if (y + barH > a->vsy + a->vsh - app_scale_px(4))
        y = a->vsy + a->vsh - barH - app_scale_px(4);

    SetRect(&a->tbRect, x, y, x + total, y + barH);

    x += pad;
    for (i = 0; i < n; i++) {
        TbItem *t = &a->tb[i];
        t->id = items[i].id;
        t->colorIndex = (items[i].kind == 1) ? (items[i].id - TB_SWATCH0) : -1;
        if (t->colorIndex >= 0)      wcsncpy(t->tip, swatch_tip(t->colorIndex), 63);
        else if (t->id >= 0)         wcsncpy(t->tip, tool_tip(t->id), 63);
        else                          t->tip[0] = 0;
        t->tip[63] = 0;
        SetRect(&t->hit, x, y + (barH - items[i].w) / 2,
                    x + items[i].w, y + (barH - items[i].w) / 2 + items[i].w);
        x += items[i].w;
    }

    /* In verbose mode the whole row is dumped, ids and tips included. The
     * outside probes use it to aim at a specific button without having to
     * re-derive the layout, which is exactly the thing that changes. */
    if (app_verbose()) {
        app_logw(L"toolbar %ld,%ld %ldx%d with %d items",
                 (long)a->tbRect.left, (long)a->tbRect.top,
                 (long)(a->tbRect.right - a->tbRect.left),
                 (long)(a->tbRect.bottom - a->tbRect.top), n);
        for (i = 0; i < n; i++) {
            const TbItem *t = &a->tb[i];
            app_logw(L"  tb[%2d] id=%-5d color=%-2d %ld,%ld %ldx%d  %ls",
                     i, t->id, t->colorIndex,
                     (long)t->hit.left, (long)t->hit.top,
                     (long)(t->hit.right - t->hit.left),
                     (long)(t->hit.bottom - t->hit.top), t->tip);
        }
    }
}

int hit_toolbar(const ShotApp *a, int sx, int sy)
{
    int i;
    POINT p;
    p.x = sx; p.y = sy;
    if (!PtInRect(&a->tbRect, p)) return -1;
    for (i = 0; i < a->ntb; i++) {
        if (a->tb[i].id < 0) continue;
        if (PtInRect(&a->tb[i].hit, p)) return i;
    }
    return -1;
}

static int tool_selected(const ShotApp *a, int id)
{
    static const struct { int id; ToolKind k; } MAP[] = {
        { TB_PEN, T_PEN }, { TB_ERASER, T_ERASER }, { TB_LINE, T_LINE },
        { TB_RECT, T_RECT }, { TB_ELLIPSE, T_ELLIPSE }, { TB_ARROW, T_ARROW },
        { TB_TEXT, T_TEXT }, { TB_MOSAIC, T_MOSAIC }, { TB_BLUR, T_BLUR },
        { TB_FILL, T_FILL }
    };
    int i;
    for (i = 0; i < (int)(sizeof(MAP) / sizeof(MAP[0])); i++)
        if (MAP[i].id == id) return a->tool == MAP[i].k;
    return 0;
}

static int swatch_selected(const ShotApp *a, int idx)
{
    if (idx < 0 || idx >= PAL_N) return 0;
    return (a->color & 0x00FFFFFF) == (PALETTE[idx] & 0x00FFFFFF);
}

void draw_toolbar(ShotApp *a, GpGraphics *g)
{
    int i;
    REAL h = (REAL)(a->tbRect.bottom - a->tbRect.top);
    REAL w = (REAL)(a->tbRect.right - a->tbRect.left);
    REAL rr = h * 0.22f;
    REAL S = (REAL)app_scale_px(1);
    ARGB bg = 0xF01C1C1E, line = 0x40FFFFFF;

    /* rounded backdrop: centre rect + four corner discs */
    gx_fill_rect(g, bg, (REAL)a->tbRect.left + rr, (REAL)a->tbRect.top, w - rr * 2.0f, h);
    gx_fill_rect(g, bg, (REAL)a->tbRect.left, (REAL)a->tbRect.top + rr, rr, h - rr * 2.0f);
    gx_fill_rect(g, bg, (REAL)a->tbRect.right - rr, (REAL)a->tbRect.top + rr, rr, h - rr * 2.0f);
    gx_fill_ellipse(g, bg, (REAL)a->tbRect.left, (REAL)a->tbRect.top, rr * 2, rr * 2);
    gx_fill_ellipse(g, bg, (REAL)a->tbRect.right - rr * 2, (REAL)a->tbRect.top, rr * 2, rr * 2);
    gx_fill_ellipse(g, bg, (REAL)a->tbRect.left, (REAL)a->tbRect.bottom - rr * 2, rr * 2, rr * 2);
    gx_fill_ellipse(g, bg, (REAL)a->tbRect.right - rr * 2, (REAL)a->tbRect.bottom - rr * 2, rr * 2, rr * 2);
    gx_stroke_rect(g, line, S, (REAL)a->tbRect.left, (REAL)a->tbRect.top, w, h);

    for (i = 0; i < a->ntb; i++) {
        TbItem *t = &a->tb[i];
        REAL bw = (REAL)(t->hit.right - t->hit.left);
        REAL bh = (REAL)(t->hit.bottom - t->hit.top);
        int hot = (a->tbHot == i);
        int down = (a->tbDown == i);

        if (t->id < 0) {   /* separator */
            gx_fill_rect(g, 0x66FFFFFF, (REAL)(t->hit.left + bw / 2) - 0.5f,
                         (REAL)(t->hit.top + bh * 0.22f), 1.0f, bh * 0.56f);
            continue;
        }
        if (hot)
            gx_fill_rect(g, down ? 0x40FFFFFF : 0x28FFFFFF,
                         (REAL)t->hit.left, (REAL)t->hit.top, bw, bh);

        if (t->colorIndex >= 0) {
            ARGB c = PALETTE[t->colorIndex];
            int inset = app_scale_px(4);
            RECT ir = t->hit;
            InflateRect(&ir, -inset, -inset);
            if (swatch_selected(a, t->colorIndex)) {
                gx_fill_ellipse(g, 0xFFFFFFFF, (REAL)ir.left - 2, (REAL)ir.top - 2,
                                (REAL)(ir.right - ir.left + 4),
                                (REAL)(ir.bottom - ir.top + 4));
                InflateRect(&ir, 1, 1);
            }
            gx_fill_ellipse(g, c, (REAL)ir.left, (REAL)ir.top,
                            (REAL)(ir.right - ir.left), (REAL)(ir.bottom - ir.top));
            gx_stroke_ellipse(g, line, S, (REAL)ir.left, (REAL)ir.top,
                              (REAL)(ir.right - ir.left), (REAL)(ir.bottom - ir.top));
            continue;
        }

        if (tool_selected(a, t->id))
            gx_fill_rect(g, 0xFF0A84FF, (REAL)t->hit.left, (REAL)t->hit.top, bw, bh);

        if (t->id == TB_UNDO && a->cursor == 0) {
            draw_glyph(g, t->id, t->hit, 0xFFFFFFFF, S * 1.6f);
            draw_glyph(g, -1, t->hit, 0x50FFFFFF, S * 1.6f);
        } else if (t->id == TB_REDO && a->cursor >= a->nstrokes) {
            draw_glyph(g, t->id, t->hit, 0xFFFFFFFF, S * 1.6f);
            draw_glyph(g, -1, t->hit, 0x50FFFFFF, S * 1.6f);
        } else {
            draw_glyph(g, t->id, t->hit, 0xFFFFFFFF, S * 1.6f);
        }
    }

    /* current stroke width / mosaic cell readout, tucked under the bar */
    {
        wchar_t buf[48];
        int ty = (int)(a->tbRect.bottom + app_scale_px(2));
        if (a->tool == T_MOSAIC)
            _snwprintf_s(buf, 48, _TRUNCATE, L"格 %d px", a->mosaicBlock);
        else if (a->tool == T_TEXT)
            _snwprintf_s(buf, 48, _TRUNCATE, L"%d px 字", a->fontSize);
        else
            _snwprintf_s(buf, 48, _TRUNCATE, L"%d px", a->width);
        draw_hud_text(a, g, buf, a->tbRect.left + app_scale_px(2), ty,
                      app_scale_px(140), app_scale_px(15), 0xB0FFFFFF, app_scale_px(11));
    }
}

/* ------------------------------------------------------------------ */
/* hover bubble                                                        */
/* ------------------------------------------------------------------ */

#define TIP_DELAY_MS 450

/* A rounded dark panel, same construction as the toolbar backdrop. */
static void fill_round_rect(GpGraphics *g, ARGB fill, ARGB line, REAL S,
                            REAL l, REAL t, REAL w, REAL h)
{
    REAL rr = h * 0.30f;
    if (w <= rr * 2.0f) rr = w / 2.0f;
    gx_fill_rect(g, fill, l + rr, t, w - rr * 2.0f, h);
    gx_fill_rect(g, fill, l, t + rr, rr, h - rr * 2.0f);
    gx_fill_rect(g, fill, l + w - rr, t + rr, rr, h - rr * 2.0f);
    gx_fill_ellipse(g, fill, l, t, rr * 2, rr * 2);
    gx_fill_ellipse(g, fill, l + w - rr * 2, t, rr * 2, rr * 2);
    gx_fill_ellipse(g, fill, l, t + h - rr * 2, rr * 2, rr * 2);
    gx_fill_ellipse(g, fill, l + w - rr * 2, t + h - rr * 2, rr * 2, rr * 2);
    if (line) gx_stroke_rect(g, line, S, l, t, w, h);
}

/* Draws the explanation for `idx`. Returns the index drawn for, or -1 when
 * there is nothing to say about that item. */
static int draw_tip_bubble(ShotApp *a, GpGraphics *g, int idx)
{
    const wchar_t *text;
    int px, tw, th, bw, bh, bx, by, S;
    RECT item;
    REAL S_;

    if (idx < 0 || idx >= a->ntb) return -1;
    text = a->tb[idx].tip;
    if (!text || !*text) return -1;
    item = a->tb[idx].hit;

    S = app_scale_px(1);
    S_ = (REAL)S;
    px = app_scale_px(13);
    th = app_scale_px(26);
    tw = shot_text_width(a, text, px) + app_scale_px(18);
    bw = tw + app_scale_px(16);
    bh = th + app_scale_px(6);

    /* Prefer below the bar, flip above when that would leave the screen, and
     * keep it inside the virtual screen in both directions. */
    by = a->tbRect.bottom + app_scale_px(8);
    if (by + bh > a->vsy + a->vsh - app_scale_px(4))
        by = a->tbRect.top - bh - app_scale_px(8);
    if (by < a->vsy + app_scale_px(4))
        by = a->tbRect.bottom + app_scale_px(8);

    bx = (item.left + item.right) / 2 - bw / 2;
    if (bx < a->vsx + app_scale_px(4)) bx = a->vsx + app_scale_px(4);
    if (bx + bw > a->vsx + a->vsw - app_scale_px(4))
        bx = a->vsx + a->vsw - app_scale_px(4) - bw;

    fill_round_rect(g, 0xF2262628, 0x30FFFFFF, S_,
                    (REAL)bx, (REAL)by, (REAL)bw, (REAL)bh);
    draw_hud_text(a, g, text, bx + app_scale_px(8), by + app_scale_px(3),
                  tw, th, 0xFFF2F2F2, px);
    return idx;
}

/* The hover bubble, with the delay and stickiness that make it usable.
 *
 * TbItem.tip has always been filled in by layout_toolbar; what was missing was
 * anything ever reading it, so every one of those strings was dead weight.
 * This is the part that finally shows them.
 *
 * The bubble waits TIP_DELAY_MS before appearing, so sweeping the cursor across
 * the bar does not strobe a dozen tooltips, and it stays up while the cursor
 * moves *within* one button - otherwise a bubble that appeared under the cursor
 * would immediately be "not hovering" and flicker.
 *
 * Once it is eligible it is drawn on *every* pass. paint() rebuilds the whole
 * frame from the frozen capture each time, so nothing on screen survives from
 * one repaint to the next; drawing the bubble only on the repaint that first
 * promoted it made it vanish the moment anything else caused a repaint. */
int draw_toolbar_tip(ShotApp *a, GpGraphics *g)
{
    int idx = a->tbHot;

    if (idx < 0 || a->tbDown == idx) { a->tipShown = -1; return -1; }
    if (idx != a->tipShown &&
        (DWORD)(GetTickCount() - a->tipSince) < TIP_DELAY_MS)
        return -1;
    a->tipShown = idx;
    return draw_tip_bubble(a, g, idx);
}

/* ------------------------------------------------------------------ */
/* selection guides                                                    */
/* ------------------------------------------------------------------ */

/* Two lines through the cursor, so the eye can find the exact pixel while a
 * region is being dragged.
 *
 * There was a magnifier here as well: an 8x8 grid of magnified pixels, a marker
 * on the centre one, and its colour and coordinates printed underneath. That
 * was a colour picker built into the capture overlay - the job the eyedropper
 * exists to do, done a second time, on screen, every time anybody dragged a
 * region. Drawing a region needs lines. Taking a colour from the screen is
 * another program's whole purpose, and having it here too is what made the two
 * feel like one button. */
void draw_selection_guides(ShotApp *a, GpGraphics *g)
{
    REAL S = (REAL)app_scale_px(1);
    GpPen *pen;
    if (!a->screenBits || !a->selecting) return;
    pen = gx_pen(0x30FFFFFF, S, LineCapFlat, LineJoinMiter);
    if (!pen) return;
    GdipDrawLine(g, pen, (REAL)a->vsx, (REAL)a->magY,
                 (REAL)(a->vsx + a->vsw), (REAL)a->magY);
    GdipDrawLine(g, pen, (REAL)a->magX, (REAL)a->vsy,
                 (REAL)a->magX, (REAL)(a->vsy + a->vsh));

    /* A snap has to show itself. Without a guide the corner jumps a few pixels
     * for no visible reason, which reads as the tool being wrong rather than
     * helpful - so the edge it caught is drawn, full width, in the accent the
     * rest of the chrome uses. And only while a corner is actually on one. */
    if (a->snapX >= 0 || a->snapY >= 0) {
        GdipSetPenColor(pen, 0xFFFF3B30);
        if (a->snapX >= 0)
            GdipDrawLine(g, pen, (REAL)a->snapX, (REAL)a->vsy,
                         (REAL)a->snapX, (REAL)(a->vsy + a->vsh));
        if (a->snapY >= 0)
            GdipDrawLine(g, pen, (REAL)a->vsx, (REAL)a->snapY,
                         (REAL)(a->vsx + a->vsw), (REAL)a->snapY);
    }
    GdipDeletePen(pen);
}