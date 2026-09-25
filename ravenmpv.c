/*
 * RavenMPV - A lightweight MPV wrapper with playlist management
 * Version: 0.8.0 - Fixed Edition
 * License: MIT
 * 
 * FEATURES:
 * - Feed view with thumbnails (chafa)
 * - Search with TUI interface
 * - Quality selection
 * - Playlist management
 * - Related/Similar recommendations
 * - JSON Arc (funny messages)
 * - Debug mode
 * 
 * FIXES in 0.8.0:
 * - Fixed forward declaration issue
 * - Fixed TUI screen garbage
 * - Fixed 403 Forbidden errors
 * - Fixed thumbnail rendering with chafa
 * - Clean MPV output
 * - Better format fallback
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <stdarg.h>
#include <termios.h>
#include <sys/resource.h>
#include <ncurses.h>

#define VERSION "0.8.0"
#define CONFIG_DIR ".config/ravenmpv"
#define SOCKET_PATH "/tmp/ravenmpv.sock"
#define PLAYLIST_FILE "playlists.json"
#define CONFIG_FILE "config.json"
#define MAX_CMD_LEN 8192
#define MAX_SEARCH_RESULTS 20

static int add_count = 0;
static int debug_mode = 0;
static int has_chafa = 0;

typedef struct json_value {
    char *string;
    struct json_value *next;
    struct json_value *child;
    char type;
} json_value;

typedef struct {
    char title[256];
    char id[64];
    char channel[128];
    char duration[16];
    char views[32];
    char thumbnail_path[256];
} feed_item;

/* ========== FORWARD DECLARATIONS ========== */
static void json_free(json_value *val);
static int save_config(json_value *root);
static json_value *load_config(void);
static char *safe_sanitize_url(const char *url);
static int play_with_pipe(const char *url, const char *mode);
static int playlist_add_with_name(json_value *root, const char *name, const char *url, const char *title);
static char *get_song_name_from_url(const char *url);
static char *get_proxy_from_config(json_value *config);
static void show_tui_playlist(json_value *root, const char *name);
static json_value *get_playlist(json_value *root, const char *name);
static json_value *load_playlists(void);
static int save_playlists(json_value *root);
static char *get_playlist_url_by_index(int index);
static int show_related_results(const char *url, const char *mode);
static int show_similar_results(const char *url, const char *mode);
static void show_play_more_prompt(const char *url, const char *mode);
static int is_mpv_paused(const char *socket_path);
static void debug_log(const char *format, ...);
static void debug_step(const char *step, const char *info);
static void segfault_handler(int sig);
static void show_json_arc_spinner(const char *message, int duration_ms);
static void show_json_arc_animation(int add_count, int success, int is_paused);
static int show_quality_menu_tui(const char *url);
static char *get_format_string(int choice);
static void tui_search(const char *query);
static void tui_feed(const char *query);
static int add_to_playlist_with_json_arc(const char *url, const char *title);
static int check_chafa_exists(void);
static int download_thumbnail(const char *video_id, char *output_path, size_t path_size);
static void draw_thumbnail(int y, int x, int w, int h, const char *image_path);

/* ========== JSON PARSER ========== */
static char *json_skip_whitespace(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

static char *json_parse_string(char *s, char **out) {
    s = json_skip_whitespace(s);
    if (*s != '"') return NULL;
    s++;
    char *start = s;
    int len = 0;
    while (*s && *s != '"') {
        if (*s == '\\') { s++; if (*s) s++; }
        else s++;
        len++;
    }
    if (*s != '"') return NULL;
    *out = malloc(len + 1);
    if (!*out) return NULL;
    char *dst = *out;
    s = start;
    while (*s && *s != '"') {
        if (*s == '\\') {
            s++;
            if (!*s) break;
            switch (*s) {
                case 'n': *dst++ = '\n'; break;
                case 't': *dst++ = '\t'; break;
                case 'r': *dst++ = '\r'; break;
                default: *dst++ = *s; break;
            }
            s++;
        } else *dst++ = *s++;
    }
    *dst = '\0';
    return s + 1;
}

static char *json_parse_value(char *s, json_value **val) {
    if (!s || !*s) { *val = NULL; return s; }
    s = json_skip_whitespace(s);
    if (!s || !*s) { *val = NULL; return s; }
    *val = calloc(1, sizeof(json_value));
    if (!*val) return NULL;
    
    if (*s == '"') { (*val)->type = 's'; return json_parse_string(s, &(*val)->string); }
    
    if (*s == '{') {
        (*val)->type = 'o';
        s++;
        json_value *last = NULL;
        while (*s && *s != '}') {
            s = json_skip_whitespace(s);
            if (*s == '}') break;
            char *key;
            s = json_parse_string(s, &key);
            if (!s || !key) break;
            s = json_skip_whitespace(s);
            if (*s != ':') { free(key); break; }
            s++;
            json_value *child;
            s = json_parse_value(s, &child);
            if (!s) { free(key); break; }
            json_value *key_node = calloc(1, sizeof(json_value));
            if (!key_node) { free(key); json_free(child); break; }
            key_node->type = 's';
            key_node->string = key;
            key_node->child = child;
            if (!(*val)->child) (*val)->child = key_node;
            else last->next = key_node;
            last = key_node;
            s = json_skip_whitespace(s);
            if (*s == ',') s++;
        }
        if (*s == '}') s++;
        return s;
    }
    
    if (*s == '[') {
        (*val)->type = 'a';
        s++;
        json_value *last = NULL;
        while (*s && *s != ']') {
            s = json_skip_whitespace(s);
            if (*s == ']') break;
            json_value *child;
            s = json_parse_value(s, &child);
            if (!s) break;
            if (!(*val)->child) (*val)->child = child;
            else last->next = child;
            last = child;
            s = json_skip_whitespace(s);
            if (*s == ',') s++;
        }
        if (*s == ']') s++;
        return s;
    }
    
    if (strncmp(s, "true", 4) == 0) { (*val)->type = 't'; return s + 4; }
    if (strncmp(s, "false", 5) == 0) { (*val)->type = 'f'; return s + 5; }
    if (strncmp(s, "null", 4) == 0) { (*val)->type = 'N'; return s + 4; }
    
    (*val)->type = 'n';
    char *end;
    strtod(s, &end);
    if (end == s) { free(*val); *val = NULL; return s; }
    int len = end - s;
    (*val)->string = malloc(len + 1);
    if (!(*val)->string) { free(*val); return NULL; }
    strncpy((*val)->string, s, len);
    (*val)->string[len] = '\0';
    return end;
}

static json_value *json_parse(const char *str) {
    if (!str) return NULL;
    char *s = strdup(str);
    if (!s) return NULL;
    json_value *root = NULL;
    char *end = json_parse_value(s, &root);
    free(s);
    if (!end || !root) { json_free(root); return NULL; }
    return root;
}

static void json_free(json_value *val) {
    if (!val) return;
    if (val->string) free(val->string);
    json_free(val->child);
    json_free(val->next);
    free(val);
}

static json_value *json_find(json_value *obj, const char *key) {
    if (!obj || obj->type != 'o' || !key) return NULL;
    json_value *child = obj->child;
    while (child) {
        if (child->string && strcmp(child->string, key) == 0)
            return child->child;
        child = child->next;
    }
    return NULL;
}

static char *json_generate(json_value *val) {
    if (!val) return strdup("null");
    char *result = NULL, *temp = NULL;
    
    switch (val->type) {
        case 's': {
            if (!val->string) return strdup("null");
            size_t len = strlen(val->string);
            result = malloc(len * 2 + 3);
            if (!result) return strdup("null");
            char *p = result;
            *p++ = '"';
            for (size_t i = 0; i < len; i++) {
                char c = val->string[i];
                if (c == '"' || c == '\\') { *p++ = '\\'; *p++ = c; }
                else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
                else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
                else *p++ = c;
            }
            *p++ = '"';
            *p = '\0';
            break;
        }
        case 'n': result = strdup(val->string ? val->string : "null"); break;
        case 't': result = strdup("true"); break;
        case 'f': result = strdup("false"); break;
        case 'N': result = strdup("null"); break;
        case 'a': {
            result = strdup("[");
            json_value *child = val->child;
            int first = 1;
            while (child) {
                temp = json_generate(child);
                char *new_result = malloc(strlen(result) + strlen(temp) + 3);
                sprintf(new_result, "%s%s%s", result, first ? "" : ",", temp);
                first = 0;
                free(result); free(temp);
                result = new_result;
                child = child->next;
            }
            temp = malloc(strlen(result) + 2);
            sprintf(temp, "%s]", result);
            free(result);
            result = temp;
            break;
        }
        case 'o': {
            result = strdup("{");
            json_value *child = val->child;
            int first = 1;
            while (child) {
                char *key_str = malloc(strlen(child->string) + 3);
                sprintf(key_str, "\"%s\"", child->string);
                temp = json_generate(child->child);
                char *new_result = malloc(strlen(result) + strlen(key_str) + strlen(temp) + 4);
                sprintf(new_result, "%s%s%s:%s", result, first ? "" : ",", key_str, temp);
                first = 0;
                free(result); free(key_str); free(temp);
                result = new_result;
                child = child->next;
            }
            temp = malloc(strlen(result) + 2);
            sprintf(temp, "%s}", result);
            free(result);
            result = temp;
            break;
        }
        default: result = strdup("null"); break;
    }
    return result ? result : strdup("null");
}

/* ========== UTILITIES ========== */
static char *get_config_path(const char *filename) {
    char *home = getenv("HOME");
    if (!home) return NULL;
    char *path = malloc(strlen(home) + strlen(CONFIG_DIR) + strlen(filename) + 3);
    sprintf(path, "%s/%s/%s", home, CONFIG_DIR, filename);
    return path;
}

static int ensure_config_dir(void) {
    char *home = getenv("HOME");
    if (!home) return -1;
    char *path = malloc(strlen(home) + strlen(CONFIG_DIR) + 2);
    sprintf(path, "%s/%s", home, CONFIG_DIR);
    struct stat st;
    if (stat(path, &st) != 0) mkdir(path, 0755);
    free(path);
    return 0;
}

static int file_exists(const char *path) {
    return path && access(path, F_OK) == 0;
}

static char *read_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(len + 1);
    size_t read_len = fread(content, 1, len, f);
    content[read_len] = '\0';
    fclose(f);
    return content;
}

