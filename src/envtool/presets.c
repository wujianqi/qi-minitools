/* presets.c - built-in development environment presets with auto-detection.
 *
 * Detection is deliberately shallow: for each configured scan root we probe a
 * handful of fixed sub-paths and one level of `name*` globs. That is fast
 * enough to run on a button press and never walks a whole drive.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "presets.h"
#include <shlobj.h>

static const Preset PRESETS[] = {
    { L"Java (JDK)", L"JAVA_HOME", NULL, L"%JAVA_HOME%\\bin", NULL, NULL,
      { L"JDK*", L"jdk*", L"java", L"Program Files/Java/*", L"Program Files/Eclipse Adoptium/*", L"Program Files/Zulu/*" } },

    { L"Android SDK", L"ANDROID_HOME", NULL, L"%ANDROID_HOME%\\platform-tools",
      L"ANDROID_SDK_ROOT", L"%ANDROID_HOME%",
      { L"android-sdk", L"Android/Sdk", L"AppData/Local/Android/Sdk", NULL, NULL } },

    { L"Node.js", L"NODE_HOME", NULL, L"%NODE_HOME%", NULL, NULL,
      { L"nodejs", L"node", L"Program Files/nodejs", L"nvm/*", NULL } },

    { L"Go", L"GOROOT", NULL, L"%GOROOT%\\bin", L"GOPATH", L"%USERPROFILE%\\go",
      { L"Go", L"go", L"Program Files/Go", NULL, NULL } },

    { L"Rust", L"RUSTUP_HOME", NULL, L"%USERPROFILE%\\.cargo\\bin", L"CARGO_HOME", L"%USERPROFILE%\\.cargo",
      { L"rust", L".cargo", L"Program Files/Rust*", NULL, NULL } },

    { L"Python", L"PYTHONHOME", L"", L"%PYTHONHOME%\\Scripts", NULL, NULL,
      { L"Python*", L"python*", L"miniconda3", L"anaconda3", L"Program Files/Python*", NULL } },

    { L"Maven", L"MAVEN_HOME", NULL, L"%MAVEN_HOME%\\bin", L"M2_HOME", L"%MAVEN_HOME%",
      { L"apache-maven*", L"maven", L"Program Files/apache-maven*", L".m2/wrapper", NULL } },

    { L"Gradle", L"GRADLE_HOME", NULL, L"%GRADLE_HOME%\\bin", NULL, NULL,
      { L"gradle*", L"Program Files/Gradle*", NULL, NULL } },

    { L"Flutter / Dart", L"FLUTTER_HOME", NULL, L"%FLUTTER_HOME%\\bin", L"PUB_CACHE", L"%USERPROFILE%\\.pub-cache",
      { L"flutter", L"Program Files/flutter", L"flutter/bin", NULL } },

    { L"Zig", L"ZIG_HOME", NULL, L"%ZIG_HOME%", NULL, NULL,
      { L"zig", L"zig-*", L"Program Files/Zig", NULL } },

    { L"LLVM / Clang", L"LLVM_HOME", NULL, L"%LLVM_HOME%\\bin", NULL, NULL,
      { L"LLVM", L"llvm", L"Program Files/LLVM", L"Program Files (x86)/LLVM", NULL } },

    { L"MinGW-w64", L"MINGW_HOME", NULL, L"%MINGW_HOME%\\bin", NULL, NULL,
      { L"mingw64", L"msys64/mingw64", L"Program Files/mingw-w64*", L"mingw", NULL } },

    { L"CMake", L"CMAKE_HOME", NULL, L"%CMAKE_HOME%\\bin", NULL, NULL,
      { L"cmake", L"CMake*", L"Program Files/CMake", NULL } },

    { L"Git", L"GIT_HOME", NULL, L"%GIT_HOME%\\cmd", NULL, NULL,
      { L"Git", L"git", L"Program Files/Git", NULL } },

    { L"SQLite", L"SQLITE_HOME", NULL, L"%SQLITE_HOME%", NULL, NULL,
      { L"sqlite3", L"sqlite", L"Program Files/SQLite", NULL } },

    { L"Bun", L"BUN_INSTALL", NULL, L"%BUN_INSTALL%", NULL, NULL,
      { L"bun", L".bun", L"Program Files/Bun", NULL } },

    { L"uv (Python)", L"UV_HOME", NULL, L"%UV_HOME%", NULL, NULL,
      { L"uv", L".local/bin", L"Program Files/uv", NULL } },

    { L"MySQL", L"MYSQL_HOME", NULL, L"%MYSQL_HOME%\\bin", NULL, NULL,
      { L"mysql*", L"Program Files/MySQL*", L"xampp/mysql", NULL } },

    { L"PostgreSQL", L"PG_HOME", NULL, L"%PG_HOME%\\bin", NULL, NULL,
      { L"PostgreSQL/*", L"pgsql", L"Program Files/PostgreSQL/*", NULL } },

    { L"CUDA", L"CUDA_PATH", NULL, L"%CUDA_PATH%\\bin", NULL, NULL,
      { L"CUDA/*", L"Program Files/NVIDIA GPU Computing Toolkit/CUDA/*", NULL } },

    { L"Perl", L"PERL_HOME", NULL, L"%PERL_HOME%\\bin", L"PERL5LIB", L"lib",
      { L"perl*", L"StrawberryPerl", L"Program Files/Perl*", NULL } },

    { L"Node 版本管理 nvm", L"NVM_HOME", NULL, L"%NVM_HOME%", L"NVM_SYMLINK", NULL,
      { L"nvm", NULL, NULL } }
};

#define PRESET_N ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

int preset_count(void) { return PRESET_N; }

const Preset *preset_at(int i)
{
    if (i < 0 || i >= PRESET_N) return NULL;
    return &PRESETS[i];
}

/* ------------------------------------------------------------------ */
/* scan roots                                                          */
/* ------------------------------------------------------------------ */

