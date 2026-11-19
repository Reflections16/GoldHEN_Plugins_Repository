#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

#include <Common.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "trophy_restore";
attr_public const char *g_pluginDesc = "Unlock trophies from list with custom time (multi-game)";
attr_public const char *g_pluginAuth = "you";
attr_public u32 g_pluginVersion = 0x00000103;

#define LIST_PATH "/data/trophy_unlock.txt"
#define DONE_PATH "/data/trophy_unlock.done"
#define LOG_PATH  "/data/trophy_restore_log.txt"

typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef u64 SceNpTrophyId;
typedef struct { s64 tv_sec; s64 tv_usec; } KTimeval;
typedef struct { u64 tick; } RtcTick;

// ===== Function pointers (resolved via dlsym) =====
static s32 (*g_sceNpTrophyInit)(const char *, const char *) = NULL;
static s32 (*g_sceNpTrophyCreateContext)(SceNpTrophyContext *, s32, u32, u64) = NULL;
static s32 (*g_sceNpTrophyCreateHandle)(SceNpTrophyHandle *) = NULL;
static s32 (*g_sceNpTrophyRegisterContext)(SceNpTrophyContext, SceNpTrophyHandle, u64) = NULL;
static s32 (*g_sceNpTrophyUnlockTrophy)(SceNpTrophyContext, SceNpTrophyHandle, SceNpTrophyId, SceNpTrophyId *) = NULL;
static s32 (*g_sceNpTrophyGetTrophyList)(SceNpTrophyContext, SceNpTrophyHandle, SceNpTrophyId *, u32 *, u64) = NULL;
static s32 (*g_sceNpTrophyGetErrorString)(s32, char *, size_t) = NULL;
static s32 (*g_sceRtcGetCurrentTick)(RtcTick *) = NULL;
static s32 (*g_sceRtcSetCurrentTick)(const RtcTick *) = NULL;
static s32 (*g_sceKernelGetProcessTitleId)(char *, size_t) = NULL;
static s32 (*g_sceKernelGettimeofday)(KTimeval *) = NULL;

static SceNpTrophyContext g_ctx = -1;
static SceNpTrophyHandle g_handle = -1;
static volatile int g_done = 0;
static char g_current_npwr[32] = {0};

HOOK_INIT(sceNpTrophyCreateContext);
HOOK_INIT(sceNpTrophyCreateHandle);
HOOK_INIT(sceNpTrophyRegisterContext);
HOOK_INIT(sceNpTrophyGetTrophyList);

// ===== МАПІНГ CUSA → NPWR (всі 11 ігор) =====
typedef struct { const char *cusa; const char *npwr; } GameMap;

static const GameMap GAME_MAP[] = {
    {"CUSA00028", "NPWR05236_00"},   // Call of Duty: Ghosts
    {"CUSA01395", "NPWR05212_00"},   // Watch Dogs: Complete Edition
    {"CUSA02344", "NPWR09798_00"},   // Uncharted Collection (base, буде виправлено по label)
    {"CUSA02357", "NPWR09525_00"},   // Heavy Rain
    {"CUSA05380", "NPWR11068_00"},   // CoD: Modern Warfare Remastered
    {"CUSA08308", "NPWR13872_00"},   // Detroit: Become Human
    {"CUSA10249", "NPWR13348_00"},   // The Last of Us Part II
    {"CUSA10960", "NPWR15392_00"},   // Hello Neighbor
    {"CUSA11995", "NPWR09167_00"},   // Marvel's Spider-Man
    {"CUSA36843", "NPWR27622_00"},   // Red Dead Redemption (2023)
    {NULL, NULL}
};

static const char *resolve_npwr(const char *cusa) {
    for (int i = 0; GAME_MAP[i].cusa; i++) {
        if (strcmp(cusa, GAME_MAP[i].cusa) == 0) {
            return GAME_MAP[i].npwr;
        }
    }
    return NULL;
}

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static s64 parse_time(const char *s) {
    int y, mo, d, h, mi;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int leap = (y%4==0 && (y%100!=0 || y%400==0));
    int doy = mdays[mo-1] + d - 1 + (mo>2 && leap);
    s64 days = (s64)(y-1970)*365 + (y-1969)/4 - (y-1901)/100 + (y-1601)/400 + doy;
    return days*86400 + h*3600 + mi*60;
}