static int write_file(const char *path, const char *content) {
    if (!path || !content) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%s", content);
    fclose(f);
    return 0;
}

static char *get_timestamp(void) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char *buffer = malloc(30);
    strftime(buffer, 30, "%Y-%m-%d %H:%M:%S", tm_info);
    return buffer;
}

/* ========== URL SANITIZER ========== */
static char *safe_sanitize_url(const char *url) {
    if (!url) return NULL;
    size_t len = strlen(url);
    char *safe = malloc(len * 2 + 1);
    int j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = url[i];
        if (isalnum(c) || strchr("/.-_:?=&%+#@!*()~", c))
            safe[j++] = c;
    }
    safe[j] = '\0';
    return safe;
}

/* ========== PROXY ========== */
static char *get_proxy_from_config(json_value *config) {
    if (!config) return NULL;
    json_value *mpv = json_find(config, "mpv");
    if (!mpv) return NULL;
    json_value *proxy = json_find(mpv, "proxy");
    if (!proxy || !proxy->string) return NULL;
    if (strlen(proxy->string) == 0) return NULL;
    return strdup(proxy->string);
}

/* ========== RATE LIMIT ========== */
static int check_rate_limit(void) {
    static time_t last = 0;
    static int count = 0;
    time_t now = time(NULL);
    if (now - last > 60) { count = 0; last = now; }
    count++;
    if (count > 5) { sleep(30); count = 0; last = time(NULL); return 1; }
    return 0;
}

/* ========== CONFIG ========== */
static json_value *load_config(void) {
    char *path = get_config_path(CONFIG_FILE);
    if (!path) return NULL;
    json_value *root = NULL;
    if (file_exists(path)) {
        char *content = read_file(path);
        if (content) { root = json_parse(content); free(content); }
    }
    free(path);
    
    if (!root) {
        root = calloc(1, sizeof(json_value));
        root->type = 'o';
        json_value *mpv = calloc(1, sizeof(json_value));
        mpv->type = 'o';
        mpv->string = strdup("mpv");
        
        json_value *cache = calloc(1, sizeof(json_value));
        cache->type = 's'; cache->string = strdup("cache_size");
        cache->child = calloc(1, sizeof(json_value));
        cache->child->type = 's'; cache->child->string = strdup("64MiB");
        
        json_value *res = calloc(1, sizeof(json_value));
        res->type = 's'; res->string = strdup("max_resolution");
        res->child = calloc(1, sizeof(json_value));
        res->child->type = 's'; res->child->string = strdup("720");
        
        json_value *browser = calloc(1, sizeof(json_value));
        browser->type = 's'; browser->string = strdup("browser");
        browser->child = calloc(1, sizeof(json_value));
        browser->child->type = 's'; browser->child->string = strdup("firefox");
        
        json_value *proxy = calloc(1, sizeof(json_value));
        proxy->type = 's'; proxy->string = strdup("proxy");
        proxy->child = calloc(1, sizeof(json_value));
        proxy->child->type = 's'; proxy->child->string = strdup("");
        
        mpv->child = cache;
        cache->next = res;
        res->next = browser;
        browser->next = proxy;
        root->child = mpv;
        save_config(root);
    }
    return root;
}

static int save_config(json_value *root) {
    if (!root) return -1;
    ensure_config_dir();
    char *path = get_config_path(CONFIG_FILE);
    char *json_str = json_generate(root);
    int result = write_file(path, json_str);
    free(json_str);
    free(path);
    return result;
}

