// Claude NX — простой чат-клиент Claude (Anthropic API) для Nintendo Switch (homebrew, libnx + SDL2)
//
// Управление:
//   A        — написать сообщение (экранная клавиатура)
//   X        — новый чат (очистить историю)
//   Y        — ввести / сменить API-ключ
//   Вверх/Вниз, стики, касание — прокрутка;  L/R — прокрутка на страницу
//   +        — выход
//
// Настройки: sdmc:/switch/claude/config.txt

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>

#include "cJSON.h"

// --------------------------------------------------------------------------
// Журнал запуска: sdmc:/switch/claude/log.txt (каждая строка сразу сохраняется на карту)
// --------------------------------------------------------------------------
#include <stdarg.h>
static void logf_(const char *fmt, ...) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/claude", 0777);
    FILE *f = fopen("sdmc:/switch/claude/log.txt", "a");
    if (!f) return;
    u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    fprintf(f, "[%llu ms] ", (unsigned long long)ms);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}
#define LOG(...) logf_(__VA_ARGS__)

#define SCREEN_W 1280
#define SCREEN_H 720

#define APP_DIR      "sdmc:/switch/claude"
#define CONFIG_PATH  APP_DIR "/config.txt"
#define HISTORY_PATH APP_DIR "/history.json"
#define FONT_OVERRIDE_PATH APP_DIR "/font.ttf"

#define API_URL "https://api.anthropic.com/v1/messages"

#define DEFAULT_MODEL "claude-sonnet-5-5"
#define DEFAULT_MAX_TOKENS 2048
#define DEFAULT_SYSTEM \
    "Ты общаешься с пользователем через приложение на Nintendo Switch. " \
    "Экран небольшой, разметка Markdown не отображается: отвечай обычным текстом, " \
    "без Markdown, таблиц и эмодзи. Отвечай на языке пользователя."

#define HEADER_H 56
#define FOOTER_H 44
#define MARGIN_X 40
#define CONTENT_TOP (HEADER_H + 10)
#define CONTENT_BOTTOM (SCREEN_H - FOOTER_H - 10)
#define CONTENT_W (SCREEN_W - MARGIN_X * 2 - 20)

// --------------------------------------------------------------------------
// Настройки
// --------------------------------------------------------------------------

typedef struct {
    char api_key[512];
    char model[128];
    int  max_tokens;
    int  system_keyboard; // 1 = системная клавиатура Switch, 0 = встроенная
    char system[4096];
} Config;

static Config g_cfg;

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static void config_save(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# Настройки Claude для Switch\n");
    fprintf(f, "# API-ключ с https://console.anthropic.com (начинается с sk-ant-)\n");
    fprintf(f, "api_key=%s\n", g_cfg.api_key);
    fprintf(f, "# Модель: claude-sonnet-5-5, claude-opus-5-5, claude-haiku-5-5 ...\n");
    fprintf(f, "model=%s\n", g_cfg.model);
    fprintf(f, "max_tokens=%d\n", g_cfg.max_tokens);
    fprintf(f, "# 1 = системная клавиатура Switch (на некоторых прошивках зависает), 0 = встроенная\n");
    fprintf(f, "system_keyboard=%d\n", g_cfg.system_keyboard);
    fprintf(f, "# Системный промпт (одной строкой)\n");
    fprintf(f, "system=%s\n", g_cfg.system);
    fclose(f);
}

static void config_load(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.model, sizeof(g_cfg.model), "%s", DEFAULT_MODEL);
    g_cfg.max_tokens = DEFAULT_MAX_TOKENS;
    snprintf(g_cfg.system, sizeof(g_cfg.system), "%s", DEFAULT_SYSTEM);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) { config_save(); return; }
    char line[5000];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key); trim(val);
        if (!strcmp(key, "api_key")) snprintf(g_cfg.api_key, sizeof(g_cfg.api_key), "%s", val);
        else if (!strcmp(key, "model") && val[0]) snprintf(g_cfg.model, sizeof(g_cfg.model), "%s", val);
        else if (!strcmp(key, "max_tokens")) { int v = atoi(val); if (v > 0) g_cfg.max_tokens = v; }
        else if (!strcmp(key, "system_keyboard")) g_cfg.system_keyboard = atoi(val);
        else if (!strcmp(key, "system")) snprintf(g_cfg.system, sizeof(g_cfg.system), "%s", val);
    }
    fclose(f);
}

// --------------------------------------------------------------------------
// История сообщений
// --------------------------------------------------------------------------

typedef enum { ROLE_USER = 0, ROLE_ASSISTANT = 1, ROLE_INFO = 2, ROLE_ERROR = 3 } Role;

typedef struct {
    Role role;
    char *text;
    bool excluded; // не отправлять в API (например, сообщение, на которое пришла ошибка)
} Msg;

static Msg *g_msgs = NULL;
static int g_msg_count = 0, g_msg_cap = 0;

static void msg_add(Role role, const char *text) {
    if (g_msg_count == g_msg_cap) {
        g_msg_cap = g_msg_cap ? g_msg_cap * 2 : 32;
        g_msgs = realloc(g_msgs, sizeof(Msg) * g_msg_cap);
    }
    g_msgs[g_msg_count].role = role;
    g_msgs[g_msg_count].text = strdup(text ? text : "");
    g_msgs[g_msg_count].excluded = false;
    g_msg_count++;
}

static void msg_clear(void) {
    for (int i = 0; i < g_msg_count; i++) free(g_msgs[i].text);
    g_msg_count = 0;
}

static void history_save(void) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_msg_count; i++) {
        if (g_msgs[i].excluded) continue;
        if (g_msgs[i].role != ROLE_USER && g_msgs[i].role != ROLE_ASSISTANT) continue;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", g_msgs[i].role == ROLE_USER ? "user" : "assistant");
        cJSON_AddStringToObject(m, "content", g_msgs[i].text);
        cJSON_AddItemToArray(arr, m);
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s) return;
    FILE *f = fopen(HISTORY_PATH, "w");
    if (f) { fputs(s, f); fclose(f); }
    free(s);
}

