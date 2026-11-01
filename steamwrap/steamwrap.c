/*
 * steamwrap: steam_api.dll wrapper for the Alien: Isolation co-op TEST COPY (Steam build 4940658).
 *
 * - Forwards every steam_api export to the real DLL (renamed steam_api_real.dll) through jmp thunks,
 *   so no argument counts have to be known for anything that is merely passed through.
 * - SteamAPI_RestartAppIfNecessary -> false, so the test copy never relaunches the registered install.
 * - SteamRemoteStorage(): local per-profile file store for slots 0..32 (file + cloud-sync API).
 *   The owner's Steam Cloud saves are never read or written. Slots 33+ (workshop/UGC) forward.
 * - SteamUserStats(): SetStat/SetAchievement/ClearAchievement/StoreStats/IndicateProgress are no-ops
 *   returning true; everything else forwards.
 * - If AIC_PROFILE is set: the single-instance semaphore/mutex name gets "_<profile>" appended
 *   (IAT redirect of CreateSemaphoreA/W, CreateMutexA/W in AI.exe), so two instances can run.
 * - Loads coopnet.dll from the game dir if present.
 *
 * Signatures come from the Steamworks interface headers ISteamRemoteStorage012 / ISteamUserStats011
 * (versions read from the game's own steam_api.dll).
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define THISCALL __attribute__((thiscall))
typedef uint64_t u64;

static HMODULE g_real;
static wchar_t g_gamedir[MAX_PATH];
static char g_savedir[MAX_PATH];
static char g_profile[64];
static FILE *g_log;
static CRITICAL_SECTION g_cs;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

/* ---------- pass-through exports ---------- */
#define REAL_LIST(X) X(SteamAPI_Init) X(SteamAPI_Shutdown) X(SteamAPI_RunCallbacks) \
    X(SteamAPI_RegisterCallback) X(SteamAPI_UnregisterCallback) X(SteamAPI_RegisterCallResult) \
    X(SteamAPI_UnregisterCallResult) X(SteamUtils) X(SteamFriends) X(SteamUser) X(SteamApps) \
    X(SteamRemoteStorage) X(SteamUserStats)
#define DECL(n) void *real_##n;
REAL_LIST(DECL)

#define THUNK(n) __attribute__((naked)) void w_##n(void) { __asm__("jmp *_real_" #n); }
THUNK(SteamAPI_Shutdown) THUNK(SteamAPI_RunCallbacks)
THUNK(SteamAPI_RegisterCallback) THUNK(SteamAPI_UnregisterCallback)
THUNK(SteamAPI_RegisterCallResult) THUNK(SteamAPI_UnregisterCallResult)
THUNK(SteamUtils) THUNK(SteamFriends) THUNK(SteamUser) THUNK(SteamApps)

int __cdecl w_SteamAPI_RestartAppIfNecessary(uint32_t appid) {
    logf_("RestartAppIfNecessary(%u) -> false", appid);
    return 0;
}

int __cdecl w_SteamAPI_Init(void) {
    int r = ((int (__cdecl *)(void))real_SteamAPI_Init)() & 0xFF;
    logf_("SteamAPI_Init -> %d", r);
    return r;
}

/* ---------- generic forwarding vtable ---------- */
/* Wrapper object layout: { vtbl, real_iface }. Forward thunk i: ecx = real; jmp [[real]+4*i]. */
typedef struct { void **vtbl; void *real; } Wrap;
#define NFWD 96
#define FWD(i) __attribute__((naked)) static void fwd_##i(void) { __asm__( \
    "movl 4(%ecx), %ecx\n\t" "movl (%ecx), %eax\n\t" "jmp *" #i "*4(%eax)"); }
