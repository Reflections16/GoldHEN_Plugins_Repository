static void load_config(void) {
    char buf[16] = {0};
    s32 fd = sceKernelOpen(CONFIG_PATH, 0 /* O_RDONLY */, 0);
    if (fd < 0) return; // файлу немає, лишається дефолт
    s32 n = sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    for (s32 i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ') { buf[i] = 0; break; }
    }
    // 1) шукаємо за кодом мови (ru, en, de ...)
    for (u32 i = 0; i < LANG_COUNT; i++) {
        if (strcmp(buf, LANGS[i].code) == 0) {
            g_targetLang = LANGS[i].id;
            strncpy(g_langCode, buf, sizeof(g_langCode) - 1);
            return;
        }
    }
    // 2) якщо там число, беремо його як ID напряму
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