static char *get_config_string(json_value *root, const char *key, const char *def) {
    if (!root || !key) return strdup(def ? def : "");
    json_value *mpv = json_find(root, "mpv");
    if (!mpv) return strdup(def ? def : "");
    json_value *val = json_find(mpv, key);
    if (!val || !val->string) return strdup(def ? def : "");
    return strdup(val->string);
}

/* ========== DEBUG ========== */
static void debug_log(const char *format, ...) {
    if (!debug_mode) return;
    va_list args;
    va_start(args, format);
    printf("\033[1;33m[DEBUG]\033[0m ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

static void debug_step(const char *step, const char *info) {
    if (!debug_mode) return;
    printf("\033[1;36m[STEP] %s\033[0m\n", step);
    if (info) printf("  -> %s\n", info);
}

static void segfault_handler(int sig) {
    fprintf(stderr, "\n\033[1;31m[!] Crash (signal %d)\033[0m\n", sig);
    exit(1);
}

/* ========== CHAFA CHECK ========== */
static int check_chafa_exists(void) {
    char *args[] = {"which", "chafa", NULL};
    pid_t pid = fork();
    if (pid == 0) {
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        execvp("which", args);
        exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ========== DOWNLOAD THUMBNAIL ========== */
static int download_thumbnail(const char *video_id, char *output_path, size_t path_size) {
    if (!video_id || strlen(video_id) == 0) return -1;
    snprintf(output_path, path_size, "/tmp/raven_thumb_%s.jpg", video_id);
    if (file_exists(output_path)) return 0;
    
    char url[512];
    snprintf(url, sizeof(url), "https://img.youtube.com/vi/%s/mqdefault.jpg", video_id);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -sL -o \"%s\" \"%s\" 2>/dev/null", output_path, url);
    
    system(cmd);
    return file_exists(output_path) ? 0 : -1;
}

/* ========== DRAW THUMBNAIL (FIXED) ========== */
static void draw_thumbnail(int y, int x, int w, int h, const char *image_path) {
    if (!image_path || !file_exists(image_path)) return;
    if (!has_chafa) return;
    
    // Use chafa with no colors, strip escape codes
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "chafa --format=symbols --colors=none --symbols=block "
             "--size=%dx%d \"%s\" 2>/dev/null",
             w, h, image_path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    
    char line[2048];
    int row = 0;
    while (fgets(line, sizeof(line), fp) && row < h) {
        line[strcspn(line, "\n")] = '\0';
        
        // Strip ANSI escape codes
        char clean[2048];
        int j = 0, in_esc = 0;
        for (int i = 0; line[i] && j < 2047; i++) {
            if (line[i] == '\033') { in_esc = 1; continue; }
            if (in_esc) {
                if (line[i] >= '@' && line[i] <= '~') in_esc = 0;
                continue;
            }
            clean[j++] = line[i];
        }
        clean[j] = '\0';
        
        mvprintw(y + row, x, "%s", clean);
        row++;
    }
    pclose(fp);
}

/* ========== JSON ARC ========== */
static void show_json_arc_spinner(const char *msg, int duration_ms) {
    char spinner[] = "|/-\\";
    int steps = duration_ms / 100;
    for (int i = 0; i < steps; i++) {
        printf("\r%c %s ", spinner[i % 4], msg);
        fflush(stdout);
        usleep(100000);
    }
    printf("\r  %s\n", msg);
}

static void show_json_arc_animation(int add_count, int success, int is_paused) {
    char *messages[] = {"Fighting JSON...", "Loading...", "Almost there..."};
    for (int i = 0; i < 10; i++) {
        printf("\r%c %s", "|/-\\"[i % 4], messages[i % 3]);
        fflush(stdout);
        usleep(150000);
    }
    printf("\r  Done!\n");
    
    if (!success) {
        if (is_paused) {
            printf("json:no no boss why pause?!\n");
            printf("json:YOU COOKED NOW!\n");
        } else {
            printf("json:you cooked now!\n");
            printf("json:MWAHAHAHA!\n");
        }
        return;
    }
    
    if (add_count == 1) {
        printf("Yess json is so noob!\n");
        printf("I win wait who tf name is json?\n");
    } else {
        char *scared[] = {
            "json:no no don't hurt me!",
            "json:please i have family!",
            "json:okay okay you win!",
            "json:nooooooo!",
            "json:i give up!",
        };
        printf("%s\n", scared[rand() % 5]);
    }
}

/* ========== QUALITY MENU ========== */
static int show_quality_menu_tui(const char *url) {
    char cmd[1024];
    json_value *config = load_config();
    char *browser = config ? get_config_string(config, "browser", "firefox") : strdup("firefox");
    
    snprintf(cmd, sizeof(cmd),
             "yt-dlp --cookies-from-browser %s --list-formats \"%s\" 2>/dev/null | "
             "grep -E '^[0-9]+' | head -20",
             browser, url);
    free(browser);
    if (config) json_free(config);
    
    printf("\n");
    printf("┌─────────────────────────────────────────────┐\n");
    printf("│            Available Formats                │\n");
    printf("├─────────────────────────────────────────────┤\n");
    printf("│ 0. Auto (best quality)                      │\n");
    printf("├─────────────────────────────────────────────┤\n");
    
    FILE *fp = popen(cmd, "r");
    if (!fp) { printf("└─────────────────────────────────────────────┘\n"); return 0; }
    
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < 15) {
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) > 0 && isdigit(line[0])) {
            // Parse format ID
            int format_id = atoi(line);
            
            // Get display name
            char display[128] = "";
            if (strstr(line, "audio only")) {
                if (strstr(line, "opus")) snprintf(display, 128, "Opus Audio");
                else if (strstr(line, "m4a")) snprintf(display, 128, "AAC Audio");
                else snprintf(display, 128, "Audio");
            } else {
                char *res = strstr(line, "x");
                if (res) {
                    int h = 0;
                    char *p = res + 1;
                    while (*p && isdigit(*p)) { h = h * 10 + (*p - '0'); p++; }
                    snprintf(display, 128, "%dp Video", h);
                } else {
                    snprintf(display, 128, "Video");
                }
            }
            
            printf("│ %2d. %-39s │\n", format_id, display);
            count++;
        }
    }
    pclose(fp);
    
    printf("└─────────────────────────────────────────────┘\n");
    printf("Enter number (0 for auto): ");
    fflush(stdout);
    
    char input[32];
    if (!fgets(input, sizeof(input), stdin)) return 0;
    input[strcspn(input, "\n")] = '\0';
    return atoi(input);
}

static char *get_format_string(int format_id) {
    if (format_id <= 0) {
        return strdup("bestvideo+bestaudio/best");
    }
    char *result = malloc(128);
    snprintf(result, 128, "%d+bestaudio/%d/bestvideo+bestaudio/best", format_id, format_id);
    return result;
}

