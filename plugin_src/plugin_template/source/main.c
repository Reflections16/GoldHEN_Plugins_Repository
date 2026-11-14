// trophy_restore.c
// Розблоковує трофеї зі списку /data/trophy_unlock.txt, підміняючи час,
// який бачить процес гри, на вказаний у списку.
// ЧЕРНЕТКА: не компілювалась і не тестувалась.

#include <Common.h>
#include <GoldHEN.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

attr_public const char *g_pluginName = "trophy_restore";
attr_public const char *g_pluginDesc = "Unlock trophies from list with custom time";
attr_public uint32_t g_pluginVersion = 0x00000100;

#define LIST_PATH "/data/trophy_unlock.txt"
#define DONE_PATH "/data/trophy_unlock.done"
#define LOG_PATH  "/data/trophy_restore_log.txt"

typedef int32_t SceNpTrophyContext;
typedef int32_t SceNpTrophyHandle;
typedef int32_t SceNpTrophyId;
extern int sceNpTrophyUnlockTrophy(SceNpTrophyContext ctx, SceNpTrophyHandle h,
                                   SceNpTrophyId id, SceNpTrophyId *platinumId);

typedef struct { int64_t tv_sec; int64_t tv_usec; } KTimeval;
typedef struct { uint64_t tick; } RtcTick;

static SceNpTrophyContext g_ctx = -1;
static SceNpTrophyHandle  g_handle = -1;
static volatile int g_fake_on = 0;
static int64_t g_fake_unix = 0;
static int g_done = 0;

HOOK_INIT(sceNpTrophyCreateContext);
HOOK_INIT(sceNpTrophyCreateHandle);
HOOK_INIT(sceNpTrophyRegisterContext);
HOOK_INIT(sceKernelGettimeofday);
HOOK_INIT(sceRtcGetCurrentTick);

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fclose(f);
}

static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t last_sunday_0100utc(int year, int month) {
    int64_t d = days_from_civil(year, month, 31);
    int wd = (int)(((d + 4) % 7 + 7) % 7);
    return (d - wd) * 86400 + 3600;
}

static int is_kyiv_dst(int year, int64_t t_utc) {
    return t_utc >= last_sunday_0100utc(year, 3) && t_utc < last_sunday_0100utc(year, 10);
}

static int64_t to_unix(int y, int mo, int d, int h, int mi, int kyiv) {
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
    if (!kyiv) return t;
    int off = is_kyiv_dst(y, t - 2 * 3600) ? 3 : 2;
    return t - off * 3600;
}

int sceKernelGettimeofday_hook(KTimeval *tp) {
    int r = HOOK_CONTINUE(sceKernelGettimeofday, int (*)(KTimeval *), tp);
    if (g_fake_on && tp) { tp->tv_sec = g_fake_unix; }
    return r;
}

int sceRtcGetCurrentTick_hook(RtcTick *t) {
    int r = HOOK_CONTINUE(sceRtcGetCurrentTick, int (*)(RtcTick *), t);
    if (g_fake_on && t) {
        t->tick = ((uint64_t)g_fake_unix + 62135596800ULL) * 1000000ULL;
    }
    return r;
}

int sceNpTrophyCreateContext_hook(SceNpTrophyContext *ctx, int userId, uint32_t label, uint64_t opt) {
    int r = HOOK_CONTINUE(sceNpTrophyCreateContext,
                          int (*)(SceNpTrophyContext *, int, uint32_t, uint64_t),
                          ctx, userId, label, opt);
    if (r >= 0 && ctx) g_ctx = *ctx;
    return r;
}

int sceNpTrophyCreateHandle_hook(SceNpTrophyHandle *h) {
    int r = HOOK_CONTINUE(sceNpTrophyCreateHandle, int (*)(SceNpTrophyHandle *), h);
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
            g_fake_unix = to_unix(y, mo, d, h, mi, kyiv) + (int64_t)(id - id_from) * 60;
            g_fake_on = 1;
            SceNpTrophyId plat = -1;
            int r = sceNpTrophyUnlockTrophy(g_ctx, g_handle, id, &plat);
            g_fake_on = 0;
            logf_("unlock id=%d time=%lld ret=0x%x\n", id, (long long)g_fake_unix, r);
        }
    }
    fclose(f);
    rename(LIST_PATH, DONE_PATH);
}

int sceNpTrophyRegisterContext_hook(SceNpTrophyContext ctx, SceNpTrophyHandle h, uint64_t opt) {
    int r = HOOK_CONTINUE(sceNpTrophyRegisterContext,
                          int (*)(SceNpTrophyContext, SceNpTrophyHandle, uint64_t),
                          ctx, h, opt);
    g_ctx = ctx; g_handle = h;
    if (r >= 0 && !g_done) { g_done = 1; process_list(); }
    return r;
}

attr_module_hidden int module_start(size_t argc, const void *args) {
    HOOK32(sceNpTrophyCreateContext);
    HOOK32(sceNpTrophyCreateHandle);
    HOOK32(sceNpTrophyRegisterContext);
    HOOK32(sceKernelGettimeofday);
    HOOK32(sceRtcGetCurrentTick);
    logf_("trophy_restore loaded\n");
    return 0;
}

attr_module_hidden int module_stop(size_t argc, const void *args) {
    UNHOOK(sceNpTrophyCreateContext);
    UNHOOK(sceNpTrophyCreateHandle);
    UNHOOK(sceNpTrophyRegisterContext);
    UNHOOK(sceKernelGettimeofday);
    UNHOOK(sceRtcGetCurrentTick);
    return 0;
}