#define R8(b) FWD(b##0) FWD(b##1) FWD(b##2) FWD(b##3) FWD(b##4) FWD(b##5) FWD(b##6) FWD(b##7)
FWD(0) FWD(1) FWD(2) FWD(3) FWD(4) FWD(5) FWD(6) FWD(7) FWD(8) FWD(9)
R8(1) FWD(18) FWD(19) R8(2) FWD(28) FWD(29) R8(3) FWD(38) FWD(39) R8(4) FWD(48) FWD(49)
R8(5) FWD(58) FWD(59) R8(6) FWD(68) FWD(69) R8(7) FWD(78) FWD(79) R8(8) FWD(88) FWD(89)
FWD(90) FWD(91) FWD(92) FWD(93) FWD(94) FWD(95)
#define E8(b) fwd_##b##0, fwd_##b##1, fwd_##b##2, fwd_##b##3, fwd_##b##4, fwd_##b##5, fwd_##b##6, fwd_##b##7
static void *g_fwd[NFWD] = {
    fwd_0, fwd_1, fwd_2, fwd_3, fwd_4, fwd_5, fwd_6, fwd_7, fwd_8, fwd_9,
    E8(1), fwd_18, fwd_19, E8(2), fwd_28, fwd_29, E8(3), fwd_38, fwd_39, E8(4), fwd_48, fwd_49,
    E8(5), fwd_58, fwd_59, E8(6), fwd_68, fwd_69, E8(7), fwd_78, fwd_79, E8(8), fwd_88, fwd_89,
    fwd_90, fwd_91, fwd_92, fwd_93, fwd_94, fwd_95 };

/* ---------- local RemoteStorage (ISteamRemoteStorage012 slots 0..32) ---------- */
static void save_path(char *out, const char *name) {
    char clean[MAX_PATH]; size_t i = 0;
    for (; name && name[i] && i < sizeof clean - 1; i++) {
        char c = name[i];
        clean[i] = (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
                    c == '<' || c == '>' || c == '|') ? '_' : c;
    }
    clean[i] = 0;
    snprintf(out, MAX_PATH, "%s\\%s", g_savedir, clean);
}

static int THISCALL rs_FileWrite(Wrap *s, const char *f, const void *d, int32_t n) {
    (void)s; char p[MAX_PATH]; save_path(p, f);
    FILE *fp = fopen(p, "wb"); int ok = 0;
    if (fp) { ok = n <= 0 || fwrite(d, 1, (size_t)n, fp) == (size_t)n; ok &= fclose(fp) == 0; }
    logf_("RS FileWrite %s %d -> %d", f, n, ok);
    return ok;
}
static int32_t THISCALL rs_FileRead(Wrap *s, const char *f, void *d, int32_t n) {
    (void)s; char p[MAX_PATH]; save_path(p, f);
    FILE *fp = fopen(p, "rb"); int32_t r = 0;
    if (fp && n > 0) r = (int32_t)fread(d, 1, (size_t)n, fp);
    if (fp) fclose(fp);
    logf_("RS FileRead %s %d -> %d", f, n, r);
    return r;
}
static int THISCALL rs_FileDelete(Wrap *s, const char *f) {
    (void)s; char p[MAX_PATH]; save_path(p, f);
    int ok = DeleteFileA(p) != 0; logf_("RS FileDelete %s -> %d", f, ok); return ok;
}
static int THISCALL rs_FileForget(Wrap *s, const char *f) { (void)s; logf_("RS FileForget %s", f); return 1; }
static u64 THISCALL rs_FileShare(Wrap *s, const char *f) { (void)s; logf_("RS FileShare %s", f); return 0; }
static int THISCALL rs_SetSyncPlatforms(Wrap *s, const char *f, int e) { (void)s; (void)f; (void)e; return 1; }