static void history_load(void) {
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return; }
    char *buf = malloc(n + 1);
    size_t rd = fread(buf, 1, n, f);
    buf[rd] = 0;
    fclose(f);
    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }
    cJSON *m;
    cJSON_ArrayForEach(m, arr) {
        cJSON *r = cJSON_GetObjectItem(m, "role");
        cJSON *c = cJSON_GetObjectItem(m, "content");
        if (!cJSON_IsString(r) || !cJSON_IsString(c)) continue;
        msg_add(!strcmp(r->valuestring, "user") ? ROLE_USER : ROLE_ASSISTANT, c->valuestring);
    }
    cJSON_Delete(arr);
    // Если история закончилась на сообщении пользователя без ответа — не отправляем его повторно
    if (g_msg_count > 0 && g_msgs[g_msg_count - 1].role == ROLE_USER)
        g_msgs[g_msg_count - 1].excluded = true;
}

// --------------------------------------------------------------------------
// Сетевой запрос (в отдельном потоке, чтобы интерфейс не зависал)
// --------------------------------------------------------------------------

typedef struct {
    char *body;          // JSON-запрос
    char *resp;          // ответ сервера
    size_t resp_len;
    long http_code;
    CURLcode curl_rc;
    char curl_err[CURL_ERROR_SIZE];
    volatile bool done;
} Request;

static Request g_req;
static Thread g_req_thread;
static bool g_busy = false;
static volatile bool g_abort = false; // прервать запрос при выходе из приложения

static int progress_cb(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ud; (void)dt; (void)dn; (void)ut; (void)un;
    return g_abort ? 1 : 0;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    Request *r = (Request *)ud;
    size_t n = size * nmemb;
    char *nb = realloc(r->resp, r->resp_len + n + 1);
    if (!nb) return 0;
    r->resp = nb;
    memcpy(r->resp + r->resp_len, ptr, n);
    r->resp_len += n;
    r->resp[r->resp_len] = 0;
    return n;
}

static void request_thread(void *arg) {
    Request *r = (Request *)arg;
    CURL *c = curl_easy_init();
    if (!c) {
        r->curl_rc = CURLE_FAILED_INIT;
        snprintf(r->curl_err, sizeof(r->curl_err), "curl_easy_init failed");
        r->done = true;
        return;
    }
    char keyhdr[600];
    snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_cfg.api_key);
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "content-type: application/json");
    h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    h = curl_slist_append(h, keyhdr);

    curl_easy_setopt(c, CURLOPT_URL, API_URL);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, r->curl_err);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "claude-nx/1.0 (Nintendo Switch)");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);

    r->curl_rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r->http_code);

    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    r->done = true;
}

static char *build_request_body(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", g_cfg.model);
    cJSON_AddNumberToObject(root, "max_tokens", g_cfg.max_tokens);
    if (g_cfg.system[0]) cJSON_AddStringToObject(root, "system", g_cfg.system);
    cJSON *arr = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < g_msg_count; i++) {
        Msg *m = &g_msgs[i];
        if (m->excluded) continue;
        if (m->role != ROLE_USER && m->role != ROLE_ASSISTANT) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "role", m->role == ROLE_USER ? "user" : "assistant");
        cJSON_AddStringToObject(o, "content", m->text);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static bool request_start(void) {
    LOG("request_start");
    free(g_req.body);
    free(g_req.resp);
    memset(&g_req, 0, sizeof(g_req));
    g_req.body = build_request_body();
    if (!g_req.body) return false;
    // Стек 256 КБ, приоритет чуть ниже основного, любое ядро
    Result rc = threadCreate(&g_req_thread, request_thread, &g_req, NULL, 0x40000, 0x2C, -2);
    if (R_FAILED(rc)) return false;
    rc = threadStart(&g_req_thread);
    if (R_FAILED(rc)) { threadClose(&g_req_thread); return false; }
    g_busy = true;
    return true;
}

// Убирает ** и __ из ответа (Markdown на экране не нужен)
static void strip_markdown_bold(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if ((r[0] == '*' && r[1] == '*') || (r[0] == '_' && r[1] == '_')) { r += 2; continue; }
        *w++ = *r++;
    }
    *w = 0;
}

// Вызывается из главного потока, когда запрос завершён
static void request_finish(void) {
    LOG("request_finish: curl %d http %ld", (int)g_req.curl_rc, g_req.http_code);
    threadWaitForExit(&g_req_thread);
    threadClose(&g_req_thread);
    g_busy = false;

    int last_user = -1;
    for (int i = g_msg_count - 1; i >= 0; i--) if (g_msgs[i].role == ROLE_USER) { last_user = i; break; }

    if (g_req.curl_rc != CURLE_OK) {
        char buf[512];
        snprintf(buf, sizeof(buf), "Ошибка сети: %s",
                 g_req.curl_err[0] ? g_req.curl_err : curl_easy_strerror(g_req.curl_rc));
        if (last_user >= 0) g_msgs[last_user].excluded = true;
        msg_add(ROLE_ERROR, buf);
        return;
    }

    cJSON *root = g_req.resp ? cJSON_Parse(g_req.resp) : NULL;
    if (g_req.http_code != 200) {
        char buf[1024];
        const char *emsg = NULL;
        cJSON *err = root ? cJSON_GetObjectItem(root, "error") : NULL;
        cJSON *m = err ? cJSON_GetObjectItem(err, "message") : NULL;
        if (cJSON_IsString(m)) emsg = m->valuestring;
        const char *hint = "";
        if (g_req.http_code == 401) hint = "\nПроверь API-ключ (кнопка Y или config.txt).";
        else if (g_req.http_code == 404) hint = "\nВозможно, неверное имя модели в config.txt.";
        else if (g_req.http_code == 429) hint = "\nСлишком много запросов или закончились лимиты — подожди немного.";
        else if (g_req.http_code == 529) hint = "\nСерверы перегружены — попробуй ещё раз позже.";
        snprintf(buf, sizeof(buf), "Ошибка API (HTTP %ld): %s%s", g_req.http_code,
                 emsg ? emsg : (g_req.resp ? g_req.resp : "пустой ответ"), hint);
        if (last_user >= 0) g_msgs[last_user].excluded = true;
        msg_add(ROLE_ERROR, buf);
        cJSON_Delete(root);
        return;
    }

    // Собираем все текстовые блоки ответа
    size_t total = 0;
    cJSON *content = root ? cJSON_GetObjectItem(root, "content") : NULL;
    cJSON *blk;
    cJSON_ArrayForEach(blk, content) {
        cJSON *t = cJSON_GetObjectItem(blk, "text");
        if (cJSON_IsString(t)) total += strlen(t->valuestring) + 1;
    }
    if (total == 0) {
        if (last_user >= 0) g_msgs[last_user].excluded = true;
        msg_add(ROLE_ERROR, "Пустой ответ от Claude.");
        cJSON_Delete(root);
        return;
    }
    char *text = calloc(1, total + 1);
    cJSON_ArrayForEach(blk, content) {
        cJSON *t = cJSON_GetObjectItem(blk, "text");
        if (cJSON_IsString(t)) {
            if (text[0]) strcat(text, "\n");
            strcat(text, t->valuestring);
        }
    }
    strip_markdown_bold(text);
    msg_add(ROLE_ASSISTANT, text);
    free(text);

    cJSON *sr = cJSON_GetObjectItem(root, "stop_reason");
    if (cJSON_IsString(sr) && !strcmp(sr->valuestring, "max_tokens"))
        msg_add(ROLE_INFO, "(Ответ обрезан по лимиту max_tokens — его можно увеличить в config.txt)");

    cJSON_Delete(root);
    history_save();
}

