/* presets.h - built-in development environment presets with auto-detection. */
#ifndef PRESETS_H
#define PRESETS_H

#include "envreg.h"

typedef struct {
    const wchar_t *name;        /* display label                       */
    const wchar_t *var;         /* primary variable this preset sets   */
    const wchar_t *subdir;      /* path appended under var, or NULL    */
    const wchar_t *pathEntry;   /* directory added to PATH, or NULL    */
    const wchar_t *extraVar;    /* secondary variable, or NULL         */
    const wchar_t *extraValue;  /* relative to the detected root, or NULL */
    /* one-level-deep globs probed under each scan root */
    const wchar_t *patterns[6];
} Preset;

int         preset_count(void);
const Preset *preset_at(int i);
/* Finds an installed copy; returns an xmalloc'd absolute path or NULL. */
wchar_t    *preset_detect(const Preset *p);

/* Comma-separated scan roots, editable by the user, persisted to %APPDATA%. */
void  preset_load_roots(void);
void  preset_save_roots(void);
int   preset_roots(wchar_t ***out);       /* caller frees with preset_free_roots */
void  preset_free_roots(wchar_t **roots, int n);
int   preset_set_roots(wchar_t **roots, int n);

#endif /* PRESETS_H */