/* write streams: one buffered file per handle */
typedef struct { char path[MAX_PATH]; FILE *fp; } Stream;
static Stream g_streams[16];
static u64 THISCALL rs_FWSOpen(Wrap *s, const char *f) {
    (void)s;
    for (int i = 0; i < 16; i++) if (!g_streams[i].fp) {
        save_path(g_streams[i].path, f);
        char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.part", g_streams[i].path);
        g_streams[i].fp = fopen(tmp, "wb");
        logf_("RS StreamOpen %s -> %d", f, g_streams[i].fp ? i + 1 : 0);
        return g_streams[i].fp ? (u64)(i + 1) : (u64)-1;
    }
    return (u64)-1;
}
static Stream *stream_of(u64 h) { return (h >= 1 && h <= 16 && g_streams[h - 1].fp) ? &g_streams[h - 1] : NULL; }
static int THISCALL rs_FWSWrite(Wrap *s, u64 h, const void *d, int32_t n) {
    (void)s; Stream *st = stream_of(h);
    return st && (n <= 0 || fwrite(d, 1, (size_t)n, st->fp) == (size_t)n);
}
static int THISCALL rs_FWSClose(Wrap *s, u64 h) {
    (void)s; Stream *st = stream_of(h); if (!st) return 0;
    fclose(st->fp); st->fp = NULL;
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.part", st->path);
    int ok = MoveFileExA(tmp, st->path, MOVEFILE_REPLACE_EXISTING) != 0;
    logf_("RS StreamClose %s -> %d", st->path, ok); return ok;
}
static int THISCALL rs_FWSCancel(Wrap *s, u64 h) {
    (void)s; Stream *st = stream_of(h); if (!st) return 0;
    fclose(st->fp); st->fp = NULL;
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.part", st->path); DeleteFileA(tmp);
    return 1;
}
static int file_attr(const char *f, WIN32_FILE_ATTRIBUTE_DATA *a) {
    char p[MAX_PATH]; save_path(p, f);
    return GetFileAttributesExA(p, GetFileExInfoStandard, a) && !(a->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
static int THISCALL rs_FileExists(Wrap *s, const char *f) {
    (void)s; WIN32_FILE_ATTRIBUTE_DATA a; int r = file_attr(f, &a); logf_("RS FileExists %s -> %d", f, r); return r;
}
static int THISCALL rs_FilePersisted(Wrap *s, const char *f) { WIN32_FILE_ATTRIBUTE_DATA a; (void)s; return file_attr(f, &a); }
static int32_t THISCALL rs_GetFileSize(Wrap *s, const char *f) {
    (void)s; WIN32_FILE_ATTRIBUTE_DATA a; int32_t r = file_attr(f, &a) ? (int32_t)a.nFileSizeLow : 0;
    logf_("RS GetFileSize %s -> %d", f, r); return r;
}
static int64_t THISCALL rs_GetFileTimestamp(Wrap *s, const char *f) {
    (void)s; WIN32_FILE_ATTRIBUTE_DATA a; if (!file_attr(f, &a)) return 0;
    ULARGE_INTEGER u; u.LowPart = a.ftLastWriteTime.dwLowDateTime; u.HighPart = a.ftLastWriteTime.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
static int THISCALL rs_GetSyncPlatforms(Wrap *s, const char *f) { (void)s; (void)f; return -1; /* all */ }

/* file enumeration: snapshot on GetFileCount */
static char g_names[256][MAX_PATH]; static int32_t g_sizes[256]; static int g_nfiles;
static int32_t THISCALL rs_GetFileCount(Wrap *s) {
    (void)s; char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", g_savedir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); g_nfiles = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t L = strlen(fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (L > 5 && !strcmp(fd.cFileName + L - 5, ".part"))) continue;
            if (g_nfiles < 256) { lstrcpynA(g_names[g_nfiles], fd.cFileName, MAX_PATH); g_sizes[g_nfiles++] = (int32_t)fd.nFileSizeLow; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    logf_("RS GetFileCount -> %d", g_nfiles); return g_nfiles;
}
static const char *THISCALL rs_GetFileNameAndSize(Wrap *s, int i, int32_t *sz) {
    (void)s; if (i < 0 || i >= g_nfiles) { if (sz) *sz = 0; return ""; }
    if (sz) *sz = g_sizes[i]; return g_names[i];
}
static int THISCALL rs_GetQuota(Wrap *s, int32_t *tot, int32_t *avail) {
    (void)s; if (tot) *tot = 100 << 20; if (avail) *avail = 90 << 20; return 1;
}
static int THISCALL rs_True0(Wrap *s) { (void)s; return 1; }
static int THISCALL rs_False0(Wrap *s) { (void)s; return 0; }
static void THISCALL rs_SetCloudEnabled(Wrap *s, int b) { (void)s; (void)b; }
static void THISCALL rs_Void0(Wrap *s) { (void)s; }
static int THISCALL rs_FileFetch(Wrap *s, const char *f) { WIN32_FILE_ATTRIBUTE_DATA a; (void)s; return file_attr(f, &a); }
static int THISCALL rs_FilePersist(Wrap *s, const char *f) { WIN32_FILE_ATTRIBUTE_DATA a; (void)s; return file_attr(f, &a); }
/* UGC slots 21..26 have handle args; they touch downloads, not saves -> forward them. */

static void *g_rs_vtbl[NFWD];
static Wrap g_rs, g_us;

void *__cdecl w_SteamRemoteStorage(void) {
    void *real = ((void *(__cdecl *)(void))real_SteamRemoteStorage)();
    if (!real) { logf_("SteamRemoteStorage: real returned NULL"); }
    g_rs.real = real; return &g_rs;
}

/* ---------- UserStats (ISteamUserStats011): block writes ---------- */
static void *g_us_vtbl[NFWD];
static int THISCALL us_SetStatI(Wrap *s, const char *n, int32_t v) { (void)s; logf_("US SetStat %s=%d (blocked)", n, v); return 1; }
static int THISCALL us_SetStatF(Wrap *s, const char *n, float v) { (void)s; logf_("US SetStatF %s=%f (blocked)", n, v); return 1; }
static int THISCALL us_UpdAvg(Wrap *s, const char *n, float a, double b) { (void)s; (void)a; (void)b; logf_("US UpdateAvgRateStat %s (blocked)", n); return 1; }
static int THISCALL us_SetAch(Wrap *s, const char *n) { (void)s; logf_("US SetAchievement %s (blocked)", n); return 1; }
static int THISCALL us_ClrAch(Wrap *s, const char *n) { (void)s; logf_("US ClearAchievement %s (blocked)", n); return 1; }
static int THISCALL us_Store(Wrap *s) { (void)s; logf_("US StoreStats (blocked)"); return 1; }
static int THISCALL us_Indicate(Wrap *s, const char *n, uint32_t a, uint32_t b) { (void)s; logf_("US IndicateProgress %s %u/%u (blocked)", n, a, b); return 1; }

void *__cdecl w_SteamUserStats(void) {
    g_us.real = ((void *(__cdecl *)(void))real_SteamUserStats)();
    return &g_us;
}

static void build_vtables(void) {
    memcpy(g_rs_vtbl, g_fwd, sizeof g_fwd);
    void *rs[] = { rs_FileWrite, rs_FileRead, rs_FileForget, rs_FileDelete, rs_FileShare, rs_SetSyncPlatforms,
        rs_FWSOpen, rs_FWSWrite, rs_FWSClose, rs_FWSCancel, rs_FileExists, rs_FilePersisted, rs_GetFileSize,
        rs_GetFileTimestamp, rs_GetSyncPlatforms, rs_GetFileCount, rs_GetFileNameAndSize, rs_GetQuota,
        rs_False0 /*18 IsCloudEnabledForAccount*/, rs_False0 /*19 IsCloudEnabledForApp*/, rs_SetCloudEnabled };
    memcpy(g_rs_vtbl, rs, sizeof rs);                     /* 0..20 */
    g_rs_vtbl[27] = rs_Void0;                             /* GetFileListFromServer */
    g_rs_vtbl[28] = rs_FileFetch; g_rs_vtbl[29] = rs_FilePersist;
    g_rs_vtbl[30] = rs_True0;  /* SynchronizeToClient */
    g_rs_vtbl[31] = rs_True0;  /* SynchronizeToServer: never touch the owner's cloud */
    g_rs_vtbl[32] = rs_True0;  /* ResetFileRequestState */
    g_rs.vtbl = g_rs_vtbl;

    memcpy(g_us_vtbl, g_fwd, sizeof g_fwd);
    g_us_vtbl[3] = us_SetStatI; g_us_vtbl[4] = us_SetStatF; g_us_vtbl[5] = us_UpdAvg;
    g_us_vtbl[7] = us_SetAch; g_us_vtbl[8] = us_ClrAch; g_us_vtbl[10] = us_Store; g_us_vtbl[13] = us_Indicate;
    g_us.vtbl = g_us_vtbl;
}

/* ---------- per-profile single-instance names ---------- */
static HANDLE (WINAPI *o_CSA)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCSTR);
static HANDLE (WINAPI *o_CSW)(LPSECURITY_ATTRIBUTES, LONG, LONG, LPCWSTR);
static HANDLE (WINAPI *o_CMA)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static HANDLE (WINAPI *o_CMW)(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
static const char *nameA(LPCSTR n, char *buf) {
    if (!n || !g_profile[0]) return n; snprintf(buf, 300, "%s_%s", n, g_profile); return buf;
}
static const wchar_t *nameW(LPCWSTR n, wchar_t *buf) {
    if (!n || !g_profile[0]) return n; _snwprintf(buf, 300, L"%ls_%hs", n, g_profile); buf[299] = 0; return buf;
}
static HANDLE WINAPI h_CSA(LPSECURITY_ATTRIBUTES a, LONG i, LONG m, LPCSTR n) {
    char b[300]; const char *x = nameA(n, b); logf_("CreateSemaphoreA '%s' -> '%s'", n ? n : "(null)", x ? x : "(null)"); return o_CSA(a, i, m, x);
}
static HANDLE WINAPI h_CSW(LPSECURITY_ATTRIBUTES a, LONG i, LONG m, LPCWSTR n) {
    wchar_t b[300]; const wchar_t *x = nameW(n, b); logf_("CreateSemaphoreW '%ls' -> '%ls'", n ? n : L"(null)", x ? x : L"(null)"); return o_CSW(a, i, m, x);
}
static HANDLE WINAPI h_CMA(LPSECURITY_ATTRIBUTES a, BOOL o, LPCSTR n) {
    char b[300]; const char *x = nameA(n, b); logf_("CreateMutexA '%s' -> '%s'", n ? n : "(null)", x ? x : "(null)"); return o_CMA(a, o, x);
}
static HANDLE WINAPI h_CMW(LPSECURITY_ATTRIBUTES a, BOOL o, LPCWSTR n) {
    wchar_t b[300]; const wchar_t *x = nameW(n, b); logf_("CreateMutexW '%ls' -> '%ls'", n ? n : L"(null)", x ? x : L"(null)"); return o_CMW(a, o, x);
}

static int patch_iat(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig) {
    BYTE *b = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(b + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn)) continue;
            DWORD old; VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            *orig = (void *)iat->u1.Function; iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* ---------- init ---------- */
static void init(HMODULE self) {
    InitializeCriticalSection(&g_cs);
    GetModuleFileNameW(self, g_gamedir, MAX_PATH);
    wchar_t *sl = wcsrchr(g_gamedir, L'\\'); if (sl) *sl = 0;

    DWORD n = GetEnvironmentVariableA("AIC_PROFILE", g_profile, sizeof g_profile);
    if (n == 0 || n >= sizeof g_profile) g_profile[0] = 0;
    for (char *c = g_profile; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) *c = '_';

    char gd[MAX_PATH]; WideCharToMultiByte(CP_ACP, 0, g_gamedir, -1, gd, MAX_PATH, NULL, NULL);
    char logdir[MAX_PATH]; snprintf(logdir, sizeof logdir, "%s\\coop_logs", gd); CreateDirectoryA(logdir, NULL);
    char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\steamwrap_%s_%lu.log", logdir, g_profile[0] ? g_profile : "default", GetCurrentProcessId());
    g_log = fopen(lp, "w");

    char sd[MAX_PATH]; snprintf(sd, sizeof sd, "%s\\coop_saves", gd); CreateDirectoryA(sd, NULL);
    snprintf(g_savedir, sizeof g_savedir, "%s\\%s", sd, g_profile[0] ? g_profile : "default");
    CreateDirectoryA(g_savedir, NULL);
    logf_("steamwrap up. profile='%s' savedir='%s'", g_profile, g_savedir);

    wchar_t rp[MAX_PATH]; _snwprintf(rp, MAX_PATH, L"%ls\\steam_api_real.dll", g_gamedir); rp[MAX_PATH - 1] = 0;
    g_real = LoadLibraryW(rp);
    if (!g_real) { logf_("FATAL: cannot load steam_api_real.dll (err %lu)", GetLastError()); return; }
#define RES(nm) real_##nm = (void *)GetProcAddress(g_real, #nm); if (!real_##nm) logf_("missing export " #nm);
    REAL_LIST(RES)
    build_vtables();

    if (g_profile[0]) {
        HMODULE exe = GetModuleHandleW(NULL);
        int a = patch_iat(exe, "KERNEL32.dll", "CreateSemaphoreA", h_CSA, (void **)&o_CSA);
        int b = patch_iat(exe, "KERNEL32.dll", "CreateSemaphoreW", h_CSW, (void **)&o_CSW);
        int c = patch_iat(exe, "KERNEL32.dll", "CreateMutexA", h_CMA, (void **)&o_CMA);
        int d = patch_iat(exe, "KERNEL32.dll", "CreateMutexW", h_CMW, (void **)&o_CMW);
        logf_("IAT redirects: CSA=%d CSW=%d CMA=%d CMW=%d", a, b, c, d);
    }

    wchar_t cp[MAX_PATH]; _snwprintf(cp, MAX_PATH, L"%ls\\coopnet.dll", g_gamedir); cp[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(cp) != INVALID_FILE_ATTRIBUTES) {
        HMODULE c = LoadLibraryW(cp); logf_("coopnet.dll load -> %p (err %lu)", (void *)c, c ? 0 : GetLastError());
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init(h); }
    else if (reason == DLL_PROCESS_DETACH && g_log) { logf_("detach"); fclose(g_log); g_log = NULL; }
    return TRUE;
}
