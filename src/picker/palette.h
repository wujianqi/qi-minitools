/* palette.h - built-in colour palettes plus the user's saved favourites. */
#ifndef PICKER_PALETTE_H
#define PICKER_PALETTE_H

#include "color.h"

#define SWATCH_NAME_MAX 40

typedef struct {
    Rgb      c;
    wchar_t  name[SWATCH_NAME_MAX];
} Swatch;

typedef struct {
    const wchar_t *id;
    const wchar_t *label;
    const Swatch  *items;
    int            count;
} Palette;

typedef enum {
    PAL_FAVORITE = 0,   /* user-saved, persisted; may be empty       */
    /* Theme palettes, in the order of the boards they come from.        */
    PAL_THEME_SOFT,     /* 柔和、明亮、温柔                            */
    PAL_THEME_LIVELY,   /* 活泼、快乐、有趣                            */
    PAL_THEME_NATURAL,  /* 希望、自然、健康                            */
    PAL_THEME_CLASSIC,  /* 传统、神秘、优雅                            */
    PAL_THEME_ELEGANT,  /* 回味、优雅、女性化                          */
    PAL_THEME_CLEAN,    /* 简单、洁净、进步                            */
    PAL_THEME_RICH,     /* 轻快、华丽、动态                            */
    PAL_THEME_FEMININE, /* 华丽、花哨、女性化                          */
    PAL_THEME_RATIONAL, /* 简单、进步、时尚                            */
    PAL_THEME_COUNT,    /* not a palette: span of the nine above       */
    PAL_COMMON,         /* everyday UI + brand colours                */
    PAL_WEB,            /* CSS named colours                          */
    PAL_MATERIAL,       /* Material Design 2014                       */
    PAL_TAILWIND,       /* a practical Tailwind subset                */
    PAL_GRAY,           /* grayscale ramp                             */
    PAL_SKIN,           /* skin tones                                 */
    PAL_COUNT
} PaletteId;

#define PAL_THEME_FIRST PAL_THEME_SOFT
#define PAL_THEME_LAST  PAL_THEME_RATIONAL

const Palette *palette_get(PaletteId id);
int            palette_count(void);
PaletteId      palette_next(PaletteId id);
/* The one-line colour rule that goes with a theme palette, or NULL for the
 * non-theme palettes that have no such thing. */
const wchar_t *palette_note(PaletteId id);
int            palette_is_theme(PaletteId id);

/* Favourites ------------------------------------------------------------ */
int  fav_count(void);
const Swatch *fav_at(int i);
int  fav_add(Rgb c);                 /* 1 if added, 0 if already present */
int  fav_remove_at(int i);
void fav_load(void);
void fav_save(void);
void fav_free(void);

#endif /* PICKER_PALETTE_H */