/* ========== PLAY WITH PIPE (FIXED 403) ========== */
static int play_with_pipe(const char *url, const char *mode) {
    if (!url) return -1;
    check_rate_limit();
    
    int format_id = show_quality_menu_tui(url);
    char *format_str = get_format_string(format_id);
    char *safe_url = safe_sanitize_url(url);
    if (!safe_url) { free(format_str); return -1; }
    
    json_value *config = load_config();
    if (!config) { free(safe_url); free(format_str); return -1; }
    
    char *cache_size = get_config_string(config, "cache_size", "64MiB");
    char *browser = get_config_string(config, "browser", "firefox");
    char *proxy = get_proxy_from_config(config);
    
    char proxy_flag[256] = "";
    if (proxy) {
        snprintf(proxy_flag, sizeof(proxy_flag), "--proxy %s", proxy);
        setenv("http_proxy", proxy, 1);
        setenv("https_proxy", proxy, 1);
    }
    
    char *mpv_opts = (mode && strcmp(mode, "audio") == 0)
        ? "--no-video --keep-open=no --really-quiet"
        : "--keep-open=no --really-quiet";
    
    char cmd[MAX_CMD_LEN];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp "
             "--sleep-interval 2 --max-sleep-interval 5 "
             "--extractor-args \"youtube:player_client=android,ios,web\" "
             "--user-agent \"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\" "
             "--no-check-certificate "
             "%s "
             "--cookies-from-browser %s "
             "-f \"%s\" \"%s\" -o - 2>/dev/null | "
             "mpv --cache=yes --demuxer-max-bytes=%s --keep-open=no "
             "--profile=fast --hwdec=auto %s --no-osd --no-terminal - 2>/dev/null",
             proxy_flag, browser, format_str, safe_url, cache_size, mpv_opts);
    
    debug_log("Command: %s", cmd);
    int ret = system(cmd);
    
    if (ret != 0) {
        printf("\n[!] Playback failed\n");
        printf("Try: 0 (auto) or set proxy\n");
    }
    
    free(safe_url);
    free(format_str);
    free(cache_size);
    free(browser);
    if (proxy) free(proxy);
    json_free(config);
    return ret;
}

/* ========== GET SONG NAME ========== */
static char *get_song_name_from_url(const char *url) {
    if (!url) return NULL;
    char cmd[1024];
    json_value *config = load_config();
    char *browser = config ? get_config_string(config, "browser", "firefox") : strdup("firefox");
    snprintf(cmd, sizeof(cmd), "yt-dlp --cookies-from-browser %s --get-title \"%s\" 2>/dev/null",
             browser, url);
    free(browser);
    if (config) json_free(config);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char buffer[512];
    if (fgets(buffer, sizeof(buffer), fp)) {
        buffer[strcspn(buffer, "\n")] = '\0';
        pclose(fp);
        if (strlen(buffer) > 0) return strdup(buffer);
    }
    pclose(fp);
    return NULL;
}

/* ========== ARTIST ========== */
static char *get_artist_from_url(const char *url) {
    if (!url) return NULL;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "yt-dlp --get-artist \"%s\" 2>/dev/null", url);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char buffer[256];
    if (fgets(buffer, sizeof(buffer), fp)) {
        buffer[strcspn(buffer, "\n")] = '\0';
        pclose(fp);
        if (strlen(buffer) > 0) return strdup(buffer);
    }
    pclose(fp);
    return NULL;
}

/* ========== QUERIES ========== */
static char *generate_related_query(const char *artist, int count) {
    if (!artist) {
        char *fb[] = {"music", "songs", "vibes", "playlist", "hits"};
        return strdup(fb[(rand() + count) % 5]);
    }
    char *q[] = {"\"%s\" similar", "\"%s\" music", "\"%s\" best", "\"%s\" hits"};
    char *result = malloc(256);
    snprintf(result, 256, q[(rand() + count) % 4], artist);
    return result;
}

static char *generate_similar_query(const char *artist, int count) {
    if (!artist) return strdup("similar music");
    char *q[] = {"\"%s\" similar songs", "\"%s\" like this", "\"%s\" vibe"};
    char *result = malloc(256);
    snprintf(result, 256, q[(rand() + count) % 3], artist);
    return result;
}

/* ========== PLAY MORE ========== */
static void show_play_more_prompt(const char *url, const char *mode) {
    printf("\n──────────────────────────\n");
    printf("Do u wanna play more?\n\n");
    printf("[R]andom  [F]ind  [N]o\n");
    printf("> ");
    fflush(stdout);
    
    char input[32];
    if (!fgets(input, sizeof(input), stdin)) return;
    input[strcspn(input, "\n")] = '\0';
    
    if (input[0] == 'r' || input[0] == 'R') show_related_results(url, mode);
    else if (input[0] == 'f' || input[0] == 'F') show_similar_results(url, mode);
}

/* ========== ADD TO PLAYLIST ========== */
static int add_to_playlist_with_json_arc(const char *url, const char *title) {
    char *auto_name = get_song_name_from_url(url);
    json_value *root = load_playlists();
    int ret = -1;
    add_count++;
    
    show_json_arc_spinner("Loading...", 800);
    int is_paused = is_mpv_paused(SOCKET_PATH);
    
    if (root) {
        ret = playlist_add_with_name(root, "default", url, auto_name);
        if (ret == 0) {
            save_playlists(root);
            printf("\nAdded: %s\n", title);
            show_json_arc_animation(add_count, 1, 0);
        } else {
            show_json_arc_animation(add_count, 0, is_paused);
        }
        json_free(root);
    } else {
        show_json_arc_animation(add_count, 0, is_paused);
    }
    free(auto_name);
    return ret;
}

/* ========== SHOW RESULTS ========== */
static int show_results_general(const char *artist, const char *title, const char *url,
                                const char *mode, int is_similar) {
    (void)title;
    static int count = 0;
    count++;
    char *query = is_similar ? generate_similar_query(artist, count)
                             : generate_related_query(artist, count);
    printf("\n[+] Query: %s\n", query);
    tui_feed(query);
    free(query);
    return 0;
}

static int show_related_results(const char *url, const char *mode) {
    if (!url) return 1;
    char *artist = get_artist_from_url(url);
    return show_results_general(artist, NULL, url, mode, 0);
}

static int show_similar_results(const char *url, const char *mode) {
    if (!url) return 1;
    char *artist = get_artist_from_url(url);
    return show_results_general(artist, NULL, url, mode, 1);
}