// --------------------------------------------------------------------------
// Экранная клавиатура
// --------------------------------------------------------------------------

static bool ask_text(const char *header, const char *guide, const char *initial,
                     char *out, size_t out_size, u32 max_chars, bool password) {
    SwkbdConfig kbd;
    out[0] = 0;
    Result crc = swkbdCreate(&kbd, 0);
    LOG("swkbdCreate: 0x%x", crc);
    if (R_FAILED(crc)) return false;
    // Только стандартные настройки: нестандартные (тип «все раскладки», Box, перенос строки,
    // режим пароля) приводили к зависанию клавиатуры на прошивке пользователя
    swkbdConfigMakePresetDefault(&kbd);
    (void)password;
    if (header) swkbdConfigSetHeaderText(&kbd, header);
    if (guide) swkbdConfigSetGuideText(&kbd, guide);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, max_chars);
    LOG("swkbdShow...");
    Result rc = swkbdShow(&kbd, out, out_size);
    LOG("swkbdShow: 0x%x", rc);
    swkbdClose(&kbd);
    if (R_FAILED(rc)) { out[0] = 0; return false; }
    trim(out);
    return out[0] != 0;
}

// --------------------------------------------------------------------------
// Вёрстка текста
// --------------------------------------------------------------------------

typedef struct {
    char *text;
    Role role;
    bool is_label;   // строка-заголовок «Вы» / «Claude»
    int y;           // позиция от начала документа
    int h;
    SDL_Texture *tex;
    int tw, th;
} Line;

static Line *g_lines = NULL;
static int g_line_count = 0, g_line_cap = 0;
static int g_doc_h = 0;

static TTF_Font *g_font = NULL;       // основной текст
static TTF_Font *g_font_bold = NULL;  // заголовки
static TTF_Font *g_font_small = NULL; // подсказки

static void lines_free(void) {
    for (int i = 0; i < g_line_count; i++) {
        free(g_lines[i].text);
        if (g_lines[i].tex) SDL_DestroyTexture(g_lines[i].tex);
    }
    g_line_count = 0;
}

static void line_push(const char *s, size_t len, Role role, bool label, int h) {
    if (g_line_count == g_line_cap) {
        g_line_cap = g_line_cap ? g_line_cap * 2 : 256;
        g_lines = realloc(g_lines, sizeof(Line) * g_line_cap);
    }
    Line *l = &g_lines[g_line_count++];
    l->text = malloc(len + 1);
    memcpy(l->text, s, len);
    l->text[len] = 0;
    l->role = role;
    l->is_label = label;
    l->y = g_doc_h;
    l->h = h;
    l->tex = NULL;
    l->tw = l->th = 0;
    g_doc_h += h;
}

static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

static int text_width(TTF_Font *f, const char *s, size_t len) {
    static char tmp[8192];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, s, len);
    tmp[len] = 0;
    int w = 0, h = 0;
    if (TTF_SizeUTF8(f, tmp, &w, &h) != 0) return 0;
    return w;
}

// Перенос одного абзаца по ширине
static void wrap_paragraph(const char *s, size_t len, Role role, int max_w, int lh) {
    // Табуляцию считаем как пробелы — она отрисовывается квадратиком
    if (len == 0) { line_push("", 0, role, false, lh); return; }
    size_t pos = 0;
    while (pos < len) {
        const char *p = s + pos;
        size_t rem = len - pos;
        size_t i = 0, last_space = 0;
        bool has_space = false;
        while (i < rem) {
            size_t next = i + utf8_len((unsigned char)p[i]);
            if (next > rem) next = rem;
            if (text_width(g_font, p, next) > max_w) break;
            if (p[i] == ' ') { last_space = i; has_space = true; }
            i = next;
        }
        if (i >= rem) { line_push(p, rem, role, false, lh); break; }
        size_t cut;
        if (has_space && last_space > 0) cut = last_space;
        else cut = i > 0 ? i : (size_t)utf8_len((unsigned char)p[0]);
        line_push(p, cut, role, false, lh);
        pos += cut;
        while (pos < len && s[pos] == ' ') pos++;
    }
}

static void layout_all(void) {
    lines_free();
    g_doc_h = 0;
    int lh = TTF_FontLineSkip(g_font);
    int lh_label = TTF_FontLineSkip(g_font_bold);
    for (int m = 0; m < g_msg_count; m++) {
        Msg *msg = &g_msgs[m];
        if (msg->role == ROLE_USER) line_push("Вы", 4, ROLE_USER, true, lh_label);
        else if (msg->role == ROLE_ASSISTANT) line_push("Claude", 6, ROLE_ASSISTANT, true, lh_label);

        // заменяем табы на пробелы
        char *t = strdup(msg->text);
        for (char *q = t; *q; q++) if (*q == '\t') *q = ' ';
        const char *p = t;
        while (1) {
            const char *nl = strchr(p, '\n');
            size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
            if (seglen > 0 && p[seglen - 1] == '\r') seglen--;
            wrap_paragraph(p, seglen, msg->role, CONTENT_W, lh);
            if (!nl) break;
            p = nl + 1;
        }
        free(t);
        g_doc_h += lh / 2 + 6; // отступ между сообщениями
    }
}

// --------------------------------------------------------------------------
// Отрисовка
// --------------------------------------------------------------------------

static SDL_Renderer *g_ren = NULL;

