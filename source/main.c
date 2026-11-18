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
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    if (password) swkbdConfigMakePresetPassword(&kbd);
    else swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, password ? SwkbdType_QWERTY : SwkbdType_All); // все раскладки, включая русскую
    if (header) swkbdConfigSetHeaderText(&kbd, header);
    if (guide) swkbdConfigSetGuideText(&kbd, guide);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetOkButtonText(&kbd, password ? "Сохранить" : "Отправить");
    swkbdConfigSetStringLenMax(&kbd, max_chars);
    swkbdConfigSetTextDrawType(&kbd, SwkbdTextDrawType_Box);
    if (!password) swkbdConfigSetReturnButtonFlag(&kbd, 1);
    Result rc = swkbdShow(&kbd, out, out_size);
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
// main
// --------------------------------------------------------------------------

static void send_message(void) {
    if (g_busy) return;
    if (!g_cfg.api_key[0]) {
        msg_add(ROLE_ERROR, "Не задан API-ключ. Нажми Y, чтобы ввести его, или впиши api_key=... в sdmc:/switch/claude/config.txt");
        layout_all();
        g_stick_bottom = true;
        return;
    }
    static char input[8192];
    if (!ask_text("Сообщение для Claude", "Напиши сообщение...", NULL, input, sizeof(input), 2000, false)) return;
    msg_add(ROLE_USER, input);
    if (!request_start()) {
        g_msgs[g_msg_count - 1].excluded = true;
        msg_add(ROLE_ERROR, "Не удалось запустить сетевой поток.");
    }
    layout_all();
    g_stick_bottom = true;
}

static void enter_api_key(void) {
    if (g_busy) return;
    static char key[600];
    if (ask_text("API-ключ Anthropic", "sk-ant-...", NULL, key, sizeof(key), 300, true)) {
        snprintf(g_cfg.api_key, sizeof(g_cfg.api_key), "%s", key);
        config_save();
        msg_add(ROLE_INFO, "API-ключ сохранён в sdmc:/switch/claude/config.txt");
        layout_all();
        g_stick_bottom = true;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    socketInitializeDefault();
    plInitialize(PlServiceType_User);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    SDL_Window *win = SDL_CreateWindow("Claude", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    config_load();
    bool fonts_ok = load_fonts();

    if (!fonts_ok) {
        // Без шрифта нечего рисовать — выходим аккуратно
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
    if (g_msg_count > 0) msg_add(ROLE_INFO, "(Продолжение прошлого чата. X — начать новый.)");
    layout_all();

    bool touching = false;
    int touch_last_y = 0;
    float touch_vel = 0;
    u64 frame = 0;

    while (appletMainLoop()) {
        frame++;
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        u64 held = padGetButtons(&pad);

        if (down & HidNpadButton_Plus) break;

        if (g_busy && g_req.done) {
            request_finish();
            layout_all();
            g_stick_bottom = true;
        }

        if (down & HidNpadButton_A) send_message();
        if ((down & HidNpadButton_X) && !g_busy) {
            msg_clear();
            remove(HISTORY_PATH);
            layout_all();
            g_scroll = 0;
        }
        if (down & HidNpadButton_Y) enter_api_key();

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
    }

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
