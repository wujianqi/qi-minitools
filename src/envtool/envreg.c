/* envreg.c - registry-backed environment variable access. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "envreg.h"

#define HKLM_ENV L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"

/* GetLastError() is read by the caller several frames away, after the
 * intermediate code has had its own chance to reset it. The registry status
 * is therefore captured here, at the one place that knows it. */
static DWORD g_lastErr = ERROR_SUCCESS;

/* ------------------------------------------------------------------ */
/* test redirection                                                     */
/* ------------------------------------------------------------------ */
/* The self-test exercises the real write path - RegCreateKeyEx, RegSetValueEx,
 * RegDeleteValue, the two scopes, REG_SZ vs REG_EXPAND_SZ - rather than a mock
 * of it, because a mocked registry never finds out whether the value is
 * actually written with the type the variable needs or silently truncated.
 * Doing that against the real environment would be reckless, so the root and
 * subkey are redirected at a throwaway key for the duration of the test.
 *
 * SCOPE_SYSTEM is mapped onto HKEY_CURRENT_USER while redirected, not onto
 * HKEY_LOCAL_MACHINE: the machine environment needs elevation, and a test that
 * needs admin rights is a test that silently does not run. Two disposable keys
 * under HKCU still prove that the two scopes stay independent. */
static const wchar_t *g_testUser = NULL;
static const wchar_t *g_testSys  = NULL;

void env_set_test_root(const wchar_t *userKey, const wchar_t *systemKey)
{
    g_testUser = userKey;
    g_testSys  = systemKey;
}

int env_test_root_active(void) { return g_testUser != NULL; }

/* Removes the scratch tree and lifts the redirection in one step, so callers
 * cannot leave either behind. The parent shells go too: RegDeleteKey takes a
 * key and everything under it, but not its parents, so deleting only the two
 * leaves would leave empty Software\MuskEnvTool\Selftest behind forever.
 * Returns 1 once the tree is confirmed gone. */
int env_test_cleanup(void)
{
    static const wchar_t *PARENT1 = L"Software\\MuskEnvTool\\Selftest";
    static const wchar_t *PARENT2 = L"Software\\MuskEnvTool";
    HKEY h = NULL;
    int gone;
    if (!g_testUser) return 0;
    RegDeleteKeyW(HKEY_CURRENT_USER, g_testUser);
    if (g_testSys) RegDeleteKeyW(HKEY_CURRENT_USER, g_testSys);
    RegDeleteKeyW(HKEY_CURRENT_USER, PARENT1);
    RegDeleteKeyW(HKEY_CURRENT_USER, PARENT2);
    gone = (RegOpenKeyExW(HKEY_CURRENT_USER, PARENT1, 0, KEY_READ, &h) != ERROR_SUCCESS);
    g_testUser = g_testSys = NULL;
    return gone;
}

const wchar_t *env_scope_name(Scope s)
{
    return s == SCOPE_SYSTEM ? L"系统" : L"用户";
}

static HKEY root_of(Scope s)
{
    if (s == SCOPE_SYSTEM) return g_testSys ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    return HKEY_CURRENT_USER;
}

static const wchar_t *path_of(Scope s)
{
    if (s == SCOPE_SYSTEM) return g_testSys ? g_testSys : HKLM_ENV;
    return g_testUser ? g_testUser : L"Environment";
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

static wchar_t *read_value(Scope scope, const wchar_t *name, DWORD *typeOut)
{
    HKEY k;
    DWORD type = 0, cb = 0;
    wchar_t *buf = NULL;

    if (RegOpenKeyExW(root_of(scope), path_of(scope), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return NULL;
    if (RegQueryValueExW(k, name, NULL, &type, NULL, &cb) != ERROR_SUCCESS) {
        RegCloseKey(k);
        return NULL;
    }
    /* +2 for the terminating NUL the registry does not guarantee */
    buf = (wchar_t *)xmalloc(cb + 2);
    if (buf) {
        if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)buf, &cb) != ERROR_SUCCESS) {
            xfree(buf);
            buf = NULL;
        } else {
            if (type != REG_SZ && type != REG_EXPAND_SZ) {
                xfree(buf);
                buf = NULL;
            } else {
                buf[cb / sizeof(wchar_t)] = 0;   /* force terminate */
                if (typeOut) *typeOut = type;
            }
        }
    }
    RegCloseKey(k);
    return buf;
}