static SDL_Color role_color(Role r, bool label) {
    switch (r) {
        case ROLE_USER:      return label ? (SDL_Color){110, 190, 255, 255} : (SDL_Color){215, 232, 255, 255};
        case ROLE_ASSISTANT: return label ? (SDL_Color){230, 150, 100, 255} : (SDL_Color){240, 240, 240, 255};
        case ROLE_INFO:      return (SDL_Color){150, 150, 160, 255};
        case ROLE_ERROR:     return (SDL_Color){255, 110, 110, 255};
    }
    return (SDL_Color){255, 255, 255, 255};
}

static void draw_text(TTF_Font *f, const char *s, int x, int y, SDL_Color c) {
    if (!s || !s[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_RenderCopy(g_ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

static int text_w(TTF_Font *f, const char *s) {
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s, &w, &h);
    return w;
}

static void fill_rect(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(g_ren, r, g, b, 255);
    SDL_Rect rc = {x, y, w, h};
    SDL_RenderFillRect(g_ren, &rc);
}

static float g_scroll = 0;      // смещение прокрутки (пиксели)
static bool g_stick_bottom = true;

static int max_scroll(void) {
    int view = CONTENT_BOTTOM - CONTENT_TOP;
    int extra = g_busy ? 60 : 0;
    int m = g_doc_h + extra - view;
    return m > 0 ? m : 0;
}

static void clamp_scroll(void) {
    int m = max_scroll();
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > m) g_scroll = m;
}

static void render(u64 frame) {
    SDL_SetRenderDrawColor(g_ren, 24, 24, 28, 255);
    SDL_RenderClear(g_ren);

    // Содержимое
    SDL_Rect clip = {0, CONTENT_TOP, SCREEN_W, CONTENT_BOTTOM - CONTENT_TOP};
    SDL_RenderSetClipRect(g_ren, &clip);
    int base = CONTENT_TOP - (int)g_scroll;
    for (int i = 0; i < g_line_count; i++) {
        Line *l = &g_lines[i];
        int y = base + l->y;
        if (y + l->h < CONTENT_TOP) {
            // далеко за экраном — освобождаем текстуру, чтобы не копить память
            if (l->tex && y + l->h < CONTENT_TOP - SCREEN_H) { SDL_DestroyTexture(l->tex); l->tex = NULL; }
            continue;
        }
        if (y > CONTENT_BOTTOM) {
            if (l->tex && y > CONTENT_BOTTOM + SCREEN_H) { SDL_DestroyTexture(l->tex); l->tex = NULL; }
            continue;
        }
        if (!l->text[0]) continue;
        if (!l->tex) {
            SDL_Surface *s = TTF_RenderUTF8_Blended(l->is_label ? g_font_bold : g_font, l->text, role_color(l->role, l->is_label));
            if (!s) continue;
            l->tex = SDL_CreateTextureFromSurface(g_ren, s);
            l->tw = s->w; l->th = s->h;
            SDL_FreeSurface(s);
            if (!l->tex) continue;
        }
        int x = MARGIN_X;
        if (l->role == ROLE_USER && !l->is_label) {
            fill_rect(MARGIN_X - 14, y, 4, l->h, 70, 130, 200);
        }
        SDL_Rect dst = {x, y, l->tw, l->th};
        SDL_RenderCopy(g_ren, l->tex, NULL, &dst);
    }
    if (g_busy) {
        static const char *dots[] = {"", ".", "..", "..."};
        char buf[64];
        snprintf(buf, sizeof(buf), "Claude думает%s", dots[(frame / 20) % 4]);
        draw_text(g_font, buf, MARGIN_X, base + g_doc_h + 6, (SDL_Color){230, 150, 100, 255});
    }
    if (g_msg_count == 0 && !g_busy) {
        const char *hello1 = "Привет! Нажми A, чтобы написать Claude.";
        draw_text(g_font, hello1, (SCREEN_W - text_w(g_font, hello1)) / 2, SCREEN_H / 2 - 40, (SDL_Color){200, 200, 210, 255});
        if (!g_cfg.api_key[0]) {
            const char *h2 = "Сначала нужен API-ключ: нажми Y или впиши его в";
            const char *h3 = "sdmc:/switch/claude/config.txt";
            draw_text(g_font_small, h2, (SCREEN_W - text_w(g_font_small, h2)) / 2, SCREEN_H / 2 + 10, (SDL_Color){255, 170, 120, 255});
            draw_text(g_font_small, h3, (SCREEN_W - text_w(g_font_small, h3)) / 2, SCREEN_H / 2 + 42, (SDL_Color){255, 170, 120, 255});
        }
    }
    SDL_RenderSetClipRect(g_ren, NULL);

    // Полоса прокрутки
    int m = max_scroll();
    if (m > 0) {
        int view = CONTENT_BOTTOM - CONTENT_TOP;
        int total = view + m;
        int bh = view * view / total; if (bh < 30) bh = 30;
        int by = CONTENT_TOP + (int)((view - bh) * (g_scroll / (float)m));
        fill_rect(SCREEN_W - 14, CONTENT_TOP, 6, view, 45, 45, 52);
        fill_rect(SCREEN_W - 14, by, 6, bh, 120, 120, 135);
    }

    // Шапка
    fill_rect(0, 0, SCREEN_W, HEADER_H, 36, 36, 42);
    fill_rect(0, HEADER_H - 2, SCREEN_W, 2, 217, 119, 87);
    draw_text(g_font_bold, "Claude", MARGIN_X, (HEADER_H - TTF_FontHeight(g_font_bold)) / 2, (SDL_Color){230, 150, 100, 255});
    char mdl[200];
    snprintf(mdl, sizeof(mdl), "модель: %s", g_cfg.model);
    draw_text(g_font_small, mdl, SCREEN_W - MARGIN_X - text_w(g_font_small, mdl),
              (HEADER_H - TTF_FontHeight(g_font_small)) / 2, (SDL_Color){160, 160, 170, 255});

    // Подвал
    fill_rect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, 36, 36, 42);
    const char *hint = "A — написать    X — новый чат    Y — API-ключ    стик — прокрутка    L/R — страница    + — выход";
    draw_text(g_font_small, hint, MARGIN_X, SCREEN_H - FOOTER_H + (FOOTER_H - TTF_FontHeight(g_font_small)) / 2,
              (SDL_Color){170, 170, 180, 255});

    SDL_RenderPresent(g_ren);
}

// --------------------------------------------------------------------------
// Шрифты
// --------------------------------------------------------------------------

static void *g_font_override_data = NULL;
static size_t g_font_override_size = 0;

static TTF_Font *open_font(const void *data, size_t size, int pt) {
    SDL_RWops *rw = SDL_RWFromConstMem(data, (int)size);
    if (!rw) return NULL;
    return TTF_OpenFontRW(rw, 1, pt);
}

static bool load_fonts(void) {
    const void *data = NULL;
    size_t size = 0;

    // Свой шрифт (если положить font.ttf рядом с config.txt)
    FILE *f = fopen(FONT_OVERRIDE_PATH, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) {
            g_font_override_data = malloc(n);
            if (g_font_override_data && fread(g_font_override_data, 1, n, f) == (size_t)n) {
                data = g_font_override_data;
                size = n;
                g_font_override_size = n;
            }
        }
        fclose(f);
    }
    if (!data) {
        PlFontData fd;
        if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) return false;
        data = fd.address;
        size = fd.size;
    }
    g_font = open_font(data, size, 26);
    g_font_bold = open_font(data, size, 27);
    g_font_small = open_font(data, size, 20);
    if (!g_font || !g_font_bold || !g_font_small) return false;
    TTF_SetFontStyle(g_font_bold, TTF_STYLE_BOLD);
    return true;
}


