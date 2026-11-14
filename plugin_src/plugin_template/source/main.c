// trophy_restore: unlocks trophies from /data/trophy_unlock.txt while
// faking the time seen by the game process. EXPERIMENTAL.

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <Common.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "trophy_restore";
attr_public const char *g_pluginDesc = "Unlock trophies from list with custom time";
attr_public const char *g_pluginAuth = "you";
attr_public u32 g_pluginVersion = 0x00000100; // 1.00

#define LIST_PATH "/data/trophy_unlock.txt"
#define DONE_PATH "/data/trophy_unlock.done"
#define LOG_PATH  "/data/trophy_restore_log.txt"

typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef s32 SceNpTrophyId;
typedef struct { s64 tv_sec; s64 tv_usec; } KTimeval;
typedef struct { u64 tick; } RtcTick;

s32 sceNpTrophyCreateContext(SceNpTrophyContext *ctx, s32 userId, u32 label, u64 opt);
s32 sceNpTrophyCreateHandle(SceNpTrophyHandle *h);
s32 sceNpTrophyRegisterContext(SceNpTrophyContext ctx, SceNpTrophyHandle h, u64 opt);
s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext ctx, SceNpTrophyHandle h,
                            SceNpTrophyId id, SceNpTrophyId *platinumId);
s32 sceRtcGetCurrentTick(RtcTick *t);

static SceNpTrophyContext g_ctx = -1;
static SceNpTrophyHandle g_handle = -1;
static volatile int g_fake_on = 0;
static s64 g_fake_unix = 0;
static int g_done = 0;

HOOK_INIT(sceNpTrophyCreateContext);
HOOK_INIT(sceNpTrophyCreateHandle);
HOOK_INIT(sceNpTrophyRegisterContext);
HOOK_INIT(sceKernelGettimeofday);
HOOK_INIT(sceRtcGetCurrentTick);

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static s64 days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (s64)doe - 719468;
}

static s64 last_sunday_0100utc(int year, int month) {
    s64 d = days_from_civil(year, month, 31);
    int wd = (int)(((d + 4) % 7 + 7) % 7); // 0 = Sunday
    return (d - wd) * 86400 + 3600;
}

static int is_kyiv_dst(int year, s64 t_utc) {
    return t_utc >= last_sunday_0100utc(year, 3) && t_utc < last_sunday_0100utc(year, 10);
}

static s64 to_unix(int y, int mo, int d, int h, int mi, int kyiv) {
    s64 t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
    if (!kyiv) return t;
    int off = is_kyiv_dst(y, t - 2 * 3600) ? 3 : 2; // summer +3, winter +2
    return t - off * 3600;
}

s32 sceKernelGettimeofday_hook(KTimeval *tp) {
    s32 r = HOOK_CONTINUE(sceKernelGettimeofday, s32 (*)(KTimeval *), tp);
    if (g_fake_on && tp) tp->tv_sec = g_fake_unix;
    return r;
}

s32 sceRtcGetCurrentTick_hook(RtcTick *t) {
    s32 r = HOOK_CONTINUE(sceRtcGetCurrentTick, s32 (*)(RtcTick *), t);
    if (g_fake_on && t) t->tick = ((u64)g_fake_unix + 62135596800ULL) * 1000000ULL;
    return r;
}

s32 sceNpTrophyCreateContext_hook(SceNpTrophyContext *ctx, s32 userId, u32 label, u64 opt) {
    s32 r = HOOK_CONTINUE(sceNpTrophyCreateContext,
                          s32 (*)(SceNpTrophyContext *, s32, u32, u64), ctx, userId, label, opt);
    if (r >= 0 && ctx) g_ctx = *ctx;
    return r;
}

s32 sceNpTrophyCreateHandle_hook(SceNpTrophyHandle *h) {
    s32 r = HOOK_CONTINUE(sceNpTrophyCreateHandle, s32 (*)(SceNpTrophyHandle *), h);
    if (r >= 0 && h) g_handle = *h;
    return r;
}

static void process_list(void) {
    FILE *f = fopen(LIST_PATH, "r");
    if (!f) { logf_("no list file\n"); return; }
    char line[128];
    int kyiv = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "tz=", 3)) { kyiv = !strncmp(line + 3, "KYIV", 4); continue; }
        int id_from, id_to, y, mo, d, h, mi;
        int n = sscanf(line, "%d-%d;%d-%d-%d %d:%d", &id_from, &id_to, &y, &mo, &d, &h, &mi);
        if (n != 7) {
            n = sscanf(line, "%d;%d-%d-%d %d:%d", &id_from, &y, &mo, &d, &h, &mi);
            if (n != 6) continue;
            id_to = id_from;
        }
        for (int id = id_from; id <= id_to; id++) {
            g_fake_unix = to_unix(y, mo, d, h, mi, kyiv) + (s64)(id - id_from) * 60;
            g_fake_on = 1;
            SceNpTrophyId plat = -1;
            s32 r = sceNpTrophyUnlockTrophy(g_ctx, g_handle, id, &plat);
            g_fake_on = 0;
            logf_("unlock id=%d time=%lld ret=0x%x\n", id, (long long)g_fake_unix, r);
        }
    }
    fclose(f);
    rename(LIST_PATH, DONE_PATH);
}

s32 sceNpTrophyRegisterContext_hook(SceNpTrophyContext ctx, SceNpTrophyHandle h, u64 opt) {
    s32 r = HOOK_CONTINUE(sceNpTrophyRegisterContext,
                          s32 (*)(SceNpTrophyContext, SceNpTrophyHandle, u64), ctx, h, opt);
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
    HOOK32(sceKernelGettimeofday);
    HOOK32(sceRtcGetCurrentTick);
    logf_("trophy_restore loaded\n");
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    UNHOOK(sceNpTrophyCreateContext);
    UNHOOK(sceNpTrophyCreateHandle);
    UNHOOK(sceNpTrophyRegisterContext);
    UNHOOK(sceKernelGettimeofday);
    UNHOOK(sceRtcGetCurrentTick);
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void *args) { return 0; }