static void list_add(EnvList *out, const wchar_t *name)
{
    int i;
    for (i = 0; i < out->count; i++) {
        if (_wcsicmp(out->items[i].name, name) == 0) return;
    }
    if (out->count == out->cap) {
        int cap = out->cap ? out->cap * 2 : 64;
        EnvVar *ni = (EnvVar *)xrealloc(out->items, (size_t)cap * sizeof(EnvVar));
        if (!ni) return;
        out->items = ni;
        out->cap = cap;
    }
    memset(&out->items[out->count], 0, sizeof(EnvVar));
    wcsncpy(out->items[out->count].name, name, ENV_NAME_MAX - 1);
    out->items[out->count].name[ENV_NAME_MAX - 1] = 0;
    out->count++;
}

static void scan_scope(EnvList *out, Scope scope, const wchar_t *filter)
{
    HKEY k;
    int i;

    if (RegOpenKeyExW(root_of(scope), path_of(scope), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    for (i = 0; ; i++) {
        wchar_t name[ENV_NAME_MAX];
        DWORD cb = sizeof(name) / sizeof(wchar_t);
        LSTATUS st = RegEnumValueW(k, i, name, &cb, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;   /* skip unreadable entries */
        name[ENV_NAME_MAX - 1] = 0;
        if (filter && !wcs_icontains(name, filter)) continue;
        list_add(out, name);
    }
    RegCloseKey(k);
}

int env_load(EnvList *out, const wchar_t *filter)
{
    int i;
    memset(out, 0, sizeof(*out));
    scan_scope(out, SCOPE_SYSTEM, filter);
    scan_scope(out, SCOPE_USER, filter);
    /* Keep an alphabetical order so the list is stable between refreshes. */
    for (i = 1; i < out->count; i++) {
        EnvVar tmp = out->items[i];
        int j = i - 1;
        while (j >= 0 && _wcsicmp(out->items[j].name, tmp.name) > 0) {
            out->items[j + 1] = out->items[j];
            j--;
        }
        out->items[j + 1] = tmp;
    }
    for (i = 0; i < out->count; i++) {
        EnvVar *v = &out->items[i];
        DWORD tu = 0, ts = 0;
        /* Both scopes are read every time. The old code stopped at the first
         * hit and left inSystem at 0 whenever the user copy existed, so a
         * variable defined in both places was reported as if only one of them
         * did - and the "用户+系统" branch in the list could never be taken. */
        v->userValue = read_value(SCOPE_USER, v->name, &tu);
        v->sysValue  = read_value(SCOPE_SYSTEM, v->name, &ts);
        v->inUser    = v->userValue != NULL;
        v->inSystem  = v->sysValue  != NULL;
        if (v->inUser) { v->value = v->userValue; v->type = tu; }
        else           { v->value = v->sysValue;  v->type = ts; }
    }
    return out->count;
}

void env_free(EnvList *l)
{
    int i;
    if (!l) return;
    for (i = 0; i < l->count; i++) {
        xfree(l->items[i].userValue);
        xfree(l->items[i].sysValue);
        l->items[i].userValue = l->items[i].sysValue = NULL;
        l->items[i].value = NULL;
    }
    xfree(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

wchar_t *env_get_value(Scope scope, const wchar_t *name, DWORD *typeOut)
{
    DWORD t = 0;
    wchar_t *v;
    if (!name || !*name) return NULL;
    v = read_value(scope, name, &t);
    if (v && typeOut) *typeOut = t;
    return v;
}

/* What a user needs when a write fails. "LONG=5" is a compiler constant, not
 * an explanation: the two cases that actually happen here are "not elevated"
 * and "the value is locked by policy", and each needs a different next step. */
const wchar_t *env_error_text(DWORD err, Scope scope)
{
    if (err == ERROR_SUCCESS)
        return L"未知原因。";
    if (scope == SCOPE_SYSTEM) {
        if (err == ERROR_ACCESS_DENIED)
            return L"没有权限写入系统环境变量。\r\n\r\n"
                   L"系统环境由 Windows 保护，需要管理员权限。\r\n"
                   L"右键本程序选择【以管理员身份运行】，然后再保存一次。";
        if (err == ERROR_INVALID_NAME)
            return L"变量名里含有系统不允许的字符。\r\n\r\n"
                   L"环境变量名只能用字母、数字和下划线。";
    } else {
        if (err == ERROR_ACCESS_DENIED)
            return L"没有权限写入用户环境变量。\r\n\r\n"
                   L"可能组策略禁止了修改，或当前用户配置文件不可写。";
    }
    if (err == ERROR_FILE_NOT_FOUND)
        return L"注册表里找不到这个变量，它可能刚刚被别的程序删掉了。\r\n\r\n"
               L"点【刷新】重新读取后再试。";
    if (err == ERROR_WRITE_PROTECT)
        return L"这个位置被写保护，通常是组策略锁定了环境变量。";
    return L"写入失败。\r\n\r\n"
           L"先点【刷新】确认变量还在，再试一次；仍然失败就以管理员身份重启本工具。";
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

int env_write(Scope scope, const wchar_t *name, const wchar_t *value, DWORD type)
{
    HKEY k;
    LSTATUS st;
    DWORD bytes;

    if (!name || !*name) { g_lastErr = ERROR_INVALID_PARAMETER; return 0; }
    st = RegCreateKeyExW(root_of(scope), path_of(scope), 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &k, NULL);
    if (st != ERROR_SUCCESS) { g_lastErr = (DWORD)st; return 0; }
    if (!value) {
        st = RegDeleteValueW(k, name);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    } else {
        bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
        st = RegSetValueExW(k, name, 0, type, (const BYTE *)value, bytes);
    }
    RegCloseKey(k);
    if (st != ERROR_SUCCESS) { g_lastErr = (DWORD)st; return 0; }
    /* Update our own process copy so anything we spawn inherits the change.
     * Skipped while redirected: the test writes names like MUSK_TEST_ONE into
     * a key nobody reads, and only the registry is under test here - letting
     * them into the process environment would just leave litter that a later
     * expansion in the same run could pick up by accident. */
    if (!g_testUser)
        SetEnvironmentVariableW(name, value);
    g_lastErr = ERROR_SUCCESS;
    return 1;
}

DWORD env_last_error(void) { return g_lastErr; }

int env_delete(Scope scope, const wchar_t *name)
{
    return env_write(scope, name, NULL, REG_EXPAND_SZ);
}

int env_exists(Scope scope, const wchar_t *name)
{
    HKEY k;
    int found = 0;
    if (RegOpenKeyExW(root_of(scope), path_of(scope), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExW(k, name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
        found = 1;
    RegCloseKey(k);
    return found;
}

int env_broadcast_change(void)
{
    /* Explorer and every new process pick this up. PATH itself is only re-read
     * by Explorer on logon, so the UI also offers a Explorer restart. */
    LRESULT r = SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                    (LPARAM)L"Environment",
                                    SMTO_ABORTIFHUNG, 2000, NULL);
    return r != 0;
}

/* ------------------------------------------------------------------ */
/* PATH handling                                                       */
/* ------------------------------------------------------------------ */

int env_is_path_var(const wchar_t *name)
{
    /* Windows only special-cases "Path"; "PATH" and "Path" both work. */
    return _wcsicmp(name, L"Path") == 0;
}

void path_free(PathList *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < p->count; i++) xfree(p->items[i]);
    xfree(p->items);
    p->items = NULL;
    p->count = p->cap = 0;
}

int path_parse(const wchar_t *value, PathList *out)
{
    const wchar_t *p = value;
    memset(out, 0, sizeof(*out));
    if (!value) return 0;
    while (*p) {
        const wchar_t *sep = wcschr(p, L';');
        size_t n = sep ? (size_t)(sep - p) : wcslen(p);
        if (n > 0) {
            wchar_t *e = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
            if (e) {
                memcpy(e, p, n * sizeof(wchar_t));
                e[n] = 0;
                wcs_rtrim(e);
                if (*e) {
                    if (out->count == out->cap) {
                        int cap = out->cap ? out->cap * 2 : 16;
                        wchar_t **ni = (wchar_t **)xrealloc(out->items,
                                          (size_t)cap * sizeof(wchar_t *));
                        if (!ni) { xfree(e); break; }
                        out->items = ni;
                        out->cap = cap;
                    }
                    out->items[out->count++] = e;
                    e = NULL;
                }
                xfree(e);
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return out->count;
}

wchar_t *path_join(const PathList *p)
{
    WBuf b;
    int i;
    wbuf_init(&b);
    for (i = 0; i < p->count; i++) {
        if (i) wbuf_addc(&b, L';');
        wbuf_add(&b, p->items[i]);
    }
    return b.buf;   /* caller xfree()s; may be NULL for an empty list */
}

int path_add(PathList *p, const wchar_t *entry)
{
    wchar_t *e;
    if (!entry || !*entry) return 0;
    e = xwcsdup(entry);
    if (!e) return 0;
    wcs_rtrim(e);
    if (!*e) { xfree(e); return 0; }
    if (p->count == p->cap) {
        int cap = p->cap ? p->cap * 2 : 16;
        wchar_t **ni = (wchar_t **)xrealloc(p->items, (size_t)cap * sizeof(wchar_t *));
        if (!ni) { xfree(e); return 0; }
        p->items = ni;
        p->cap = cap;
    }
    p->items[p->count++] = e;
    return 1;
}

int path_remove_at(PathList *p, int i)
{
    if (i < 0 || i >= p->count) return 0;
    xfree(p->items[i]);
    memmove(&p->items[i], &p->items[i + 1],
            (size_t)(p->count - i - 1) * sizeof(wchar_t *));
    p->count--;
    return 1;
}

int path_move(PathList *p, int from, int to)
{
    wchar_t *tmp;
    if (from < 0 || from >= p->count || to < 0 || to >= p->count || from == to)
        return 0;
    tmp = p->items[from];
    if (from < to) {
        memmove(&p->items[from], &p->items[from + 1],
                (size_t)(to - from) * sizeof(wchar_t *));
    } else {
        memmove(&p->items[to + 1], &p->items[to],
                (size_t)(from - to) * sizeof(wchar_t *));
    }
    p->items[to] = tmp;
    return 1;
}

int path_contains(const PathList *p, const wchar_t *entry)
{
    int i;
    for (i = 0; i < p->count; i++)
        if (_wcsicmp(p->items[i], entry) == 0) return 1;
    return 0;
}

wchar_t *env_merged_path(const EnvList *l)
{
    PathList merged;
    WBuf b;
    int i, first = 1;

    wbuf_init(&b);
    path_parse(L"", &merged);
    for (i = 0; i < l->count; i++) {
        const EnvVar *v = &l->items[i];
        PathList one;
        int j;
        if (!env_is_path_var(v->name) || !v->value) continue;
        path_parse(v->value, &one);
        for (j = 0; j < one.count; j++) {
            if (path_contains(&merged, one.items[j])) continue;
            if (!first) wbuf_addc(&b, L';');
            wbuf_add(&b, one.items[j]);
            first = 0;
            path_add(&merged, one.items[j]);
        }
        path_free(&one);
    }
    path_free(&merged);
    return b.buf;
}

/* ------------------------------------------------------------------ */
/* %VAR% expansion                                                     */
/* ------------------------------------------------------------------ */

wchar_t *env_expand(const wchar_t *s)
{
    WBuf out;
    const wchar_t *p = s;

    if (!s) return NULL;
    wbuf_init(&out);
    while (*p) {
        if (p[0] == L'%') {
            const wchar_t *end = wcschr(p + 1, L'%');
            if (end && end > p + 1) {
                size_t n = (size_t)(end - p - 1);
                wchar_t *name = (wchar_t *)xmalloc((n + 1) * sizeof(wchar_t));
                wchar_t *val = NULL;
                if (name) {
                    DWORD need;
                    memcpy(name, p + 1, n * sizeof(wchar_t));
                    name[n] = 0;
                    need = GetEnvironmentVariableW(name, NULL, 0);
                    if (need) {
                        val = (wchar_t *)xmalloc((size_t)need * sizeof(wchar_t) + 2);
                        if (val) GetEnvironmentVariableW(name, val, need + 1);
                    }
                    xfree(name);
                }
                if (val) {
                    wbuf_add(&out, val);
                    xfree(val);
                } else {
                    wbuf_addn(&out, p, (size_t)(end - p) + 1);   /* keep as-is */
                }
                p = end + 1;
                continue;
            }
        }
        wbuf_addc(&out, *p++);
    }
    if (!out.buf) {
        /* always hand back a valid (possibly empty) string */
        out.buf = (wchar_t *)xmalloc(sizeof(wchar_t));
        if (out.buf) out.buf[0] = 0;
    }
    return out.buf;
}