// --------------------------------------------------------------------------
// Встроенная экранная клавиатура (без системного апплета swkbd)
// --------------------------------------------------------------------------

typedef enum { K_CHAR, K_SHIFT, K_SYM, K_SPACE, K_BKSP, K_LANG, K_OK } KeyType;

typedef struct {
    SDL_Rect r;
    KeyType type;
    char text[24];
    int row;
} Key;

enum { OSK_MSG = 0, OSK_KEY = 1 };

static bool g_osk_active = false;
static int  g_osk_mode = OSK_MSG;
static char g_osk_buf[4096];
static int  g_osk_lang = 0;      // 0 = RU, 1 = EN
static bool g_osk_shift = false;
static bool g_osk_sym = false;
static Key  g_keys[80];
static int  g_nkeys = 0;
static int  g_osk_sel = 0;
static bool g_osk_touching = false;
static int  g_osk_hold_frames = 0;
static u64  g_osk_hold_dir = 0;
static int  g_osk_ver = 0; // меняется при изменении текста

#define OSK_MAX_CHARS 1500

// Кэш текстур подписей клавиш
typedef struct { char label[24]; SDL_Texture *tex; int w, h; } LabelTex;
static LabelTex g_label_cache[256];
static int g_label_count = 0;

static LabelTex *label_tex(const char *label) {
    for (int i = 0; i < g_label_count; i++)
        if (!strcmp(g_label_cache[i].label, label)) return &g_label_cache[i];
    if (g_label_count >= (int)(sizeof(g_label_cache) / sizeof(g_label_cache[0]))) return NULL;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, label, (SDL_Color){240, 240, 245, 255});
    if (!surf) return NULL;
    LabelTex *lt = &g_label_cache[g_label_count];
    snprintf(lt->label, sizeof(lt->label), "%s", label);
    lt->tex = SDL_CreateTextureFromSurface(g_ren, surf);
    lt->w = surf->w; lt->h = surf->h;
    SDL_FreeSurface(surf);
    if (!lt->tex) return NULL;
    g_label_count++;
    return lt;
}

#define KB_TOP 330
#define KEY_H 64
#define KEY_GAP 8

static void osk_add_row_chars(const char *chars, int row) {
    // считаем символы в строке
    int n = 0;
    for (const char *p = chars; *p; p += utf8_len((unsigned char)*p)) n++;
    int kw = 88;
    int total = n * kw + (n - 1) * KEY_GAP;
    int x = (SCREEN_W - total) / 2;
    int y = KB_TOP + row * (KEY_H + KEY_GAP);
    for (const char *p = chars; *p; ) {
        int l = utf8_len((unsigned char)*p);
        Key *k = &g_keys[g_nkeys++];
        k->r = (SDL_Rect){x, y, kw, KEY_H};
        k->type = K_CHAR;
        k->row = row;
        memcpy(k->text, p, l);
        k->text[l] = 0;
        p += l;
        x += kw + KEY_GAP;
    }
}

static void osk_add_special(KeyType t, const char *label, int w, int *x, int row) {
    Key *k = &g_keys[g_nkeys++];
    k->r = (SDL_Rect){*x, KB_TOP + row * (KEY_H + KEY_GAP), w, KEY_H};
    k->type = t;
    k->row = row;
    snprintf(k->text, sizeof(k->text), "%s", label);
    *x += w + KEY_GAP;
}

static void osk_build(void) {
    int keep_row = (g_osk_sel < g_nkeys) ? g_keys[g_osk_sel].row : 1;
    int keep_x = (g_osk_sel < g_nkeys) ? g_keys[g_osk_sel].r.x + g_keys[g_osk_sel].r.w / 2 : SCREEN_W / 2;
    g_nkeys = 0;
    osk_add_row_chars("1234567890", 0);
    if (g_osk_sym) {
        osk_add_row_chars("!?@#$%&*()-+", 1);
        osk_add_row_chars("_=:;\"'/\\<>", 2);
        osk_add_row_chars("[]{}~^|`.,", 3);
    } else if (g_osk_lang == 0) {
        osk_add_row_chars(g_osk_shift ? "ЙЦУКЕНГШЩЗХЪ" : "йцукенгшщзхъ", 1);
        osk_add_row_chars(g_osk_shift ? "ФЫВАПРОЛДЖЭ" : "фывапролджэ", 2);
        osk_add_row_chars(g_osk_shift ? "ЯЧСМИТЬБЮЁ?!" : "ячсмитьбюё.,", 3);
    } else {
        osk_add_row_chars(g_osk_shift ? "QWERTYUIOP" : "qwertyuiop", 1);
        osk_add_row_chars(g_osk_shift ? "ASDFGHJKL" : "asdfghjkl", 2);
        osk_add_row_chars(g_osk_shift ? "ZXCVBNM?!" : "zxcvbnm.,", 3);
    }
    // нижний ряд
    int widths[6] = {130, 130, 420, 170, 130, 190};
    int total = 0;
    for (int i = 0; i < 6; i++) total += widths[i];
    total += 5 * KEY_GAP;
    int x = (SCREEN_W - total) / 2;
    osk_add_special(K_SHIFT, "Aa", widths[0], &x, 4);
    osk_add_special(K_SYM, g_osk_sym ? "абв" : "?123", widths[1], &x, 4);
    osk_add_special(K_SPACE, "Пробел", widths[2], &x, 4);
    osk_add_special(K_BKSP, "Стереть", widths[3], &x, 4);
    osk_add_special(K_LANG, g_osk_lang == 0 ? "EN" : "RU", widths[4], &x, 4);
    osk_add_special(K_OK, "Готово", widths[5], &x, 4);

    // восстанавливаем выделение примерно на том же месте
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < g_nkeys; i++) {
        if (g_keys[i].row != keep_row) continue;
        int d = abs(g_keys[i].r.x + g_keys[i].r.w / 2 - keep_x);
        if (d < bestd) { bestd = d; best = i; }
    }
    g_osk_sel = best;
}