/* ========== PLAYLIST ADD ========== */
static int playlist_add_with_name(json_value *root, const char *name, const char *url, const char *title) {
    if (!root || !name || !url) return -1;
    
    char *song_name = title ? strdup(title) : get_song_name_from_url(url);
    if (!song_name) song_name = strdup("Unknown");
    
    char *safe_url = safe_sanitize_url(url);
    
    json_value *pl = get_playlist(root, name);
    if (!pl) {
        pl = calloc(1, sizeof(json_value));
        pl->type = 'o';
        pl->string = strdup(name);
        json_value *items = calloc(1, sizeof(json_value));
        items->type = 'a';
        pl->child = items;
        if (!root->child) root->child = pl;
        else {
            json_value *last = root->child;
            while (last->next) last = last->next;
            last->next = pl;
        }
    }
    
    json_value *items = pl->child;
    if (!items || items->type != 'a') {
        items = calloc(1, sizeof(json_value));
        items->type = 'a';
        pl->child = items;
    }
    
    json_value *item = calloc(1, sizeof(json_value));
    item->type = 'o';
    
    json_value *url_node = calloc(1, sizeof(json_value));
    url_node->type = 's'; url_node->string = strdup("url");
    url_node->child = calloc(1, sizeof(json_value));
    url_node->child->type = 's'; url_node->child->string = strdup(safe_url);
    
    json_value *title_node = calloc(1, sizeof(json_value));
    title_node->type = 's'; title_node->string = strdup("title");
    title_node->child = calloc(1, sizeof(json_value));
    title_node->child->type = 's'; title_node->child->string = strdup(song_name);
    
    item->child = url_node;
    url_node->next = title_node;
    
    if (!items->child) items->child = item;
    else {
        json_value *last = items->child;
        while (last->next) last = last->next;
        last->next = item;
    }
    
    free(safe_url);
    free(song_name);
    return 0;
}

/* ========== PLAYLIST MGMT ========== */
static json_value *load_playlists(void) {
    char *path = get_config_path(PLAYLIST_FILE);
    json_value *root = NULL;
    if (file_exists(path)) {
        char *content = read_file(path);
        if (content) { root = json_parse(content); free(content); }
    }
    free(path);
    if (!root) {
        root = calloc(1, sizeof(json_value));
        root->type = 'o';
        json_value *dp = calloc(1, sizeof(json_value));
        dp->type = 'o'; dp->string = strdup("default");
        json_value *items = calloc(1, sizeof(json_value));
        items->type = 'a';
        dp->child = items;
        root->child = dp;
    }
    return root;
}

static int save_playlists(json_value *root) {
    if (!root) return -1;
    ensure_config_dir();
    char *path = get_config_path(PLAYLIST_FILE);
    char *json_str = json_generate(root);
    int result = write_file(path, json_str);
    free(json_str);
    free(path);
    return result;
}

static json_value *get_playlist(json_value *root, const char *name) {
    if (!root || !name) return NULL;
    json_value *child = root->child;
    while (child) {
        if (child->string && strcmp(child->string, name) == 0) return child;
        child = child->next;
    }
    return NULL;
}

static void playlist_list(json_value *root, const char *name) {
    json_value *pl = get_playlist(root, name);
    if (!pl) { printf("Playlist not found\n"); return; }
    printf("\nPlaylist: %s\n========================================\n", name);
    json_value *items = pl->child;
    int count = 0;
    if (items && items->type == 'a') {
        json_value *item = items->child;
        while (item) {
            json_value *title = NULL;
            json_value *child = item->child;
            while (child) {
                if (child->string && strcmp(child->string, "title") == 0) {
                    title = child->child; break;
                }
                child = child->next;
            }
            printf("%2d. %s\n", ++count, title && title->string ? title->string : "Unknown");
            item = item->next;
        }
    }
    if (count == 0) printf("  (empty)\n");
    printf("========================================\nTotal: %d\n", count);
}

/* ========== MPV IPC ========== */
static int mpv_is_running(void) {
    if (!file_exists(SOCKET_PATH)) return 0;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
    return ret == 0;
}

static int is_mpv_paused(const char *socket_path) {
    (void)socket_path;
    return 0;
}