static void resolve_symbols(void) {
    void *np_mod = dlopen("libSceNpTrophy.sprx", RTLD_NOW);
    void *rtc_mod = dlopen("libSceRtc.sprx", RTLD_NOW);
    void *kernel_mod = dlopen("libSceKernel.sprx", RTLD_NOW);
    
    if (np_mod) {
        g_sceNpTrophyInit = (void*)dlsym(np_mod, "sceNpTrophyInit");
        g_sceNpTrophyCreateContext = (void*)dlsym(np_mod, "sceNpTrophyCreateContext");
        g_sceNpTrophyCreateHandle = (void*)dlsym(np_mod, "sceNpTrophyCreateHandle");
        g_sceNpTrophyRegisterContext = (void*)dlsym(np_mod, "sceNpTrophyRegisterContext");
        g_sceNpTrophyUnlockTrophy = (void*)dlsym(np_mod, "sceNpTrophyUnlockTrophy");
        g_sceNpTrophyGetTrophyList = (void*)dlsym(np_mod, "sceNpTrophyGetTrophyList");
        g_sceNpTrophyGetErrorString = (void*)dlsym(np_mod, "sceNpTrophyGetErrorString");
    }
    if (rtc_mod) {
        g_sceRtcGetCurrentTick = (void*)dlsym(rtc_mod, "sceRtcGetCurrentTick");
        g_sceRtcSetCurrentTick = (void*)dlsym(rtc_mod, "sceRtcSetCurrentTick");
    }
    if (kernel_mod) {
        g_sceKernelGetProcessTitleId = (void*)dlsym(kernel_mod, "sceKernelGetProcessTitleId");
        g_sceKernelGettimeofday = (void*)dlsym(kernel_mod, "sceKernelGettimeofday");
    }
    
    logf_("Symbols resolved: np_mod=%p rtc_mod=%p kernel_mod=%p\n", np_mod, rtc_mod, kernel_mod);
}

static void process_list(void) {
    if (!g_current_npwr[0]) {
        logf_("ERROR: NPWR not resolved\n");
        return;
    }
    logf_("Processing list for %s\n", g_current_npwr);

    FILE *f = fopen(LIST_PATH, "r");
    if (!f) { logf_("no list file\n"); return; }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
        if (!strncmp(line, "tz=", 3)) continue;

        char id_str[64], time_str[64];
        if (sscanf(line, "%63[^;];%63s", id_str, time_str) != 2) continue;

        SceNpTrophyId id = 0;
        if (id_str[0]=='0' && (id_str[1]=='x' || id_str[1]=='X')) {
            sscanf(id_str, "%llx", (unsigned long long*)&id);
        } else {
            logf_("SKIP non-hex id: %s\n", id_str);
            continue;
        }

        s64 unix_time = parse_time(time_str);
        if (!unix_time) { logf_("BAD time: %s\n", time_str); continue; }

        if (g_sceRtcSetCurrentTick) {
            RtcTick tick = { .tick = ((u64)unix_time + 62135596800ULL) * 1000000ULL };
            g_sceRtcSetCurrentTick(&tick);
        }

        SceNpTrophyId plat = 0;
        s32 r = 0;
        if (g_sceNpTrophyUnlockTrophy) {
            r = g_sceNpTrophyUnlockTrophy(g_ctx, g_handle, id, &plat);
        } else {
            r = -1;
        }

        if (r < 0) {
            char err[256] = {0};
            if (g_sceNpTrophyGetErrorString) {
                g_sceNpTrophyGetErrorString(r, err, sizeof(err));
            }
            logf_("FAIL id=0x%llx ret=0x%x err=%s\n", (unsigned long long)id, r, err);
        } else {
            logf_("OK   id=0x%llx time=%lld\n", (unsigned long long)id, (long long)unix_time);
        }
    }
    fclose(f);
    rename(LIST_PATH, DONE_PATH);
}