static int osk_char_count(void) {
    int n = 0;
    for (const char *p = g_osk_buf; *p; p += utf8_len((unsigned char)*p)) n++;
    return n;
}

static void osk_insert(const char *t) {
    size_t len = strlen(g_osk_buf), tl = strlen(t);
    if (len + tl + 1 >= sizeof(g_osk_buf) || osk_char_count() >= OSK_MAX_CHARS) return;
    memcpy(g_osk_buf + len, t, tl + 1);
    g_osk_ver++;
}

static void osk_backspace(void) {
    size_t len = strlen(g_osk_buf);
    if (!len) return;
    size_t i = len - 1;
    while (i > 0 && ((unsigned char)g_osk_buf[i] & 0xC0) == 0x80) i--;
    g_osk_buf[i] = 0;
    g_osk_ver++;
}

static void osk_open(int mode) {
    LOG("osk_open %d", mode);
    g_osk_mode = mode;
    g_osk_buf[0] = 0;
    g_osk_ver++;
    g_osk_shift = false;
    g_osk_sym = false;
    if (mode == OSK_KEY) g_osk_lang = 1;
    g_osk_sel = 0;
    g_nkeys = 0;
    osk_build();
    // выделяем первую букву
    for (int i = 0; i < g_nkeys; i++) if (g_keys[i].row == 1) { g_osk_sel = i; break; }
    g_osk_touching = true; // не реагировать на касание, которым открыли клавиатуру
    g_osk_active = true;
}

static void osk_finish(bool ok);

static void osk_press(int idx) {
    if (idx < 0 || idx >= g_nkeys) return;
    Key *k = &g_keys[idx];
    switch (k->type) {
        case K_CHAR:
            osk_insert(k->text);
            if (g_osk_shift && !g_osk_sym) { g_osk_shift = false; osk_build(); }
            break;
        case K_SHIFT: g_osk_shift = !g_osk_shift; osk_build(); break;
        case K_SYM:   g_osk_sym = !g_osk_sym; osk_build(); break;
        case K_SPACE: osk_insert(" "); break;
        case K_BKSP:  osk_backspace(); break;
        case K_LANG:  g_osk_lang = !g_osk_lang; g_osk_sym = false; osk_build(); break;
        case K_OK:    osk_finish(true); break;
    }
}

static void osk_move(int dx, int dy) {
    Key *cur = &g_keys[g_osk_sel];
    if (dx) {
        int n = g_osk_sel + dx;
        if (n >= 0 && n < g_nkeys && g_keys[n].row == cur->row) g_osk_sel = n;
        return;
    }
    int row = cur->row + dy;
    if (row < 0 || row > 4) return;
    int cx = cur->r.x + cur->r.w / 2;
    int best = -1, bestd = 1 << 30;
    for (int i = 0; i < g_nkeys; i++) {
        if (g_keys[i].row != row) continue;
        int d = abs(g_keys[i].r.x + g_keys[i].r.w / 2 - cx);
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best >= 0) g_osk_sel = best;
}

static void osk_update(u64 down, u64 held, HidTouchScreenState *ts) {
    if (down & HidNpadButton_Minus) { osk_finish(false); return; }
    if (down & HidNpadButton_Plus)  { osk_finish(true); return; }
    if (down & HidNpadButton_A) { osk_press(g_osk_sel); if (!g_osk_active) return; }
    if (down & HidNpadButton_B) osk_backspace();
    if (down & HidNpadButton_Y) osk_insert(" ");
    if (down & HidNpadButton_X) { g_osk_lang = !g_osk_lang; g_osk_sym = false; osk_build(); }
    if (down & (HidNpadButton_L | HidNpadButton_ZL)) { g_osk_shift = !g_osk_shift; osk_build(); }
    if (down & (HidNpadButton_R | HidNpadButton_ZR)) { g_osk_sym = !g_osk_sym; osk_build(); }

    // перемещение с автоповтором
    const u64 dirs = HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_AnyUp | HidNpadButton_AnyDown;
    u64 dir = held & dirs;
    if (dir) {
        bool fire = false;
        if (dir != g_osk_hold_dir) { g_osk_hold_dir = dir; g_osk_hold_frames = 0; fire = true; }
        else { g_osk_hold_frames++; if (g_osk_hold_frames > 18 && (g_osk_hold_frames % 5) == 0) fire = true; }
        if (fire) {
            if (dir & HidNpadButton_AnyLeft) osk_move(-1, 0);
            else if (dir & HidNpadButton_AnyRight) osk_move(1, 0);
            else if (dir & HidNpadButton_AnyUp) osk_move(0, -1);
            else if (dir & HidNpadButton_AnyDown) osk_move(0, 1);
        }
    } else {
        g_osk_hold_dir = 0;
    }

    // касание: нажатие срабатывает в момент прикосновения
    if (ts->count > 0) {
        if (!g_osk_touching) {
            int tx = ts->touches[0].x, ty = ts->touches[0].y;
            for (int i = 0; i < g_nkeys; i++) {
                SDL_Rect *r = &g_keys[i].r;
                if (tx >= r->x && tx < r->x + r->w && ty >= r->y && ty < r->y + r->h) {
                    g_osk_sel = i;
                    osk_press(i);
                    break;
                }
            }
        }
        g_osk_touching = true;
    } else {
        g_osk_touching = false;
    }
}

