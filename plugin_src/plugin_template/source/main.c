#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

#include <Common.h>
#include "plugin_common.h"
#include <orbis/libkernel.h>

attr_public const char *g_pluginName = "trophy_restore";
attr_public const char *g_pluginDesc = "Unlock trophies from list with custom time (multi-game, forced init)";
attr_public const char *g_pluginAuth = "you";
attr_public u32 g_pluginVersion = 0x00000109;

#define LIST_PATH "/data/trophy_unlock.txt"
#define DONE_PATH "/data/trophy_unlock.done"
#define LOG_PATH  "/data/trophy_restore_log.txt"

typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef u64 SceNpTrophyId;
typedef struct { u64 tick; } RtcTick;

s32 sceNpTrophyInit(const char *commId, const char *svcLabel);
s32 sceNpTrophyCreateContext(SceNpTrophyContext *ctx, s32 userId, u32 label, u64 opt);
s32 sceNpTrophyCreateHandle(SceNpTrophyHandle *h);
s32 sceNpTrophyRegisterContext(SceNpTrophyContext ctx, SceNpTrophyHandle h, u64 opt);
s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext ctx, SceNpTrophyHandle h,
                            SceNpTrophyId id, SceNpTrophyId *platinumId);
s32 sceNpTrophyGetErrorString(s32 err, char *buf, size_t len);
s32 sceRtcGetCurrentTick(RtcTick *t);
s32 sceRtcSetCurrentTick(const RtcTick *t);
s32 sceKernelGetProcessTitleId(char *titleId, size_t len);

static s32 (*real_sceNpTrophyInit)(const char *, const char *) = NULL;
static s32 (*real_sceNpTrophyCreateContext)(SceNpTrophyContext *, s32, u32, u64) = NULL;
static s32 (*real_sceNpTrophyCreateHandle)(SceNpTrophyHandle *) = NULL;
static s32 (*real_sceNpTrophyRegisterContext)(SceNpTrophyContext, SceNpTrophyHandle, u64) = NULL;
static s32 (*real_sceNpTrophyUnlockTrophy)(SceNpTrophyContext, SceNpTrophyHandle, SceNpTrophyId, SceNpTrophyId *) = NULL;
static s32 (*real_sceNpTrophyGetErrorString)(s32, char *, size_t) = NULL;
static s32 (*real_sceRtcGetCurrentTick)(RtcTick *) = NULL;
static s32 (*real_sceRtcSetCurrentTick)(const RtcTick *) = NULL;
static s32 (*real_sceKernelGetProcessTitleId)(char *, size_t) = NULL;

static SceNpTrophyContext g_ctx = -1;
static SceNpTrophyHandle g_handle = -1;
static volatile int g_done = 0;
static volatile int g_hooks_installed = 0;
static volatile int g_symbols_resolved = 0;
static volatile int g_init_done = 0;
static char g_current_npwr[32] = {0};

HOOK_INIT(sceNpTrophyCreateContext);
HOOK_INIT(sceNpTrophyCreateHandle);
HOOK_INIT(sceNpTrophyRegisterContext);
HOOK_INIT(sceNpTrophyInit);

typedef struct { const char *cusa; const char *npwr; } GameMap;
static const GameMap GAME_MAP[] = {
    {"CUSA00028", "NPWR05236_00"}, {"CUSA01395", "NPWR05212_00"},
    {"CUSA02344", "NPWR09798_00"}, {"CUSA02357", "NPWR09525_00"},
    {"CUSA05380", "NPWR11068_00"}, {"CUSA08308", "NPWR13872_00"},
    {"CUSA10249", "NPWR13348_00"}, {"CUSA10960", "NPWR15392_00"},
    {"CUSA11995", "NPWR09167_00"}, {"CUSA36843", "NPWR27622_00"},
    {NULL, NULL}
};

static const char *resolve_npwr(const char *cusa) {
    for (int i = 0; GAME_MAP[i].cusa; i++)
        if (strcmp(cusa, GAME_MAP[i].cusa) == 0) return GAME_MAP[i].npwr;
    return NULL;
}

#define notify(fmt, ...) final_printf("[trophy_restore] " fmt "\n", ##__VA_ARGS__)

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

#define LOG(fmt, ...) logf_("[%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

static s64 parse_time(const char *s) {
    int y, mo, d, h, mi;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int leap = (y%4==0 && (y%100!=0 || y%400==0));
    int doy = mdays[mo-1] + d - 1 + (mo>2 && leap);
    s64 days = (s64)(y-1970)*365 + (y-1969)/4 - (y-1901)/100 + (y-1601)/400 + doy;
    return days*86400 + h*3600 + mi*60;
}

// ===== FORWARD DECLARATIONS =====
static s32 sceNpTrophyCreateContext_hook(SceNpTrophyContext *ctx, s32 userId, u32 label, u64 opt);
static s32 sceNpTrophyCreateHandle_hook(SceNpTrophyHandle *h);
static s32 sceNpTrophyRegisterContext_hook(SceNpTrophyContext ctx, SceNpTrophyHandle h, u64 opt);
static s32 sceNpTrophyInit_hook(const char *commId, const char *svcLabel);
static void process_list(void);  // <--- FORWARD DECLARATION