s32 sceNpTrophyCreateContext_hook(SceNpTrophyContext *ctx, s32 userId, u32 label, u64 opt) {
    s32 r = 0;
    if (g_sceNpTrophyCreateContext) {
        r = g_sceNpTrophyCreateContext(ctx, userId, label, opt);
    }
    if (r >= 0 && ctx) g_ctx = *ctx;
    
    // Логуємо label для Uncharted (виявлення правильного NPWR)
    if (label != 0) {
        char npwr[32];
        snprintf(npwr, sizeof(npwr), "NPWR%05x_00", label & 0xFFFFF);
        logf_("sceNpTrophyCreateContext: label=0x%x -> %s\n", label, npwr);
        
        // Якщо це Uncharted і NPWR змінився — оновлюємо
        if (strstr(g_current_npwr, "NPWR09798_00") && strcmp(npwr, g_current_npwr) != 0) {
            strncpy(g_current_npwr, npwr, sizeof(g_current_npwr)-1);
            logf_("Uncharted sub-game detected: %s\n", npwr);
        }
    }
    return r;
}

s32 sceNpTrophyCreateHandle_hook(SceNpTrophyHandle *h) {
    s32 r = 0;
    if (g_sceNpTrophyCreateHandle) {
        r = g_sceNpTrophyCreateHandle(h);
    }
    if (r >= 0 && h) g_handle = *h;
    return r;
}

s32 sceNpTrophyGetTrophyList_hook(SceNpTrophyContext ctx, SceNpTrophyHandle h,
                                  SceNpTrophyId *ids, u32 *count, u64 opt) {
    s32 r = 0;
    if (g_sceNpTrophyGetTrophyList) {
        r = g_sceNpTrophyGetTrophyList(ctx, h, ids, count, opt);
    }
    if (r >= 0 && ids && *count > 0) {
        logf_("=== REAL TROPHY IDs for %s (count=%u) ===\n", g_current_npwr[0] ? g_current_npwr : "unknown", *count);
        for (u32 i = 0; i < *count; i++) {
            logf_("  [%u] 0x%llx\n", i, (unsigned long long)ids[i]);
        }
        logf_("=== END LIST ===\n");
    }
    return r;
}

s32 sceNpTrophyRegisterContext_hook(SceNpTrophyContext ctx, SceNpTrophyHandle h, u64 opt) {
    static int inited = 0;
    if (!inited) {
        resolve_symbols();
        
        char titleId[16] = {0};
        if (g_sceKernelGetProcessTitleId) {
            g_sceKernelGetProcessTitleId(titleId, sizeof(titleId));
        }
        const char *npwr = resolve_npwr(titleId);
        if (npwr) {
            strncpy(g_current_npwr, npwr, sizeof(g_current_npwr)-1);
            if (g_sceNpTrophyInit) {
                g_sceNpTrophyInit(npwr, npwr);
            }
            logf_("Game detected: %s -> %s\n", titleId, npwr);
        } else {
            logf_("WARNING: Unknown Title ID: %s\n", titleId);
        }
        inited = 1;
    }
    s32 r = 0;
    if (g_sceNpTrophyRegisterContext) {
        r = g_sceNpTrophyRegisterContext(ctx, h, opt);
    }
    g_ctx = ctx;
    g_handle = h;
    if (r >= 0 && !g_done) { g_done = 1; process_list(); }
    return r;
}

s32 attr_public plugin_load(s32 argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    boot_ver();
    HOOK32(sceNpTrophyCreateContext);
    HOOK32(sceNpTrophyCreateHandle);
    HOOK32(sceNpTrophyRegisterContext);
    HOOK32(sceNpTrophyGetTrophyList);
    logf_("trophy_restore v1.03 loaded (multi-game, all 11 games, dlsym)\n");
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    UNHOOK(sceNpTrophyCreateContext);
    UNHOOK(sceNpTrophyCreateHandle);
    UNHOOK(sceNpTrophyRegisterContext);
    UNHOOK(sceNpTrophyGetTrophyList);
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void *args) { return 0; }