/* ========== TUI FEED (FIXED) ========== */
static void tui_feed(const char *query) {
    if (!query || strlen(query) == 0) query = "trending music";
    has_chafa = check_chafa_exists();
    
    if (!has_chafa) {
        printf("\n[i] chafa not found. Install for thumbnails:\n");
        printf("    sudo apt install chafa\n\n");
    }
    
    // Fetch videos
    char search_url[256];
    snprintf(search_url, sizeof(search_url), "ytsearch%d:%s", MAX_SEARCH_RESULTS, query);
    
    char cmd[2048];
    json_value *config = load_config();
    char *browser = config ? get_config_string(config, "browser", "firefox") : strdup("firefox");
    snprintf(cmd, sizeof(cmd),
             "yt-dlp --cookies-from-browser %s --flat-playlist --no-cache-dir "
             "--print \"%%(title)s|%%(id)s|%%(channel)s|%%(duration_string)s|%%(view_count)s\" "
             "\"%s\" 2>/dev/null",
             browser, search_url);
    free(browser);
    if (config) json_free(config);
    
    char temp_file[256];
    snprintf(temp_file, sizeof(temp_file), "/tmp/raven_feed_%d.txt", getpid());
    char full_cmd[4096];
    snprintf(full_cmd, sizeof(full_cmd), "%s > %s 2>/dev/null", cmd, temp_file);
    
    printf("\n\033[1;36m[*] Loading feed: %s\033[0m\n", query);
    fflush(stdout);
    system(full_cmd);
    
    // Parse
    FILE *fp = fopen(temp_file, "r");
    if (!fp) { printf("Failed to read results\n"); return; }
    
    feed_item items[30];
    int count = 0;
    char line[1024];
    
    while (fgets(line, sizeof(line), fp) && count < 30) {
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;
        
        char *token = strtok(line, "|");
        if (!token) continue;
        strncpy(items[count].title, token, sizeof(items[count].title) - 1);
        
        token = strtok(NULL, "|");
        if (token) strncpy(items[count].id, token, sizeof(items[count].id) - 1);
        else continue;
        
        token = strtok(NULL, "|");
        if (token) strncpy(items[count].channel, token, sizeof(items[count].channel) - 1);
        else strcpy(items[count].channel, "Unknown");
        
        token = strtok(NULL, "|");
        if (token) strncpy(items[count].duration, token, sizeof(items[count].duration) - 1);
        else items[count].duration[0] = '\0';
        
        token = strtok(NULL, "|");
        if (token) strncpy(items[count].views, token, sizeof(items[count].views) - 1);
        else items[count].views[0] = '\0';
        
        items[count].thumbnail_path[0] = '\0';
        count++;
    }
    fclose(fp);
    unlink(temp_file);
    
    if (count == 0) { printf("No results found\n"); return; }
    
    // Download thumbnails
    if (has_chafa) {
        printf("\033[1;33m[*] Downloading thumbnails...\033[0m\n");
        for (int i = 0; i < count; i++) {
            char thumb_path[256];
            if (download_thumbnail(items[i].id, thumb_path, sizeof(thumb_path)) == 0) {
                strncpy(items[i].thumbnail_path, thumb_path, sizeof(items[i].thumbnail_path) - 1);
            }
            printf("\r  [%d/%d]", i + 1, count);
            fflush(stdout);
        }
        printf("\r  Done!                    \n");
    }
    
    // Main TUI loop
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    clear();
    
    int selected = 0;
    int ch;
    
    while (1) {
        clear();
        
        // Header
        attron(A_BOLD | A_REVERSE);
        mvprintw(0, 0, " RavenMPV Feed: %s ", query);
        int len = strlen(query) + 17;
        for (int i = len; i < COLS; i++) addch(' ');
        attroff(A_BOLD | A_REVERSE);
        
        // Layout
        int cols = 2;
        if (COLS < 60) cols = 1;
        
        int cell_w = COLS / cols;
        int cell_h = 14;
        int rows = (LINES - 4) / cell_h;
        if (rows < 1) rows = 1;
        int per_screen = cols * rows;
        
        int start = (selected / per_screen) * per_screen;
        
        for (int i = 0; i < per_screen && (i + start) < count; i++) {
            int idx = i + start;
            int row = i / cols;
            int col = i % cols;
            int x = col * cell_w + 1;
            int y = row * cell_h + 2;
            int is_sel = (idx == selected);
            
            int thumb_w = cell_w - 4;
            if (thumb_w > 40) thumb_w = 40;
            if (thumb_w < 20) thumb_w = 20;
            
            // Box
            if (is_sel) attron(A_REVERSE | A_BOLD);
            
            mvaddch(y, x, ACS_ULCORNER);
            for (int j = 0; j < thumb_w; j++) addch(ACS_HLINE);
            addch(ACS_URCORNER);
            
            for (int r = 1; r < 9; r++) {
                mvaddch(y + r, x, ACS_VLINE);
                for (int j = 0; j < thumb_w; j++) addch(' ');
                addch(ACS_VLINE);
            }
            
            mvaddch(y + 9, x, ACS_LLCORNER);
            for (int j = 0; j < thumb_w; j++) addch(ACS_HLINE);
            addch(ACS_LLCORNER == 0 ? '+' : ACS_LRCORNER);
            
            if (is_sel) attroff(A_REVERSE | A_BOLD);
            
            // Thumbnail
            if (has_chafa && strlen(items[idx].thumbnail_path) > 0) {
                draw_thumbnail(y + 1, x + 1, thumb_w, 8, items[idx].thumbnail_path);
            } else {
                mvprintw(y + 4, x + thumb_w / 2 - 3, " VIDEO ");
            }
            
            // Duration
            if (strlen(items[idx].duration) > 0) {
                attron(A_REVERSE);
                mvprintw(y + 8, x + thumb_w - strlen(items[idx].duration), 
                         " %s ", items[idx].duration);
                attroff(A_REVERSE);
            }
            
            // Title
            char disp[128];
            strncpy(disp, items[idx].title, thumb_w - 1);
            disp[thumb_w - 1] = '\0';
            if (is_sel) attron(A_BOLD);
            mvprintw(y + 10, x, "%s", disp);
            if (is_sel) attroff(A_BOLD);
            
            // Channel
            strncpy(disp, items[idx].channel, thumb_w - 1);
            disp[thumb_w - 1] = '\0';
            mvprintw(y + 11, x, "%s", disp);
        }
        
        // Footer
        attron(A_REVERSE);
        mvprintw(LINES - 1, 0, " Arrows: Navigate | ENTER: Play | a: Add | s: Search | q: Quit ");
        for (int i = 65; i < COLS; i++) addch(' ');
        attroff(A_REVERSE);
        
        mvprintw(LINES - 2, COLS - 20, "Page %d/%d | %d/%d",
                 (selected / per_screen) + 1,
                 ((count - 1) / per_screen) + 1,
                 selected + 1, count);
        
        refresh();
        ch = getch();
        
        if (ch == 'q' || ch == 27) break;
        else if (ch == KEY_RIGHT) { if (selected < count - 1) selected++; }
        else if (ch == KEY_LEFT) { if (selected > 0) selected--; }
        else if (ch == KEY_DOWN) { selected += cols; if (selected >= count) selected = count - 1; }
        else if (ch == KEY_UP) { selected -= cols; if (selected < 0) selected = 0; }
        else if (ch == KEY_NPAGE) { selected += per_screen; if (selected >= count) selected = count - 1; }
        else if (ch == KEY_PPAGE) { selected -= per_screen; if (selected < 0) selected = 0; }
        else if (ch == 'a' || ch == 'A') {
            endwin();
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "https://youtu.be/%s", items[selected].id);
            add_to_playlist_with_json_arc(url_buf, items[selected].title);
            printf("\nPress Enter...");
            getchar();
            initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); clear();
        }
        else if (ch == 's' || ch == 'S') {
            endwin();
            printf("Search: ");
            char q[256];
            if (fgets(q, sizeof(q), stdin)) {
                q[strcspn(q, "\n")] = '\0';
                if (strlen(q) > 0) tui_feed(q);
            }
            return;
        }
        else if (ch == 10 || ch == KEY_ENTER) {
            endwin();
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "https://youtu.be/%s", items[selected].id);
            printf("Playing: %s\n", items[selected].title);
            int ret = play_with_pipe(url_buf, "video");
            if (ret == 0) show_play_more_prompt(url_buf, "video");
            break;
        }
    }
    
    endwin();
    
    // Cleanup
    for (int i = 0; i < count; i++) {
        if (strlen(items[i].thumbnail_path) > 0) unlink(items[i].thumbnail_path);
    }
}

