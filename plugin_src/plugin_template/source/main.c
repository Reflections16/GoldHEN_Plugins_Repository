// LangSpoof: підміняє мову системи для гри через sceSystemServiceParamGetInt.
// Мова береться з /data/GoldHEN/langspoof.txt (наприклад: ru, en, de або число-ID).

#include <Common.h>
#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "lang_spoof";
attr_public const char *g_pluginDesc = "Spoof system language for games";
attr_public const char *g_pluginAuth = "me";
attr_public u32 g_pluginVersion = 0x00000100; // 1.00

#define PARAM_ID_LANG 1
#define CONFIG_PATH "/data/GoldHEN/langspoof.txt"

typedef struct { const char *code; s32 id; } LangEntry;

// ID за SCE_SYSTEM_PARAM_LANG_*. 0-15 підтверджені, далі з пам'яті.
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

static s32 g_targetLang = 8;        // за замовчуванням російська
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
    s32 fd = sceKernelOpen(CONFIG_PATH, 0 /* O_RDONLY */, 0);
    if (fd < 0) return; // файлу немає, лишається