// ===== TRY PROCESS LIST =====
static void try_process_list(void) {
    if (g_done || !g_current_npwr[0]) return;
    if (g_ctx < 0 || g_handle < 0) { LOG("try_process_list: ctx/handle not ready"); return; }
    
    g_done = 1;
    LOG("try_process_list: FORCING process_list");
    process_list();
}

static void process_list(void) {
    LOG("process_list: START, npwr='%s'", g_current_npwr);
    if (!g_current_npwr[0]) { LOG("ERROR: NPWR not resolved"); notify("ERROR no NPWR"); return; }

    FILE *f = fopen(LIST_PATH, "r");
    if (!f) { LOG("no list file"); notify("no unlock file"); return; }
    LOG("process_list: opened %s", LIST_PATH);

    char line[256];
    int line_num = 0, processed = 0;
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        if (line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
        if (!strncmp(line, "tz=", 3)) continue;

        char id_str[64], time_str[64];
        if (sscanf(line, "%63[^;];%63s", id_str, time_str) != 2) continue;

        SceNpTrophyId id = 0;
        if (id_str[0]=='0' && (id_str[1]=='x' || id_str[1]=='X'))
            sscanf(id_str, "%llx", (unsigned long long*)&id);
        else continue;

        s64 unix_time = parse_time(time_str);
        if (!unix_time) continue;

        LOG("line %d: id=0x%llx time=%lld", line_num, (unsigned long long)id, (long long)unix_time);

        if (real_sceRtcSetCurrentTick) {
            RtcTick tick = { .tick = ((u64)unix_time + 62135596800ULL) * 1000000ULL };
            real_sceRtcSetCurrentTick(&tick);
        }

        SceNpTrophyId plat = 0;
        s32 r = real_sceNpTrophyUnlockTrophy ? real_sceNpTrophyUnlockTrophy(g_ctx, g_handle, id, &plat) : -1;

        if (r < 0) {
            char err[256] = {0};
            if (real_sceNpTrophyGetErrorString) real_sceNpTrophyGetErrorString(r, err, sizeof(err));
            LOG("FAIL id=0x%llx ret=0x%x err=%s", (unsigned long long)id, r, err);
        } else {
            LOG("OK id=0x%llx plat=0x%llx", (unsigned long long)id, (unsigned long long)plat);
        }
        processed++;
    }
    fclose(f);
    rename(LIST_PATH, DONE_PATH);
    notify("processed %d trophies", processed);
    LOG("process_list: DONE, processed=%d", processed);
}

// ===== RESOLVE SYMBOLS =====
static void resolve_symbols(void) {
    if (g_symbols_resolved) return;
    g_symbols_resolved = 1;
    LOG("resolve_symbols: START");

    void *np_mod = dlopen("libSceNpTrophy.sprx", RTLD_NOW);
    void *rtc_mod = dlopen("libSceRtc.sprx", RTLD_NOW);
    void *kernel_mod = dlopen("libSceKernel.sprx", RTLD_NOW);

    if (np_mod) {
        real_sceNpTrophyInit = (void*)dlsym(np_mod, "sceNpTrophyInit"); LOG("sceNpTrophyInit=%p", real_sceNpTrophyInit);
        real_sceNpTrophyCreateContext = (void*)dlsym(np_mod, "sceNpTrophyCreateContext"); LOG("CreateContext=%p", real_sceNpTrophyCreateContext);
        real_sceNpTrophyCreateHandle = (void*)dlsym(np_mod, "sceNpTrophyCreateHandle"); LOG("CreateHandle=%p", real_sceNpTrophyCreateHandle);
        real_sceNpTrophyRegisterContext = (void*)dlsym(np_mod, "sceNpTrophyRegisterContext"); LOG("RegisterContext=%p", real_sceNpTrophyRegisterContext);
        real_sceNpTrophyUnlockTrophy = (void*)dlsym(np_mod, "sceNpTrophyUnlockTrophy"); LOG("UnlockTrophy=%p", real_sceNpTrophyUnlockTrophy);
        real_sceNpTrophyGetErrorString = (void*)dlsym(np_mod, "sceNpTrophyGetErrorString"); LOG("GetErrorString=%p", real_sceNpTrophyGetErrorString);
    }
    if (rtc_mod) {
        real_sceRtcGetCurrentTick = (void*)dlsym(rtc_mod, "sceRtcGetCurrentTick"); LOG("RtcGetCurrentTick=%p", real_sceRtcGetCurrentTick);
        real_sceRtcSetCurrentTick = (void*)dlsym(rtc_mod, "sceRtcSetCurrentTick"); LOG("RtcSetCurrentTick=%p", real_sceRtcSetCurrentTick);
    }
    if (kernel_mod) {
        real_sceKernelGetProcessTitleId = (void*)dlsym(kernel_mod, "sceKernelGetProcessTitleId"); LOG("GetProcessTitleId=%p", real_sceKernelGetProcessTitleId);
    }
    LOG("resolve_symbols: DONE");
    notify("symbols resolved");
}

