/* color.h - RGB colour type plus conversions to every format the tool shows. */
#ifndef PICKER_COLOR_H
#define PICKER_COLOR_H

#include "../common/winutil.h"

typedef struct { BYTE r, g, b; } Rgb;

/* sRGB 0..255 -> linear 0..1 and back, per IEC 61966-2-1. */
double srgb_to_linear(double c);
double linear_to_srgb(double c);

void rgb_to_hsl(Rgb c, double *h, double *s, double *l);   /* h 0..360, s/l 0..1 */
void hsl_to_rgb(double h, double s, double l, Rgb *out);
void rgb_to_hsv(Rgb c, double *h, double *s, double *v);
void hsv_to_rgb(double h, double s, double v, Rgb *out);
void rgb_to_cmyk(Rgb c, double *c_, double *m, double *y, double *k); /* 0..1 */
void cmyk_to_rgb(double c_, double m, double y, double k, Rgb *out);
void rgb_to_xyz(Rgb c, double *X, double *Y, double *Z);   /* D65, 0..100 */
void rgb_to_lab(Rgb c, double *L, double *A, double *B);   /* D65 */
int  rgb_equal(Rgb a, Rgb b);
Rgb rgb_mix(Rgb a, Rgb b, double t);
Rgb rgb_scale(Rgb c, double f);                            /* brightness scale */
Rgb rgb_nearest_named(Rgb c, const wchar_t **nameOut);

/* Relative luminance and WCAG contrast, used for readable swatch labels. */
double rgb_luminance(Rgb c);
double rgb_contrast(Rgb a, Rgb b);

/* Format identifiers, in display order. */
typedef enum {
    CF_HEX = 0, CF_HEXA, CF_RGB, CF_RGBA, CF_HSL, CF_HSV,
    CF_CMYK, CF_LAB, CF_XYZ, CF_CSS, CF_DEC, CF_COUNT
} ColorFormat;

const wchar_t *color_format_label(ColorFormat f);
const wchar_t *color_format_hint(ColorFormat f);
/* Returns a freshly allocated wide string; caller xfree()s. */
wchar_t      *color_format_value(ColorFormat f, Rgb c);

/* Parse "#rgb", "#rrggbb", "rgb(r,g,b)" and bare decimal. Returns 1 on hit. */
int           color_parse(const wchar_t *s, Rgb *out);

#endif /* PICKER_COLOR_H */
