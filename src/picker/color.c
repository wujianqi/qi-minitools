/* color.c - colour space conversions and formatting. */
#include "color.h"

static BYTE clamp255(double v)
{
    long n = lround(v);
    if (n < 0) return 0;
    if (n > 255) return 255;
    return (BYTE)n;
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static double max3(double a, double b, double c)
{
    double m = a;
    if (b > m) m = b;
    if (c > m) m = c;
    return m;
}

static double min3(double a, double b, double c)
{
    double m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    return m;
}

double srgb_to_linear(double c)
{
    c = clamp01(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

double linear_to_srgb(double c)
{
    c = clamp01(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

int rgb_equal(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

Rgb rgb_mix(Rgb a, Rgb b, double t)
{
    Rgb o;
    t = clamp01(t);
    o.r = clamp255(a.r + (b.r - a.r) * t);
    o.g = clamp255(a.g + (b.g - a.g) * t);
    o.b = clamp255(a.b + (b.b - a.b) * t);
    return o;
}

Rgb rgb_scale(Rgb c, double f)
{
    Rgb o;
    o.r = clamp255(c.r * f);
    o.g = clamp255(c.g * f);
    o.b = clamp255(c.b * f);
    return o;
}

/* ------------------------------------------------------------------ */
/* HSL / HSV                                                           */
/* ------------------------------------------------------------------ */

static void rgb_to_hue(Rgb c, double maxv, double delta, double *h)
{
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    if (delta <= 0) { *h = 0; return; }
    if (maxv == r)      *h = 60.0 * fmod((g - b) / delta, 6.0);
    else if (maxv == g) *h = 60.0 * (((b - r) / delta) + 2.0);
    else                *h = 60.0 * (((r - g) / delta) + 4.0);
    if (*h < 0) *h += 360.0;
}

void rgb_to_hsl(Rgb c, double *h, double *s, double *l)
{
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double mx = max3(r, g, b), mn = min3(r, g, b);
    double d = mx - mn;
    double hh = 0, ss = 0, ll;
    rgb_to_hue(c, mx, d, &hh);
    ll = (mx + mn) / 2.0;
    if (d > 0) {
        ss = d / (1.0 - fabs(2.0 * ll - 1.0));
        ss = clamp01(ss);
    }
    /* Every out-parameter is optional: the wheel only ever wants one of the
     * three, and a NULL there used to be a hard access violation. */
    if (h) *h = hh;
    if (s) *s = ss;
    if (l) *l = ll;
}

static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6.0) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2.0) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6;
    return p;
}

void hsl_to_rgb(double h, double s, double l, Rgb *out)
{
    double r, g, b;
    h = fmod(h, 360.0); if (h < 0) h += 360.0;
    s = clamp01(s); l = clamp01(l);
    if (s <= 0) {
        r = g = b = l;
    } else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        double hh = h / 360.0;
        r = hue2rgb(p, q, hh + 1.0 / 3.0);
        g = hue2rgb(p, q, hh);
        b = hue2rgb(p, q, hh - 1.0 / 3.0);
    }
    out->r = clamp255(r * 255.0);
    out->g = clamp255(g * 255.0);
    out->b = clamp255(b * 255.0);
}

void rgb_to_hsv(Rgb c, double *h, double *s, double *v)
{
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double mx = max3(r, g, b), mn = min3(r, g, b);
    double d = mx - mn;
    double hh = 0, ss, vv = mx;
    rgb_to_hue(c, mx, d, &hh);
    ss = (mx <= 0) ? 0 : clamp01(d / mx);
    if (h) *h = hh;
    if (s) *s = ss;
    if (v) *v = vv;
}

void hsv_to_rgb(double h, double s, double v, Rgb *out)
{
    double r, g, b;
    h = fmod(h, 360.0); if (h < 0) h += 360.0;
    s = clamp01(s); v = clamp01(v);
    {
        int i = (int)(h / 60.0) % 6;
        double f = h / 60.0 - (int)(h / 60.0);
        double p = v * (1 - s);
        double q = v * (1 - f * s);
        double t = v * (1 - (1 - f) * s);
        switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
    }
    out->r = clamp255(r * 255.0);
    out->g = clamp255(g * 255.0);
    out->b = clamp255(b * 255.0);
}

/* ------------------------------------------------------------------ */
/* CMYK                                                                */
/* ------------------------------------------------------------------ */

void rgb_to_cmyk(Rgb c, double *c_, double *m, double *y, double *k)
{
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double kk = 1.0 - max3(r, g, b);
    if (kk >= 1.0 - 1e-9) { *c_ = *m = *y = 0.0; *k = 1.0; return; }
    *c_ = (1.0 - r - kk) / (1.0 - kk);
    *m = (1.0 - g - kk) / (1.0 - kk);
    *y = (1.0 - b - kk) / (1.0 - kk);
    *k = kk;
    *c_ = clamp01(*c_); *m = clamp01(*m); *y = clamp01(*y);
}

void cmyk_to_rgb(double c_, double m, double y, double k, Rgb *out)
{
    c_ = clamp01(c_); m = clamp01(m); y = clamp01(y); k = clamp01(k);
    out->r = clamp255(255.0 * (1.0 - c_) * (1.0 - k));
    out->g = clamp255(255.0 * (1.0 - m) * (1.0 - k));
    out->b = clamp255(255.0 * (1.0 - y) * (1.0 - k));
}

/* ------------------------------------------------------------------ */
/* XYZ / LAB (D65)                                                     */
/* ------------------------------------------------------------------ */

void rgb_to_xyz(Rgb c, double *X, double *Y, double *Z)
{
    double r = srgb_to_linear(c.r / 255.0);
    double g = srgb_to_linear(c.g / 255.0);
    double b = srgb_to_linear(c.b / 255.0);
    *X = (r * 0.4124564 + g * 0.3575761 + b * 0.1804375) * 100.0;
    *Y = (r * 0.2126729 + g * 0.7151522 + b * 0.0721750) * 100.0;
    *Z = (r * 0.0193339 + g * 0.1191920 + b * 0.9503041) * 100.0;
}

static double lab_f(double t)
{
    const double d = 6.0 / 29.0;
    return t > d * d * d ? cbrt(t) : (t / (3.0 * d * d) + 4.0 / 29.0);
}

void rgb_to_lab(Rgb c, double *L, double *A, double *B)
{
    /* D65 reference white */
    const double Xn = 95.047, Yn = 100.000, Zn = 108.883;
    double X, Y, Z, fx, fy, fz;
    rgb_to_xyz(c, &X, &Y, &Z);
    fx = lab_f(X / Xn);
    fy = lab_f(Y / Yn);
    fz = lab_f(Z / Zn);
    *L = 116.0 * fy - 16.0;
    *A = 500.0 * (fx - fy);
    *B = 200.0 * (fy - fz);
}

double rgb_luminance(Rgb c)
{
    return 0.2126 * srgb_to_linear(c.r / 255.0) +
           0.7152 * srgb_to_linear(c.g / 255.0) +
           0.0722 * srgb_to_linear(c.b / 255.0);
}

double rgb_contrast(Rgb a, Rgb b)
{
    double la = rgb_luminance(a), lb = rgb_luminance(b);
    double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

/* ------------------------------------------------------------------ */
/* formatting                                                          */
/* ------------------------------------------------------------------ */

const wchar_t *color_format_label(ColorFormat f)
{
    static const wchar_t *L[CF_COUNT] = {
        L"HEX", L"HEX+α", L"RGB", L"RGBA", L"HSL", L"HSV",
        L"CMYK", L"LAB", L"XYZ", L"CSS", L"DEC"
    };
    return (f >= 0 && f < CF_COUNT) ? L[f] : L[0];
}

const wchar_t *color_format_hint(ColorFormat f)
{
    static const wchar_t *H[CF_COUNT] = {
        L"网页 / 设计稿通用", L"带透明通道",
        L"函数式 rgb()",      L"带 alpha",
        L"色相 饱和度 亮度",  L"色相 饱和度 明度",
        L"印刷四色",          L"CIELAB 感知均匀",
        L"CIE 1931 D65",      L"现代 CSS 写法",
        L"整数值"
    };
    return (f >= 0 && f < CF_COUNT) ? H[f] : H[0];
}

wchar_t *color_format_value(ColorFormat f, Rgb c)
{
    switch (f) {
    case CF_HEX:
        return wcs_format(L"#%02X%02X%02X", c.r, c.g, c.b);
    case CF_HEXA:
        return wcs_format(L"#%02X%02X%02XFF", c.r, c.g, c.b);
    case CF_RGB:
        return wcs_format(L"rgb(%u, %u, %u)", c.r, c.g, c.b);
    case CF_RGBA:
        return wcs_format(L"rgba(%u, %u, %u, 1)", c.r, c.g, c.b);
    case CF_HSL: {
        double h, s, l;
        rgb_to_hsl(c, &h, &s, &l);
        return wcs_format(L"hsl(%u, %u%%, %u%%)",
                          (unsigned)lround(h), (unsigned)lround(s * 100),
                          (unsigned)lround(l * 100));
    }
    case CF_HSV: {
        double h, s, v;
        rgb_to_hsv(c, &h, &s, &v);
        return wcs_format(L"hsv(%u, %u%%, %u%%)",
                          (unsigned)lround(h), (unsigned)lround(s * 100),
                          (unsigned)lround(v * 100));
    }
    case CF_CMYK: {
        double cc, m, y, k;
        rgb_to_cmyk(c, &cc, &m, &y, &k);
        return wcs_format(L"cmyk(%u%%, %u%%, %u%%, %u%%)",
                          (unsigned)lround(cc * 100), (unsigned)lround(m * 100),
                          (unsigned)lround(y * 100), (unsigned)lround(k * 100));
    }
    case CF_LAB: {
        double L, A, B;
        rgb_to_lab(c, &L, &A, &B);
        /* a and b are signed (-128..127): %ld, or a negative value is read as
         * an unsigned int and comes out as 4294967210. */
        return wcs_format(L"lab(%ld, %ld, %ld)",
                          lround(L), lround(A), lround(B));
    }
    case CF_XYZ: {
        double X, Y, Z;
        rgb_to_xyz(c, &X, &Y, &Z);
        /* the XYZ matrix has no negative coefficients, so these cannot go
         * negative - but they are the same signed doubles as LAB, so they are
         * formatted the same way rather than trusted to stay non-negative */
        return wcs_format(L"xyz(%ld, %ld, %ld)",
                          lround(X), lround(Y), lround(Z));
    }
    case CF_CSS:
        return wcs_format(L"color(srgb %u%% %u%% %u%%)",
                          (unsigned)lround(c.r * 100.0 / 255.0),
                          (unsigned)lround(c.g * 100.0 / 255.0),
                          (unsigned)lround(c.b * 100.0 / 255.0));
    case CF_DEC:
        return wcs_format(L"%u", (unsigned)((c.r << 16) | (c.g << 8) | c.b));
    default:
        return xwcsdup(L"");
    }
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int color_parse(const wchar_t *s, Rgb *out)
{
    const wchar_t *p;
    int digits[8], n = 0;

    if (!s || !out) return 0;
    while (*s == L' ' || *s == L'\t') s++;
    p = s;
    if (*p == L'#') p++;
    while (hexval((int)*p) >= 0 && n < 8) { digits[n++] = hexval((int)*p); p++; }
    if (n == 6) {
        out->r = (BYTE)(digits[0] * 16 + digits[1]);
        out->g = (BYTE)(digits[2] * 16 + digits[3]);
        out->b = (BYTE)(digits[4] * 16 + digits[5]);
        return 1;
    }
    if (n == 3) {
        out->r = (BYTE)(digits[0] * 17);
        out->g = (BYTE)(digits[1] * 17);
        out->b = (BYTE)(digits[2] * 17);
        return 1;
    }
    /* decimal integer form */
    if (wcslen(s) >= 1 && iswdigit((int)s[0])) {
        unsigned long v = wcstoul(s, NULL, 10);
        out->r = (BYTE)((v >> 16) & 0xFF);
        out->g = (BYTE)((v >> 8) & 0xFF);
        out->b = (BYTE)(v & 0xFF);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* nearest CSS named colour                                            */
/* ------------------------------------------------------------------ */

typedef struct { const wchar_t *name; BYTE r, g, b; } NamedColor;

static const NamedColor NAMED[] = {
    { L"黑",        0,   0,   0 }, { L"白",      255, 255, 255 },
    { L"红",      255,   0,   0 }, { L"绿",        0, 128,   0 },
    { L"蓝",        0,   0, 255 }, { L"黄",      255, 255,   0 },
    { L"青",        0, 255, 255 }, { L"品红",    255,   0, 255 },
    { L"橙",      255, 165,   0 }, { L"紫",      128,   0, 128 },
    { L"粉红",    255, 192, 203 }, { L"棕",      165,  42,  42 },
    { L"藏青",      0,   0, 128 }, { L"天蓝",    135, 206, 235 },
    { L"金",      255, 215,   0 }, { L"银",      192, 192, 192 },
    { L"米色",    245, 245, 220 }, { L"青柠",    205, 220,  57 },
    { L"深青",      0, 139, 139 }, { L"深蓝",      0,   0, 139 },
    { L"深绿",      0, 100,   0 }, { L"深红",    139,   0,   0 },
    { L"深紫",    101,   0, 123 }, { L"橄榄",    128, 128,   0 },
    { L"珊瑚",    255, 127,  80 }, { L"鲑鱼",    250, 128, 114 },
    { L"番茄",    255,  99,  71 }, { L"金菊",    255, 185,  15 },
    { L"芥末",    219, 219,  16 }, { L"孔雀蓝",   30,   0, 255 }
};

Rgb rgb_nearest_named(Rgb c, const wchar_t **nameOut)
{
    int i, best = 0;
    long bestD = 0x7FFFFFFF;
    long dr, dg, db, d;
    for (i = 0; i < (int)(sizeof(NAMED) / sizeof(NAMED[0])); i++) {
        dr = (long)c.r - NAMED[i].r;
        dg = (long)c.g - NAMED[i].g;
        db = (long)c.b - NAMED[i].b;
        /* weighted to approximate perceived difference */
        d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (d < bestD) { bestD = d; best = i; }
    }
    if (nameOut) *nameOut = NAMED[best].name;
    { Rgb o; o.r = NAMED[best].r; o.g = NAMED[best].g; o.b = NAMED[best].b; return o; }
}