static void osk_render(u64 frame) {
    SDL_SetRenderDrawColor(g_ren, 24, 24, 28, 255);
    SDL_RenderClear(g_ren);

    // шапка
    fill_rect(0, 0, SCREEN_W, HEADER_H, 36, 36, 42);
    fill_rect(0, HEADER_H - 2, SCREEN_W, 2, 217, 119, 87);
    const char *title = g_osk_mode == OSK_KEY ? "API-ключ Anthropic" : "Сообщение для Claude";
    draw_text(g_font_bold, title, MARGIN_X, (HEADER_H - TTF_FontHeight(g_font_bold)) / 2, (SDL_Color){230, 150, 100, 255});
    char cnt[64];
    snprintf(cnt, sizeof(cnt), "%d / %d   %s", osk_char_count(), OSK_MAX_CHARS, g_osk_sym ? "?123" : (g_osk_lang == 0 ? "RU" : "EN"));
    draw_text(g_font_small, cnt, SCREEN_W - MARGIN_X - text_w(g_font_small, cnt),
              (HEADER_H - TTF_FontHeight(g_font_small)) / 2, (SDL_Color){160, 160, 170, 255});

    // поле ввода: показываем последние строки текста
    int box_y = HEADER_H + 12, box_h = KB_TOP - box_y - 16;
    fill_rect(MARGIN_X - 10, box_y, SCREEN_W - 2 * (MARGIN_X - 10), box_h, 40, 40, 48);
    int lh = TTF_FontLineSkip(g_font);
    int max_lines = (box_h - 16) / lh;
    int maxw = SCREEN_W - 2 * MARGIN_X - 20;
    // разбиваем текст на строки (по ширине), запоминаем начала строк
    static int starts[2048];
    static int nl = 0;
    static int cached_ver = -1;
    const char *t = g_osk_buf;
    size_t len = strlen(t);
    if (cached_ver != g_osk_ver) {
        cached_ver = g_osk_ver;
        size_t pos = 0;
        nl = 0;
        starts[nl++] = 0;
        while (pos < len && nl < 2047) {
            size_t i = pos, last_ok = pos;
            while (i < len) {
                size_t nx = i + utf8_len((unsigned char)t[i]);
                if (text_width(g_font, t + pos, nx - pos) > maxw) break;
                last_ok = nx;
                i = nx;
            }
            if (i >= len) break;
            if (last_ok == pos) last_ok = pos + utf8_len((unsigned char)t[pos]);
            pos = last_ok;
            starts[nl++] = (int)pos;
        }
    }
    int first = nl > max_lines ? nl - max_lines : 0;
    for (int l = first; l < nl; l++) {
        int a = starts[l], b = (l + 1 < nl) ? starts[l + 1] : (int)len;
        char line[1024];
        int n = b - a; if (n > 1000) n = 1000;
        memcpy(line, t + a, n); line[n] = 0;
        bool last = (l == nl - 1);
        if (last && (frame / 30) % 2 == 0) strcat(line, "|");
        if (line[0]) draw_text(g_font, line, MARGIN_X, box_y + 8 + (l - first) * lh, (SDL_Color){240, 240, 240, 255});
    }
    if (!g_osk_buf[0]) {
        const char *ph = g_osk_mode == OSK_KEY ? "Введи ключ sk-ant-... (проще вписать его в config.txt на ПК)" : "Набери сообщение...";
        draw_text(g_font, ph, MARGIN_X + 14, box_y + 8, (SDL_Color){120, 120, 130, 255});
    }

    // клавиши
    for (int i = 0; i < g_nkeys; i++) {
        Key *k = &g_keys[i];
        bool sel = (i == g_osk_sel);
        bool special = k->type != K_CHAR;
        bool active = (k->type == K_SHIFT && g_osk_shift) || (k->type == K_SYM && g_osk_sym);
        if (sel) fill_rect(k->r.x - 3, k->r.y - 3, k->r.w + 6, k->r.h + 6, 230, 150, 100);
        if (k->type == K_OK) fill_rect(k->r.x, k->r.y, k->r.w, k->r.h, 160, 90, 60);
        else if (active) fill_rect(k->r.x, k->r.y, k->r.w, k->r.h, 80, 110, 170);
        else if (special) fill_rect(k->r.x, k->r.y, k->r.w, k->r.h, 52, 52, 62);
        else fill_rect(k->r.x, k->r.y, k->r.w, k->r.h, 66, 66, 78);
        LabelTex *lt = label_tex(k->text);
        if (lt) {
            SDL_Rect dst = {k->r.x + (k->r.w - lt->w) / 2, k->r.y + (k->r.h - lt->h) / 2, lt->w, lt->h};
            SDL_RenderCopy(g_ren, lt->tex, NULL, &dst);
        }
    }

    // подсказка
    fill_rect(0, SCREEN_H - 34, SCREEN_W, 34, 36, 36, 42);
    const char *hint = "A — нажать   B — стереть   Y — пробел   X — RU/EN   L — Aa   R — ?123   + — готово   − — отмена";
    draw_text(g_font_small, hint, MARGIN_X, SCREEN_H - 34 + (34 - TTF_FontHeight(g_font_small)) / 2, (SDL_Color){170, 170, 180, 255});

    SDL_RenderPresent(g_ren);
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------

static void submit_message(const char *text) {
    msg_add(ROLE_USER, text);
    if (!request_start()) {
        g_msgs[g_msg_count - 1].excluded = true;
        msg_add(ROLE_ERROR, "Не удалось запустить сетевой поток.");
    }
    layout_all();
    g_stick_bottom = true;
}

static void save_api_key(const char *key) {
    snprintf(g_cfg.api_key, sizeof(g_cfg.api_key), "%s", key);
    config_save();
    msg_add(ROLE_INFO, "API-ключ сохранён в sdmc:/switch/claude/config.txt");
    layout_all();
    g_stick_bottom = true;
}

static void osk_finish(bool ok) {
    LOG("osk_finish %d", (int)ok);
    g_osk_active = false;
    if (!ok) return;
    char tmp[sizeof(g_osk_buf)];
    snprintf(tmp, sizeof(tmp), "%s", g_osk_buf);
    trim(tmp);
    if (!tmp[0]) return;
    if (g_osk_mode == OSK_KEY) save_api_key(tmp);
    else submit_message(tmp);
}

static void send_message(void) {
    if (g_busy) return;
    LOG("A pressed");
    if (!g_cfg.api_key[0]) {
        msg_add(ROLE_ERROR, "Не задан API-ключ. Нажми Y, чтобы ввести его, или впиши api_key=... в sdmc:/switch/claude/config.txt");
        layout_all();
        g_stick_bottom = true;
        return;
    }
    if (g_cfg.system_keyboard) {
        static char input[8192];
        if (ask_text("Сообщение для Claude", "Напиши сообщение...", NULL, input, sizeof(input), 500, false))
            submit_message(input);
        return;
    }
    osk_open(OSK_MSG);
}

static void enter_api_key(void) {
    if (g_busy) return;
    LOG("Y pressed");
    if (g_cfg.system_keyboard) {
        static char key[600];
        if (ask_text("API-ключ Anthropic", "sk-ant-...", NULL, key, sizeof(key), 300, true)) save_api_key(key);
        return;
    }
    osk_open(OSK_KEY);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    remove("sdmc:/switch/claude/log.txt");
    LOG("claude-nx 1.0.3 start, applet type %d", (int)appletGetAppletType());

    Result rc = socketInitializeDefault();
    LOG("socketInitializeDefault: 0x%x", rc);
    rc = plInitialize(PlServiceType_User);
    LOG("plInitialize: 0x%x", rc);

    CURLcode cc = curl_global_init(CURL_GLOBAL_DEFAULT);
    LOG("curl_global_init: %d", (int)cc);

    int sr = SDL_Init(SDL_INIT_VIDEO);
    LOG("SDL_Init: %d %s", sr, sr ? SDL_GetError() : "");
    sr = TTF_Init();
    LOG("TTF_Init: %d", sr);
    SDL_Window *win = SDL_CreateWindow("Claude", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    LOG("SDL_CreateWindow: %p %s", (void*)win, win ? "" : SDL_GetError());
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    LOG("SDL_CreateRenderer: %p %s", (void*)g_ren, g_ren ? "" : SDL_GetError());
    if (!win || !g_ren) { LOG("no window/renderer, exit"); SDL_Quit(); socketExit(); return 1; }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    // Тёмно-синий экран = графика заработала
    SDL_SetRenderDrawColor(g_ren, 20, 30, 60, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
    LOG("first present ok");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    LOG("input ok");

    config_load();
    LOG("config_load ok, key %s, model %s", g_cfg.api_key[0] ? "set" : "empty", g_cfg.model);
    bool fonts_ok = load_fonts();
    LOG("load_fonts: %d", (int)fonts_ok);
    if (fonts_ok) {
        LOG("cyrillic glyph: %d", TTF_GlyphIsProvided(g_font, 0x0416));
    }

    if (!fonts_ok) {
        LOG("font error: %s", TTF_GetError());
        // Красный экран = не удалось загрузить шрифт; ждём нажатия + для выхода
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            SDL_SetRenderDrawColor(g_ren, 120, 20, 20, 255);
            SDL_RenderClear(g_ren);
            SDL_RenderPresent(g_ren);
        }
        SDL_DestroyRenderer(g_ren);
        SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        curl_global_cleanup();
        plExit();
        socketExit();
        return 1;
    }

    history_load();
    LOG("history_load: %d msgs", g_msg_count);
    if (g_msg_count > 0) msg_add(ROLE_INFO, "(Продолжение прошлого чата. X — начать новый.)");
    layout_all();
    LOG("layout ok, entering main loop");

    bool touching = false;
    int touch_last_y = 0;
    float touch_vel = 0;
    u64 frame = 0;

    while (appletMainLoop()) {
        frame++;
        SDL_Event ev;
        bool quit = false;
        while (SDL_PollEvent(&ev)) if (ev.type == SDL_QUIT) quit = true;
        if (quit) break;
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad);

        if (g_busy && g_req.done) {
            request_finish();
            layout_all();
            g_stick_bottom = true;
        }

        if (g_osk_active) {
            HidTouchScreenState ots = {0};
            hidGetTouchScreenStates(&ots, 1);
            osk_update(down, held, &ots);
            if (g_osk_active) { osk_render(frame); continue; }
            // клавиатуру только что закрыли — не обрабатываем эти же нажатия в чате
            down = 0;
        }

        if (down & HidNpadButton_Plus) break;

        if (down & HidNpadButton_A) send_message();
        if ((down & HidNpadButton_X) && !g_busy) {
            msg_clear();
            remove(HISTORY_PATH);
            layout_all();
            g_scroll = 0;
        }
        if (down & HidNpadButton_Y) enter_api_key();
        if (g_osk_active) continue;

        // Прокрутка
        float dy = 0;
        if (held & (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) dy -= 14;
        if (held & (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)) dy += 14;
        HidAnalogStickState rs = padGetStickPos(&pad, 1);
        HidAnalogStickState ls = padGetStickPos(&pad, 0);
        int sy = abs(rs.y) > abs(ls.y) ? rs.y : ls.y;
        if (abs(sy) > 6000) dy = -(float)sy / JOYSTICK_MAX * 28.0f;
        int page = CONTENT_BOTTOM - CONTENT_TOP - 60;
        if (down & (HidNpadButton_L | HidNpadButton_ZL)) dy -= page;
        if (down & (HidNpadButton_R | HidNpadButton_ZR)) dy += page;

        // Касание: перетаскивание с инерцией
        HidTouchScreenState ts = {0};
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            int ty = ts.touches[0].y;
            if (touching) {
                float d = (float)(touch_last_y - ty);
                dy += d;
                touch_vel = d;
            }
            touching = true;
            touch_last_y = ty;
        } else {
            if (touching) touching = false;
            if (touch_vel > 0.5f || touch_vel < -0.5f) { dy += touch_vel; touch_vel *= 0.93f; }
            else touch_vel = 0;
        }

        if (dy != 0) {
            g_scroll += dy;
            clamp_scroll();
            g_stick_bottom = (g_scroll >= max_scroll() - 2);
        }
        if (g_stick_bottom) g_scroll = max_scroll();
        clamp_scroll();

        render(frame);
        if (frame == 1 || frame == 60) LOG("frame %llu rendered", (unsigned long long)frame);
    }
    LOG("main loop exit");

    if (g_busy) {
        // Прерываем запрос и ждём завершения сетевого потока
        g_abort = true;
        threadWaitForExit(&g_req_thread);
        threadClose(&g_req_thread);
    }
    history_save();

    lines_free();
    free(g_lines);
    msg_clear();
    free(g_msgs);
    free(g_req.body);
    free(g_req.resp);

    TTF_CloseFont(g_font);
    TTF_CloseFont(g_font_bold);
    TTF_CloseFont(g_font_small);
    free(g_font_override_data);
    TTF_Quit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    curl_global_cleanup();
    plExit();
    socketExit();
    return 0;
}
