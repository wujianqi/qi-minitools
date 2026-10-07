/* envreg.h - reading, writing and merging Windows environment variables. */
#ifndef ENVREG_H
#define ENVREG_H

#include "../common/winutil.h"

typedef enum { SCOPE_USER = 0, SCOPE_SYSTEM = 1 } Scope;

#define ENV_NAME_MAX  512
#define ENV_VALUE_MAX 32768

typedef struct {
    wchar_t name[ENV_NAME_MAX];
    /* Both scopes are kept, because a variable that exists in each of them
     * is two independent registry values and editing one of them must not
     * silently show - or overwrite - the other. `value` is a borrowed alias
     * for whichever copy is currently on screen; it is never freed twice. */
    wchar_t *value;        /* NULL when the variable does not exist      */
    wchar_t *userValue;    /* NULL when absent from the user scope       */
    wchar_t *sysValue;     /* NULL when absent from the machine scope    */
    DWORD   type;          /* REG_SZ or REG_EXPAND_SZ                    */
    int     inUser;
    int     inSystem;
} EnvVar;

typedef struct {
    EnvVar *items;
    int     count, cap;
} EnvList;

void        env_free(EnvList *l);
/* Points every read and write at throwaway keys instead of the real
 * environment, so the self-test can exercise the real registry code paths
 * without touching a single actual variable. NULL restores normal behaviour.
 * SCOPE_SYSTEM maps onto HKEY_CURRENT_USER while redirected, so the test needs
 * no elevation. */
void        env_set_test_root(const wchar_t *userKey, const wchar_t *systemKey);
int         env_test_root_active(void);
/* Deletes the scratch tree, lifts the redirection, and reports whether the
 * tree is confirmed gone. Safe to call when nothing is redirected. */
int         env_test_cleanup(void);
/* Loads both scopes. `filter` may be NULL to load everything. */
int         env_load(EnvList *out, const wchar_t *filter);
int         env_write(Scope scope, const wchar_t *name, const wchar_t *value,
                      DWORD type);
int         env_delete(Scope scope, const wchar_t *name);
int         env_exists(Scope scope, const wchar_t *name);
/* One scope's value for `name`, xmalloc'd, or NULL when it is not set there.
 * `typeOut` may be NULL. Used for the undo stack, which has to remember what
 * a variable held before the edit rather than what the list happens to show. */
wchar_t    *env_get_value(Scope scope, const wchar_t *name, DWORD *typeOut);
/* Plain-language reason for the last env_write/env_delete failure, for the
 * "why did that not work" box. Never returns NULL. */
const wchar_t *env_error_text(DWORD err, Scope scope);
/* The registry status behind the last failed write/delete. */
DWORD        env_last_error(void);

/* Windows concatenates the machine and user PATH for new processes; this
 * reproduces that merge (case-insensitive de-duplication, order preserved). */
wchar_t *env_merged_path(const EnvList *l);
int      env_is_path_var(const wchar_t *name);

/* Split / join a PATH-style value on ';', trimming and dropping empties. */
typedef struct { wchar_t **items; int count, cap; } PathList;
void  path_free(PathList *p);
int   path_parse(const wchar_t *value, PathList *out);
wchar_t *path_join(const PathList *p);
int   path_add(PathList *p, const wchar_t *entry);
int   path_remove_at(PathList *p, int i);
int   path_move(PathList *p, int from, int to);
int   path_contains(const PathList *p, const wchar_t *entry);

/* Expand %VAR% using the process environment plus registry values. */
wchar_t *env_expand(const wchar_t *s);

/* Tell running programs (and Explorer) that the environment changed. */
int   env_broadcast_change(void);

const wchar_t *env_scope_name(Scope s);

#endif /* ENVREG_H */