// ===== INSTALL HOOKS =====
static void install_hooks(void) {
    if (g_hooks_installed) return;
    g_hooks_installed = 1;
    notify("installing hooks...");
    HOOK32(sceNpTrophyCreateContext); LOG("HOOK CreateContext");
    HOOK32(sceNpTrophyCreateHandle); LOG("HOOK CreateHandle");
    HOOK32(sceNpTrophyRegisterContext); LOG("HOOK RegisterContext");
    HOOK32(sceNpTrophyInit); LOG("HOOK Init");
    notify("hooks installed");
}

// ===== HOOKS =====
s32 sceNpTrophyInit_hook(const char *commId, const char *svcLabel) {
    LOG("sceNpTrophyInit_hook: commId=%s svcLabel=%s", commId ? commId : "NULL", svcLabel ? svcLabel : "NULL");
    s32 r = real_sceNpTrophyInit ? real_sceNpTrophyInit(commId, svcLabel) : -1;
    LOG("  -> ret=%d", r);
    if (r >= 0 && !g_init_done) {
        g_init_done = 1;
        try_process_list();
    }
    return r;
}

s32 sceNpTrophyCreateContext_hook(SceNpTrophyContext *ctx, s32 userId, u32 label, u64 opt) {
    s32 r = real_sceNpTrophyCreateContext ? real_sceNpTrophyCreateContext(ctx, userId, label, opt) : -1;
    if (r >= 0 && ctx) { g_ctx = *ctx; LOG("CreateContext: ctx=%d", g_ctx); }
    if (label != 0) {
        char npwr[32];
        snprintf(npwr, sizeof(npwr), "NPWR%05x_00", label & 0xFFFFF);
        if (strstr(g_current_npwr, "NPWR09798_00") && strcmp(npwr, g_current_npwr) != 0) {
            strncpy(g_current_npwr, npwr, sizeof(g_current_npwr)-1);
            LOG("Uncharted sub-game: %s", npwr);
        }
    }
    try_process_list();
    return r;
}

s32 sceNpTrophyCreateHandle_hook(SceNpTrophyHandle *h) {
    s32 r = real_sceNpTrophyCreateHandle ? real_sceNpTrophyCreateHandle(h) : -1;
    if (r >= 0 && h) { g_handle = *h; LOG("CreateHandle: handle=%d", g_handle); }
    try_process_list();
    return r;
}

s32 sceNpTrophyRegisterContext_hook(SceNpTrophyContext ctx, SceNpTrophyHandle h, u64 opt) {
    LOG("RegisterContext_hook: ctx=%d handle=%d", ctx, h);
    if (!g_hooks_installed) {
        resolve_symbols();
        install_hooks();
    }
    if (!g_init_done) {
        char titleId[16] = {0};
        if (real_sceKernelGetProcessTitleId) real_sceKernelGetProcessTitleId(titleId, sizeof(titleId));
        const char *npwr = resolve_npwr(titleId);
        if (npwr) {
            strncpy(g_current_npwr, npwr, sizeof(g_current_npwr)-1);
            if (real_sceNpTrophyInit) real_sceNpTrophyInit(npwr, npwr);
            LOG("Game: %s -> %s", titleId, npwr);
            notify("%s -> %s", titleId, npwr);
        }
        g_init_done = 1;
    }
    g_ctx = ctx; g_handle = h;
    try_process_list();
    return real_sceNpTrophyRegisterContext ? real_sceNpTrophyRegisterContext(ctx, h, opt) : -1;
}

// ===== PLUGIN ENTRY POINTS =====
s32 attr_public plugin_load(s32 argc, const char *argv[]) {
    // IMMEDIATE LOG
    FILE *f = fopen(LOG_PATH, "a");
    if (f) { fprintf(f, "\n=== [trophy_restore v1.09] plugin_load ===\n"); fclose(f); }
    
    notify("v1.09 loaded - FORCING INIT");
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    boot_ver();

    // FORCE RESOLVE + HOOKS IMMEDIATELY
    resolve_symbols();
    install_hooks();
    
    // Try to get title ID now
    char titleId[16] = {0};
    if (real_sceKernelGetProcessTitleId) {
        real_sceKernelGetProcessTitleId(titleId, sizeof(titleId));
        LOG("plugin_load: titleId=%s", titleId);
        const char *npwr = resolve_npwr(titleId);
        if (npwr) {
            strncpy(g_current_npwr, npwr, sizeof(g_current_npwr)-1);
            if (real_sceNpTrophyInit) real_sceNpTrophyInit(npwr, npwr);
            LOG("Pre-detected: %s -> %s", titleId, npwr);
        }
    }
    
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    if (g_hooks_installed) {
        UNHOOK(sceNpTrophyCreateContext);
        UNHOOK(sceNpTrophyCreateHandle);
        UNHOOK(sceNpTrophyRegisterContext);
        UNHOOK(sceNpTrophyInit);
    }
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void *args) { return 0; }