#define MAX_ROOTS 12
static wchar_t g_roots[MAX_ROOTS][MAX_PATH];
static int     g_rootCount = 0;
static int     g_rootsReady = 0;

static void default_roots(void)
{
    static const wchar_t *CANDIDATES[] = {
        L"F:\\sdk", L"D:\\sdk", L"C:\\sdk",
        L"C:\\Program Files", L"C:\\Program Files (x86)",
        L"C:\\dev", L"D:\\dev", L"C:\\tools", L"D:\\tools",
        L"C:\\Users\\Public", NULL
    };
    int i, n = 0;
    for (i = 0; CANDIDATES[i] && n < MAX_ROOTS; i++) {
        DWORD a = GetFileAttributesW(CANDIDATES[i]);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            wcsncpy(g_roots[n], CANDIDATES[i], MAX_PATH - 1);
            g_roots[n][MAX_PATH - 1] = 0;
            n++;
        }
    }
    /* Always probe the user's own tool folders too. */
    {
        wchar_t user[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", user, MAX_PATH)) {
            wchar_t sub[MAX_PATH];
            const wchar_t *subs[] = { L".cargo\\bin", L".bun\\bin", L"scoop\\shims", NULL };
            for (i = 0; subs[i] && n < MAX_ROOTS; i++) {
                if (PathCombineW(sub, user, subs[i]) &&
                    (GetFileAttributesW(sub) & FILE_ATTRIBUTE_DIRECTORY)) {
                    wcsncpy(g_roots[n], sub, MAX_PATH - 1);
                    g_roots[n][MAX_PATH - 1] = 0;
                    n++;
                }
            }
        }
    }
    g_rootCount = n;
}

static void roots_path(wchar_t *out, size_t cch)
{
    wchar_t appdata[MAX_PATH], dir[MAX_PATH], tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) { out[0] = 0; return; }
    if (!SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata)) { out[0] = 0; return; }
    if (!PathCombineW(dir, appdata, L"MuskTools")) { out[0] = 0; return; }
    CreateDirectoryW(dir, NULL);
    PathCombineW(out, dir, L"envtool_roots.txt");
    (void)cch;
}

