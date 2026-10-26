// LangSpoof: spoofs system language for games via sceSystemServiceParamGetInt.
// Language is read from /data/GoldHEN/langspoof.txt (e.g. ru, en, de or a number).

#include <Common.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "lang_spoof";
attr_public const char *g_pluginDesc = "Spoof system language for games";
attr_public const char *g_pluginAuth = "me";
attr_public u32 g_pluginVersion = 0x00000100;

#define PARAM_ID_LANG 1
#define CONFIG_PATH "/data/GoldHEN/langspoof.txt"

typedef struct { const char *code; s32 id; } LangEntry;

// IDs 0-15 confirmed, the rest from memory
static const LangEntry LANGS[] = {
    {"ja", 0},  {"en", 1},  {"fr", 2},  {"es", 3},
    {"de", 4},  {"it", 5},  {"nl", 6},  {"pt", 7},
    {"ru", 8},  {"ko", 9},  {"zh-t", 10}, {"zh-s", 11},
    {"fi", 12}, {"sv", 13}, {"da", 14}, {"no", 15},
    {"pl", 16}, {"pt-br", 17}, {"en-gb", 18}, {"tr", 19},
    {"es-la", 20}, {"ar", 21}, {"fr-ca", 22}, {"cs", 23},
    {"hu", 24}, {"el", 25}, {"ro", 26}, {"th", 27},
    {"vi", 28}, {"id", 29}, {"uk", 30},
};
#define LANG_COUNT (sizeof(LANGS) / sizeof(LANGS[0]))

static s32 g_targetLang = 8;
static char g_langCode[16] = "ru";

extern s32 sceSystemServiceParamGetInt(s32 paramId, s32 *value);

HOOK_INIT(sceSystemServiceParamGetInt);

s32 sceSystemServiceParamGetInt_hook(s32 paramId, s32 *value) {
    s32 ret = HOOK_CONTINUE(sceSystemServiceParamGetInt,
                            s32 (*)(s32, s32 *),
                            paramId, value);
    if (ret == 0 && paramId == PARAM_ID_LANG && value) {
        *value = g_targetLang;
    }
    return ret;
}

static void load_config(void) {
    char buf[16] = {0};
    s32 fd = sceKernelOpen(CONFIG_PATH, 0, 0);
    if (fd < 0) return;
    s32 n = sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    for (s32 i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ') { buf[i] = 0; break; }
    }
    for (u32 i = 0; i < LANG_COUNT; i++) {
        if (strcmp(buf, LANGS[i].code) == 0) {
            g_targetLang = LANGS[i].id;
            strncpy(g_langCode, buf, sizeof(g_langCode) - 1);
            return;
        }
    }
    s32 num = 0, ok = 0;
    for (s32 i = 0; buf[i]; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') { num = num * 10 + (buf[i] - '0'); ok = 1; }
        else { ok = 0; break; }
    }
    if (ok) {
        g_targetLang = num;
        strncpy(g_langCode, buf, sizeof(g_langCode) - 1);
    }
}

s32 attr_public plugin_load(s32 argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    final_printf("[GoldHEN] Plugin Author(s): %s\n", g_pluginAuth);
    boot_ver();
    load_config();
    HOOK32(sceSystemServiceParamGetInt);
    char msg[64] = {0};
    snprintf(msg, sizeof(msg), "LangSpoof: %s (id %d)", g_langCode, g_targetLang);
    NotifyStatic(TEX_ICON_SYSTEM, msg);
    return 0;
}

s32 attr_public plugin_unload(s32 argc, const char *argv[]) {
    final_printf("[GoldHEN] <%s\\Ver.0x%08x> %s\n", g_pluginName, g_pluginVersion, __func__);
    UNHOOK(sceSystemServiceParamGetInt);
    return 0;
}

s32 attr_module_hidden module_start(s64 argc, const void *args) { return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void *args) { return 0; }