/* ========== TUI SEARCH (FIXED) ========== */
static void tui_search(const char *query) {
    if (!query || strlen(query) == 0) return;
    
    char search_url[256];
    snprintf(search_url, sizeof(search_url), "ytsearch%d:%s", MAX_SEARCH_RESULTS, query);
    
    char cmd[1024];
    json_value *config = load_config();
    char *browser = config ? get_config_string(config, "browser", "firefox") : strdup("firefox");
    snprintf(cmd, sizeof(cmd),
             "yt-dlp --cookies-from-browser %s --flat-playlist --no-cache-dir "
             "--get-title --get-id \"%s\" 2>/dev/null",
             browser, search_url);
    free(browser);
    if (config) json_free(config);
    
    char temp_file[256];
    snprintf(temp_file, sizeof(temp_file), "/tmp/raven_search_%d.txt", getpid());
    char full_cmd[2048];
    snprintf(full_cmd, sizeof(full_cmd), "%s > %s 2>/dev/null", cmd, temp_file);
    
    printf("\nSearching for '%s'...\n", query);
    system(full_cmd);
    
    FILE *fp = fopen(temp_file, "r");
    if (!fp) { printf("Search failed\n"); return; }
    
    char titles[30][256], ids[30][64];
    int count = 0;
    char line[512];
    int is_title = 1;
    char current[256] = {0};
    
    while (fgets(line, sizeof(line), fp) && count < MAX_SEARCH_RESULTS) {
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;
        if (is_title) {
            strncpy(current, line, sizeof(current) - 1);
            is_title = 0;
        } else {
            strncpy(titles[count], current, sizeof(titles[count]) - 1);
            strncpy(ids[count], line, sizeof(ids[count]) - 1);
            count++;
            is_title = 1;
        }
    }
    fclose(fp);
    unlink(temp_file);
    
    if (count == 0) { printf("No results\n"); return; }
    
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    clear();
    
    int selected = 0;
    int ch;
    int max_rows = LINES - 6;
    
    while (1) {
        clear();
        
        attron(A_BOLD | A_REVERSE);
        mvprintw(0, 0, " Results: %s ", query);
        for (int i = strlen(query) + 12; i < COLS; i++) addch(' ');
        attroff(A_BOLD | A_REVERSE);
        
        mvprintw(1, 0, "────────────────────────────────────────────────────");
        mvprintw(2, 0, " ENTER: Play  |  a: Add  |  q: Quit");
        mvprintw(3, 0, "────────────────────────────────────────────────────");
        
        int start = selected - (max_rows / 2);
        if (start < 0) start = 0;
        if (start > count - max_rows) start = count - max_rows;
        if (start < 0) start = 0;
        
        for (int i = 0; i < max_rows && (i + start) < count; i++) {
            int idx = i + start;
            char disp[256];
            int max_len = COLS - 6;
            if ((int)strlen(titles[idx]) > max_len) {
                strncpy(disp, titles[idx], max_len - 3);
                disp[max_len - 3] = '\0';
                strcat(disp, "...");
            } else strcpy(disp, titles[idx]);
            
            if (idx == selected) attron(A_REVERSE);
            mvprintw(i + 4, 0, " %c %2d. %s", (idx == selected) ? '>' : ' ', idx + 1, disp);
            if (idx == selected) attroff(A_REVERSE);
        }
        
        for (int i = max_rows + 4; i < LINES - 1; i++)
            mvprintw(i, 0, "%-*s", COLS - 1, " ");
        
        mvprintw(LINES - 1, 0, " %d/%d | ENTER: Play", selected + 1, count);
        refresh();
        
        ch = getch();
        
        if (ch == 'q' || ch == 27) break;
        else if (ch == KEY_DOWN) { if (selected < count - 1) selected++; }
        else if (ch == KEY_UP) { if (selected > 0) selected--; }
        else if (ch == 'a' || ch == 'A') {
            endwin();
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "https://youtu.be/%s", ids[selected]);
            add_to_playlist_with_json_arc(url_buf, titles[selected]);
            printf("\nPress Enter...");
            getchar();
            initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); clear();
        }
        else if (ch == 10 || ch == KEY_ENTER) {
            endwin();
            char url_buf[256];
            snprintf(url_buf, sizeof(url_buf), "https://youtu.be/%s", ids[selected]);
            printf("Playing: %s\n", titles[selected]);
            int ret = play_with_pipe(url_buf, "video");
            if (ret == 0) show_play_more_prompt(url_buf, "video");
            break;
        }
    }
    endwin();
}

/* ========== TUI PLAYLIST ========== */
static void show_tui_playlist(json_value *root, const char *name) {
    json_value *pl = get_playlist(root, name);
    if (!pl) { printf("Playlist '%s' not found\n", name); return; }
    
    json_value *items = pl->child;
    if (!items || items->type != 'a' || !items->child) {
        printf("Playlist is empty\n"); return;
    }
    
    int total = 0;
    json_value *tmp = items->child;
    while (tmp) { total++; tmp = tmp->next; }
    
    char **titles = malloc(total * sizeof(char*));
    char **urls = malloc(total * sizeof(char*));
    
    int idx = 0;
    json_value *item = items->child;
    while (item && idx < total) {
        json_value *t = NULL, *u = NULL;
        json_value *child = item->child;
        while (child) {
            if (child->string && strcmp(child->string, "title") == 0) t = child->child;
            if (child->string && strcmp(child->string, "url") == 0) u = child->child;
            child = child->next;
        }
        titles[idx] = t && t->string ? strdup(t->string) : strdup("Unknown");
        urls[idx] = u && u->string ? strdup(u->string) : strdup("");
        idx++;
        item = item->next;
    }
    
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); clear();
    
    int selected = 0;
    int ch;
    int max_rows = LINES - 6;
    
    while (1) {
        clear();
        
        attron(A_BOLD | A_REVERSE);
        mvprintw(0, 0, " RavenMPV - Playlist: %s ", name);
        for (int i = strlen(name) + 20; i < COLS; i++) addch(' ');
        attroff(A_BOLD | A_REVERSE);
        
        mvprintw(1, 0, "────────────────────────────────────────────────────");
        mvprintw(2, 0, " ENTER: Play  |  s: Search  |  f: Feed  |  q: Quit");
        mvprintw(3, 0, "────────────────────────────────────────────────────");
        
        int start = selected - (max_rows / 2);
        if (start < 0) start = 0;
        if (start > total - max_rows) start = total - max_rows;
        if (start < 0) start = 0;
        
        for (int i = 0; i < max_rows && (i + start) < total; i++) {
            int idx2 = i + start;
            char disp[256];
            int max_len = COLS - 8;
            if ((int)strlen(titles[idx2]) > max_len) {
                strncpy(disp, titles[idx2], max_len - 3);
                disp[max_len - 3] = '\0';
                strcat(disp, "...");
            } else strcpy(disp, titles[idx2]);
            
            if (idx2 == selected) attron(A_REVERSE);
            mvprintw(i + 4, 0, " %c %2d. %s", (idx2 == selected) ? '>' : ' ', idx2 + 1, disp);
            if (idx2 == selected) attroff(A_REVERSE);
        }
        
        for (int i = max_rows + 4; i < LINES - 1; i++)
            mvprintw(i, 0, "%-*s", COLS - 1, " ");
        
        mvprintw(LINES - 1, 0, " Total: %d | %d/%d", total, selected + 1, total);
        refresh();
        
        ch = getch();
        
        if (ch == 'q' || ch == 27) break;
        else if (ch == KEY_DOWN) { if (selected < total - 1) selected++; }
        else if (ch == KEY_UP) { if (selected > 0) selected--; }
        else if (ch == 's' || ch == 'S') {
            endwin();
            printf("Search: ");
            char q[256];
            if (fgets(q, sizeof(q), stdin)) {
                q[strcspn(q, "\n")] = '\0';
                if (strlen(q) > 0) tui_search(q);
            }
            initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); clear();
        }
        else if (ch == 'f' || ch == 'F') {
            endwin();
            printf("Feed: ");
            char q[256];
            if (fgets(q, sizeof(q), stdin)) {
                q[strcspn(q, "\n")] = '\0';
                tui_feed(strlen(q) > 0 ? q : "trending music");
            }
            initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); clear();
        }
        else if (ch == 10 || ch == KEY_ENTER) {
            endwin();
            printf("Playing: %s\n", urls[selected]);
            int ret = play_with_pipe(urls[selected], "video");
            if (ret == 0) show_play_more_prompt(urls[selected], "video");
            initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); clear();
        }
    }
    endwin();
    
    for (int i = 0; i < total; i++) { free(titles[i]); free(urls[i]); }
    free(titles); free(urls);
}