void preset_load_roots(void)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    char buf[8192];
    DWORD got = 0;
    int n = 0;

    roots_path(path, MAX_PATH);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (ReadFile(f, buf, (DWORD)sizeof(buf) - 1, &got, NULL)) {
            DWORD i = 0;
            buf[got] = 0;
            while (i < got && n < MAX_ROOTS) {
                DWORD start = i, len;
                while (i < got && buf[i] != '\n') i++;
                len = i - start;
                while (len && (buf[start + len - 1] == '\r' || buf[start + len - 1] == ' ')) len--;
                buf[start + len] = 0;
                if (len > 1) {
                    MultiByteToWideChar(CP_UTF8, 0, buf + start, (int)len,
                                        g_roots[n], MAX_PATH);
                    if (g_roots[n][0]) n++;
                }
                if (i < got) i++;
            }
        }
        CloseHandle(f);
        g_rootCount = n;
    }
    if (!g_rootCount) default_roots();
    g_rootsReady = 1;
}

void preset_save_roots(void)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    char buf[16384];
    DWORD used = 0, written = 0;
    int i;

    if (!g_rootsReady) preset_load_roots();
    roots_path(path, MAX_PATH);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    for (i = 0; i < g_rootCount; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, g_roots[i], -1, NULL, 0, NULL, NULL);
        if (n > 1 && used + (DWORD)n + 2 < sizeof(buf)) {
            WideCharToMultiByte(CP_UTF8, 0, g_roots[i], -1, buf + used, n, NULL, NULL);
            used += (DWORD)n - 1;
            buf[used++] = '\r';
            buf[used++] = '\n';
        }
    }
    WriteFile(f, buf, used, &written, NULL);
    CloseHandle(f);
}

int preset_roots(wchar_t ***out)
{
    wchar_t **arr;
    int i;
    if (!g_rootsReady) preset_load_roots();
    arr = (wchar_t **)xcalloc((size_t)(g_rootCount > 0 ? g_rootCount : 1), sizeof(wchar_t *));
    if (!arr) { *out = NULL; return 0; }
    for (i = 0; i < g_rootCount; i++) arr[i] = xwcsdup(g_roots[i]);
    *out = arr;
    return g_rootCount;
}

void preset_free_roots(wchar_t **roots, int n)
{
    int i;
    if (!roots) return;
    for (i = 0; i < n; i++) xfree(roots[i]);
    xfree(roots);
}

int preset_set_roots(wchar_t **roots, int n)
{
    int i;
    if (!g_rootsReady) preset_load_roots();
    if (n < 0) n = 0;
    if (n > MAX_ROOTS) n = MAX_ROOTS;
    for (i = 0; i < n; i++) {
        wcsncpy(g_roots[i], roots[i], MAX_PATH - 1);
        g_roots[i][MAX_PATH - 1] = 0;
    }
    g_rootCount = n;
    preset_save_roots();
    return g_rootCount;
}

/* ------------------------------------------------------------------ */
/* detection                                                           */
/* ------------------------------------------------------------------ */

static int is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int has_exe(const wchar_t *dir, const wchar_t *rel)
{
    wchar_t full[MAX_PATH * 2];
    if (!PathCombineW(full, dir, rel)) return 0;
    return !is_dir(full) &&
           (GetFileAttributesW(full) & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* Matches a pattern with a single '*' in the final component. */
static int glob_one_level(const wchar_t *root, const wchar_t *pattern,
                          wchar_t *out, size_t outCch)
{
    wchar_t dirPart[MAX_PATH], tail[MAX_PATH];
    const wchar_t *star = wcschr(pattern, L'*');
    wchar_t search[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (!star) {
        if (!PathCombineW(search, root, pattern)) return 0;
        if (!is_dir(search)) return 0;
        wcsncpy(out, search, outCch - 1);
        out[outCch - 1] = 0;
        return 1;
    }
    if ((size_t)(star - pattern) >= sizeof(dirPart) / sizeof(wchar_t)) return 0;
    memcpy(dirPart, pattern, (size_t)(star - pattern) * sizeof(wchar_t));
    dirPart[star - pattern] = 0;
    wcsncpy(tail, star + 1, MAX_PATH - 1);
    tail[MAX_PATH - 1] = 0;

    if (dirPart[0]) {
        wchar_t sub[MAX_PATH];
        if (!PathCombineW(sub, root, dirPart)) return 0;
        if (!PathCombineW(search, sub, tail)) return 0;
    } else {
        if (!PathCombineW(search, root, tail)) return 0;
    }
    h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t full[MAX_PATH];
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        if (!PathCombineW(full, root, dirPart[0] ? dirPart : L"") ||
            !PathCombineW(full, full, fd.cFileName))
            continue;
        if (!is_dir(full)) continue;
        wcsncpy(out, full, outCch - 1);
        out[outCch - 1] = 0;
        FindClose(h);
        return 1;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

wchar_t *preset_detect(const Preset *p)
{
    int r, k;
    wchar_t found[MAX_PATH];

    if (!p) return NULL;
    if (!g_rootsReady) preset_load_roots();

    /* 1. honour an existing value for the primary variable */
    {
        DWORD need = GetEnvironmentVariableW(p->var, NULL, 0);
        if (need && need < MAX_PATH) {
            wchar_t *cur = (wchar_t *)xmalloc((size_t)need * sizeof(wchar_t) + 2);
            if (cur && GetEnvironmentVariableW(p->var, cur, need + 1) && is_dir(cur)) {
                if (p->subdir && *p->subdir) {
                    wchar_t *sub = (wchar_t *)xmalloc(MAX_PATH * sizeof(wchar_t));
                    if (sub && PathCombineW(sub, cur, p->subdir)) { xfree(cur); return sub; }
                    xfree(sub);
                }
                return cur;
            }
            xfree(cur);
        }
    }

    /* 2. probe the configured scan roots */
    for (r = 0; r < g_rootCount; r++) {
        for (k = 0; k < (int)(sizeof(p->patterns) / sizeof(p->patterns[0])); k++) {
            if (!p->patterns[k]) continue;
            if (glob_one_level(g_roots[r], p->patterns[k], found, MAX_PATH)) {
                if (p->subdir && *p->subdir) {
                    wchar_t *sub = (wchar_t *)xmalloc(MAX_PATH * sizeof(wchar_t));
                    if (sub && PathCombineW(sub, found, p->subdir) && is_dir(sub))
                        return sub;
                    xfree(sub);
                }
                return xwcsdup(found);
            }
        }
    }

    /* 3. last resort: is the tool already on PATH? If we find its executable
     *    inside a bin/cmd/Scripts folder, walk one level back to the root. */
    {
        static const wchar_t *MARKERS[] = {
            L"javac.exe", L"adb.exe", L"node.exe", L"go.exe", L"rustc.exe",
            L"python.exe", L"mvn.cmd", L"gradle.bat", L"flutter.bat",
            L"zig.exe", L"clang.exe", L"gcc.exe", L"cmake.exe",
            L"git.exe", L"sqlite3.exe", L"bun.exe", L"uv.exe",
            L"mysql.exe", L"psql.exe", L"nvcc.exe", L"perl.exe", L"nvm.cmd",
            NULL
        };
        wchar_t *pathVar = (wchar_t *)xmalloc(MAX_PATH * sizeof(wchar_t));
        DWORD need = GetEnvironmentVariableW(L"Path", pathVar, MAX_PATH);
        if (pathVar && need > 0 && need < MAX_PATH) {
            PathList list;
            int i;
            wchar_t *hit = NULL;
            path_parse(pathVar, &list);
            for (i = 0; i < list.count && !hit; i++) {
                int m;
                for (m = 0; MARKERS[m] && !hit; m++) {
                    if (!has_exe(list.items[i], MARKERS[m])) continue;
                    {
                        wchar_t root[MAX_PATH];
                        wcsncpy(root, list.items[i], MAX_PATH - 1);
                        root[MAX_PATH - 1] = 0;
                        /* strip a trailing \bin, \cmd or \Scripts */
                        {
                            wchar_t *slash = wcsrchr(root, L'\\');
                            if (slash) {
                                static const wchar_t *SUFFIX[] = { L"bin", L"cmd", L"Scripts", NULL };
                                int s;
                                for (s = 0; SUFFIX[s]; s++) {
                                    if (_wcsicmp(slash + 1, SUFFIX[s]) == 0) { *slash = 0; break; }
                                }
                            }
                        }
                        hit = xwcsdup(root);
                    }
                }
            }
            path_free(&list);
            xfree(pathVar);
            if (hit) return hit;
        } else {
            xfree(pathVar);
        }
    }
    return NULL;
}