/* ========== HELPER ========== */
static char *get_playlist_url_by_index(int index) {
    if (index < 0) return NULL;
    json_value *root = load_playlists();
    if (!root) return NULL;
    json_value *pl = get_playlist(root, "default");
    if (!pl) { json_free(root); return NULL; }
    json_value *items = pl->child;
    if (!items || items->type != 'a') { json_free(root); return NULL; }
    json_value *item = items->child;
    int i = 0;
    while (item && i < index) { item = item->next; i++; }
    if (!item) { json_free(root); return NULL; }
    json_value *url = NULL;
    json_value *child = item->child;
    while (child) {
        if (child->string && strcmp(child->string, "url") == 0) { url = child->child; break; }
        child = child->next;
    }
    if (!url || !url->string) { json_free(root); return NULL; }
    char *result = strdup(url->string);
    json_free(root);
    return result;
}

/* ========== COMMANDS ========== */
static int cmd_feed(int argc, char **argv) {
    tui_feed((argc > 0) ? argv[0] : "trending music");
    return 0;
}

static int cmd_search(int argc, char **argv) {
    if (argc < 1) { printf("Usage: search <query>\n"); return 1; }
    tui_search(argv[0]);
    return 0;
}

static int cmd_play(int argc, char **argv) {
    if (argc < 1) { printf("Usage: play <url|number>\n"); return 1; }
    
    int audio = 0;
    char *url = argv[0];
    if (argc >= 2 && strcmp(argv[0], "audio") == 0) { audio = 1; url = argv[1]; }
    
    int is_num = 1;
    for (size_t i = 0; url[i]; i++) if (!isdigit(url[i])) { is_num = 0; break; }
    
    if (is_num) {
        int index = atoi(url) - 1;
        char *play_url = get_playlist_url_by_index(index);
        if (!play_url) { printf("Not found\n"); return 1; }
        int ret = play_with_pipe(play_url, audio ? "audio" : "video");
        if (ret == 0) show_play_more_prompt(play_url, audio ? "audio" : "video");
        free(play_url);
        return ret;
    }
    
    char *search_url = NULL;
    if (strstr(url, "http") != url) {
        search_url = malloc(strlen(url) + 15);
        sprintf(search_url, "ytsearch1:%s", url);
        url = search_url;
    }
    
    int ret = play_with_pipe(url, audio ? "audio" : "video");
    if (ret == 0) show_play_more_prompt(url, audio ? "audio" : "video");
    if (search_url) free(search_url);
    return ret;
}

static int cmd_add(int argc, char **argv) {
    if (argc < 1) { printf("Usage: add <url>\n"); return 1; }
    char *name = get_song_name_from_url(argv[0]);
    add_to_playlist_with_json_arc(argv[0], name ? name : "Unknown");
    free(name);
    return 0;
}

static int cmd_tui(int argc, char **argv) {
    char *name = (argc > 0) ? argv[0] : "default";
    json_value *root = load_playlists();
    if (!root) return 1;
    show_tui_playlist(root, name);
    json_free(root);
    return 0;
}

static int cmd_list(int argc, char **argv) {
    json_value *root = load_playlists();
    if (!root) return 1;
    playlist_list(root, (argc > 0) ? argv[0] : "default");
    json_free(root);
    return 0;
}

static int cmd_status(void) {
    printf("RavenMPV %s\n", VERSION);
    printf("chafa: %s\n", check_chafa_exists() ? "installed" : "not installed");
    printf("mpv: %s\n", system("which mpv >/dev/null 2>&1") == 0 ? "installed" : "not installed");
    printf("yt-dlp: %s\n", system("which yt-dlp >/dev/null 2>&1") == 0 ? "installed" : "not installed");
    return 0;
}

static int cmd_config(int argc, char **argv) {
    if (argc < 1) { printf("Usage: config <get|set|list>\n"); return 1; }
    json_value *root = load_config();
    if (!root) return 1;
    
    if (strcmp(argv[0], "list") == 0) {
        json_value *mpv = json_find(root, "mpv");
        if (mpv) {
            json_value *child = mpv->child;
            while (child) {
                if (child->string && child->child) {
                    char *val = json_generate(child->child);
                    printf("%s = %s\n", child->string, val);
                    free(val);
                }
                child = child->next;
            }
        }
    } else if (strcmp(argv[0], "set") == 0 && argc >= 3) {
        json_value *mpv = json_find(root, "mpv");
        if (!mpv) {
            mpv = calloc(1, sizeof(json_value));
            mpv->type = 'o'; mpv->string = strdup("mpv");
            root->child = mpv;
        }
        json_value *existing = json_find(mpv, argv[1]);
        if (existing) {
            if (existing->string) free(existing->string);
            existing->string = strdup(argv[2]);
        }
        save_config(root);
        printf("Set %s = %s\n", argv[1], argv[2]);
    }
    json_free(root);
    return 0;
}

static void print_help(void) {
    printf("RavenMPV %s\n\n", VERSION);
    printf("Commands:\n");
    printf("  feed [query]      YouTube feed with thumbnails\n");
    printf("  search <query>    Search YouTube\n");
    printf("  play <url|num>    Play video\n");
    printf("  play audio <url>  Play audio only\n");
    printf("  add <url>         Add to playlist\n");
    printf("  tui               TUI playlist\n");
    printf("  list              List playlist\n");
    printf("  status            Show status\n");
    printf("  config <set>      Manage config\n\n");
    printf("Install chafa for thumbnails: sudo apt install chafa\n");
}

/* ========== MAIN ========== */
int main(int argc, char *argv[]) {
    srand(time(NULL));
    signal(SIGSEGV, segfault_handler);
    
    if (argc >= 2 && strcmp(argv[1], "--debug") == 0) {
        debug_mode = 1;
        argc--; argv++;
    }
    
    if (argc < 2) { print_help(); return 1; }
    
    char *cmd = argv[1];
    
    if (strcmp(cmd, "feed") == 0) return cmd_feed(argc - 2, argv + 2);
    else if (strcmp(cmd, "search") == 0) return cmd_search(argc - 2, argv + 2);
    else if (strcmp(cmd, "play") == 0) return cmd_play(argc - 2, argv + 2);
    else if (strcmp(cmd, "add") == 0) return cmd_add(argc - 2, argv + 2);
    else if (strcmp(cmd, "tui") == 0) return cmd_tui(argc - 2, argv + 2);
    else if (strcmp(cmd, "list") == 0) return cmd_list(argc - 2, argv + 2);
    else if (strcmp(cmd, "status") == 0) return cmd_status();
    else if (strcmp(cmd, "config") == 0) return cmd_config(argc - 2, argv + 2);
    else { print_help(); return 1; }
}
