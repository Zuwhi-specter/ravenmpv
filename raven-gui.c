 /*
 * RavenMPV - Single File GUI Edition
 * Version: 1.0.0
 * License: MIT
 * 
 * All-in-one YouTube feed + player with GTK4
 * - Engine: JSON, playlist, config, play (built-in)
 * - UI: GTK4 window with feed grid
 * 
 * Compile:
 *   gcc -O2 -o raven-gui raven_gui.c $(pkg-config --cflags --libs gtk4) -lcurl -lpthread
 * 
 * Dependencies:
 *   Ubuntu: sudo apt install libgtk-4-dev libcurl4-openssl-dev
 *   Arch:   sudo pacman -S gtk4 curl
 *   Fedora: sudo dnf install gtk4-devel libcurl-devel
 */

#include <gtk/gtk.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <time.h>

/* ========== CONFIG ========== */
#define APP_ID "com.ravenmpv.gui"
#define VERSION "1.0.0"
#define CONFIG_DIR ".config/ravenmpv"
#define THUMB_DIR "/tmp/raven_thumbs"
#define FEED_FILE "/tmp/raven_gui_feed.txt"
#define PLAYLIST_FILE "playlists.json"
#define MAX_ITEMS 30

/* ========== DATA STRUCTURES ========== */
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
} video_item;

typedef struct {
    video_item items[MAX_ITEMS];
    int count;
} feed_data;

typedef struct {
    char query[256];
} fetch_data;

/* ========== GLOBAL WIDGETS ========== */
static GtkWidget *main_window;
static GtkWidget *flow_box;
static GtkWidget *status_label;
static GtkWidget *search_entry;
static feed_data current_feed;
static int feed_loading = 0;

/* ============================================================ */
/*                    ENGINE (from CLI)                          */
/* ============================================================ */

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

static void ensure_config_dir(void) {
    char *home = getenv("HOME");
    if (!home) return;
    char *path = malloc(strlen(home) + strlen(CONFIG_DIR) + 2);
    sprintf(path, "%s/%s", home, CONFIG_DIR);
    struct stat st;
    if (stat(path, &st) != 0) mkdir(path, 0755);
    free(path);
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

/* ========== PLAYLIST ENGINE ========== */
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

static int add_to_playlist_engine(const char *url, const char *title) {
    json_value *root = load_playlists();
    if (!root) return -1;
    
    char *song_name = title ? strdup(title) : strdup("Unknown");
    
    json_value *pl = get_playlist(root, "default");
    if (!pl) {
        pl = calloc(1, sizeof(json_value));
        pl->type = 'o';
        pl->string = strdup("default");
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
    url_node->child->type = 's'; url_node->child->string = strdup(url);
    
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
    
    int result = save_playlists(root);
    json_free(root);
    free(song_name);
    return result;
}

/* ========== PLAY ENGINE ========== */
static int play_engine(const char *video_id) {
    if (!video_id) return -1;
    
    /* Fork a background process that runs yt-dlp | mpv */
    pid_t pid = fork();
    if (pid < 0) return -1;
    
    if (pid == 0) {
        /* Child process - detach from GUI */
        setsid();
        
        /* Redirect stdin/stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "yt-dlp "
            "-f 'bestvideo*[height<=1080]+bestaudio/best' "
            "--extractor-args 'youtube:player_client=android,ios,web' "
            "'https://youtu.be/%s' "
            "-o - 2>/dev/null | "
            "mpv --cache=yes --really-quiet --no-osd --no-terminal "
            "--keep-open=no --profile=fast --hwdec=auto - 2>/dev/null",
            video_id);
        
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }
    
    return 0;
}

/* ============================================================ */
/*                    UI (GTK4)                                  */
/* ============================================================ */

/* ========== UTILITIES ========== */
static void ensure_thumb_dir(void) {
    struct stat st;
    if (stat(THUMB_DIR, &st) != 0) mkdir(THUMB_DIR, 0755);
}

static long parse_views(const char *views_str) {
    if (!views_str || strlen(views_str) == 0 || strcmp(views_str, "NA") == 0)
        return 0;
    return atol(views_str);
}

static void format_views(long views, char *output, size_t size) {
    if (views >= 1000000000)
        snprintf(output, size, "%.1fB views", views / 1000000000.0);
    else if (views >= 1000000)
        snprintf(output, size, "%.1fM views", views / 1000000.0);
    else if (views >= 1000)
        snprintf(output, size, "%.1fK views", views / 1000.0);
    else if (views > 0)
        snprintf(output, size, "%ld views", views);
    else
        snprintf(output, size, "");
}

static void truncate_text(const char *input, char *output, size_t max_len) {
    size_t len = strlen(input);
    if (len <= max_len) {
        strncpy(output, input, max_len);
        output[max_len] = '\0';
    } else {
        strncpy(output, input, max_len - 3);
        output[max_len - 3] = '\0';
        strcat(output, "...");
    }
}

/* ========== DOWNLOAD THUMBNAIL ========== */
static size_t write_callback(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static int download_file(const char *url, const char *output_path) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    
    FILE *fp = fopen(output_path, "wb");
    if (!fp) { curl_easy_cleanup(curl); return -1; }
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    
    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);
    
    if (res == CURLE_OK) {
        struct stat st;
        if (stat(output_path, &st) == 0 && st.st_size > 1000)
            return 0;
    }
    
    unlink(output_path);
    return -1;
}

/* ========== ACTIONS ========== */
static void on_play_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    video_item *item = (video_item *)user_data;
    
    /* Call engine directly - no CLI needed */
    play_engine(item->id);
    
    char msg[512];
    snprintf(msg, sizeof(msg), "Playing: %s", item->title);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
}

static void on_add_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    video_item *item = (video_item *)user_data;
    
    char url[256];
    snprintf(url, sizeof(url), "https://youtu.be/%s", item->id);
    
    /* Call engine directly */
    add_to_playlist_engine(url, item->title);
    
    char msg[512];
    snprintf(msg, sizeof(msg), "Added to playlist: %s", item->title);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
}

static void on_copy_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    video_item *item = (video_item *)user_data;
    
    char url[256];
    snprintf(url, sizeof(url), "https://youtu.be/%s", item->id);
    
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(clipboard, url);
    
    gtk_label_set_text(GTK_LABEL(status_label), "URL copied to clipboard");
}

/* ========== CREATE VIDEO CARD ========== */
static GtkWidget *create_video_card(video_item *item) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(card, 320, 340);
    gtk_widget_add_css_class(card, "video-card");
    
    /* Thumbnail overlay */
    GtkWidget *thumb_overlay = gtk_overlay_new();
    gtk_widget_set_size_request(thumb_overlay, 300, 170);
    gtk_widget_add_css_class(thumb_overlay, "thumbnail");
    
    GtkWidget *image;
    if (strlen(item->thumbnail_path) > 0 && access(item->thumbnail_path, F_OK) == 0) {
        image = gtk_image_new_from_file(item->thumbnail_path);
    } else {
        image = gtk_image_new_from_icon_name("video-x-generic-symbolic");
    }
    gtk_image_set_pixel_size(GTK_IMAGE(image), 300);
    gtk_widget_set_size_request(image, 300, 170);
    gtk_overlay_set_child(GTK_OVERLAY(thumb_overlay), image);
    
    /* Duration badge */
    if (strlen(item->duration) > 0 && strcmp(item->duration, "NA") != 0) {
        GtkWidget *duration = gtk_label_new(item->duration);
        gtk_widget_add_css_class(duration, "duration-badge");
        gtk_widget_set_halign(duration, GTK_ALIGN_END);
        gtk_widget_set_valign(duration, GTK_ALIGN_END);
        gtk_widget_set_margin_end(duration, 8);
        gtk_widget_set_margin_bottom(duration, 8);
        gtk_overlay_add_overlay(GTK_OVERLAY(thumb_overlay), duration);
    }
    
    gtk_box_append(GTK_BOX(card), thumb_overlay);
    
    /* Title */
    char title_trunc[100];
    truncate_text(item->title, title_trunc, 80);
    GtkWidget *title = gtk_label_new(title_trunc);
    gtk_label_set_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 40);
    gtk_label_set_lines(GTK_LABEL(title), 2);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "video-title");
    gtk_box_append(GTK_BOX(card), title);
    
    /* Channel */
    GtkWidget *channel = gtk_label_new(item->channel);
    gtk_label_set_xalign(GTK_LABEL(channel), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(channel), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(channel, "channel-name");
    gtk_box_append(GTK_BOX(card), channel);
    
    /* Views */
    char views_buf[64];
    format_views(parse_views(item->views), views_buf, sizeof(views_buf));
    GtkWidget *views = gtk_label_new(views_buf);
    gtk_label_set_xalign(GTK_LABEL(views), 0.0);
    gtk_widget_add_css_class(views, "view-count");
    gtk_box_append(GTK_BOX(card), views);
    
    /* Buttons */
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(buttons, 4);
    
    GtkWidget *play_btn = gtk_button_new_with_label("Play");
    gtk_widget_add_css_class(play_btn, "play-btn");
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play_clicked), item);
    gtk_box_append(GTK_BOX(buttons), play_btn);
    
    GtkWidget *add_btn = gtk_button_new_with_label("+ Add");
    gtk_widget_add_css_class(add_btn, "add-btn");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_clicked), item);
    gtk_box_append(GTK_BOX(buttons), add_btn);
    
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_widget_add_css_class(copy_btn, "copy-btn");
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_copy_clicked), item);
    gtk_box_append(GTK_BOX(buttons), copy_btn);
    
    gtk_box_append(GTK_BOX(card), buttons);
    
    return card;
}

/* ========== POPULATE FEED ========== */
static void populate_feed(void) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(flow_box)) != NULL) {
        gtk_flow_box_remove(GTK_FLOW_BOX(flow_box), child);
    }
    
    for (int i = 0; i < current_feed.count; i++) {
        GtkWidget *card = create_video_card(&current_feed.items[i]);
        gtk_flow_box_append(GTK_FLOW_BOX(flow_box), card);
    }
    
    GtkWidget *scrolled = gtk_widget_get_ancestor(flow_box, GTK_TYPE_SCROLLED_WINDOW);
    if (scrolled) {
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
        gtk_adjustment_set_value(vadj, 0);
    }
}

/* ========== PARSE FEED ========== */
static int parse_feed_file(void) {
    FILE *fp = fopen(FEED_FILE, "r");
    if (!fp) return 0;
    
    current_feed.count = 0;
    char line[4096];
    
    while (fgets(line, sizeof(line), fp) && current_feed.count < MAX_ITEMS) {
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;
        
        video_item *item = &current_feed.items[current_feed.count];
        memset(item, 0, sizeof(video_item));
        
        char *saveptr;
        char *token = strtok_r(line, "|", &saveptr);
        if (!token) continue;
        strncpy(item->title, token, sizeof(item->title) - 1);
        
        token = strtok_r(NULL, "|", &saveptr);
        if (!token) continue;
        strncpy(item->id, token, sizeof(item->id) - 1);
        
        token = strtok_r(NULL, "|", &saveptr);
        if (token) strncpy(item->channel, token, sizeof(item->channel) - 1);
        else strcpy(item->channel, "Unknown");
        
        token = strtok_r(NULL, "|", &saveptr);
        if (token) strncpy(item->duration, token, sizeof(item->duration) - 1);
        
        token = strtok_r(NULL, "|", &saveptr);
        if (token) strncpy(item->views, token, sizeof(item->views) - 1);
        
        /* Download thumbnail */
        snprintf(item->thumbnail_path, sizeof(item->thumbnail_path),
                 THUMB_DIR "/%s.jpg", item->id);
        
        if (access(item->thumbnail_path, F_OK) != 0) {
            char url[512];
            snprintf(url, sizeof(url),
                     "https://img.youtube.com/vi/%s/mqdefault.jpg", item->id);
            download_file(url, item->thumbnail_path);
        }
        
        current_feed.count++;
    }
    
    fclose(fp);
    unlink(FEED_FILE);
    return current_feed.count;
}

/* ========== ASYNC FETCH ========== */
static gboolean on_fetch_done(gpointer user_data) {
    fetch_data *data = (fetch_data *)user_data;
    
    int count = parse_feed_file();
    populate_feed();
    
    char msg[512];
    if (count > 0)
        snprintf(msg, sizeof(msg), "Loaded %d videos for \"%s\"", count, data->query);
    else
        snprintf(msg, sizeof(msg), "No results for \"%s\"", data->query);
    
    gtk_label_set_text(GTK_LABEL(status_label), msg);
    gtk_widget_set_sensitive(search_entry, TRUE);
    feed_loading = 0;
    
    free(data);
    return G_SOURCE_REMOVE;
}

static gpointer fetch_feed_thread(gpointer user_data) {
    fetch_data *data = (fetch_data *)user_data;
    
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp --flat-playlist --no-cache-dir --no-warnings "
             "--print \"%%(title)s|%%(id)s|%%(channel)s|%%(duration_string)s|%%(view_count)s\" "
             "\"ytsearch20:%s\" > %s 2>/dev/null",
             data->query, FEED_FILE);
    system(cmd);
    
    g_idle_add(on_fetch_done, data);
    return NULL;
}

static void search_feed(const char *query) {
    if (feed_loading) return;
    if (!query || strlen(query) == 0) return;
    
    feed_loading = 1;
    gtk_widget_set_sensitive(search_entry, FALSE);
    
    char msg[512];
    snprintf(msg, sizeof(msg), "Loading \"%s\"...", query);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
    
    fetch_data *data = malloc(sizeof(fetch_data));
    if (!data) return;
    memset(data, 0, sizeof(fetch_data));
    strncpy(data->query, query, sizeof(data->query) - 1);
    
    g_thread_new("fetch", fetch_feed_thread, data);
}

/* ========== CALLBACKS ========== */
static void on_search_activate(GtkEntry *entry, gpointer user_data) {
    (void)user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (query && strlen(query) > 0)
        search_feed(query);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (query && strlen(query) > 0)
        search_feed(query);
}

/* ========== CSS ========== */
static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    
    const char *css =
        "window {"
        "  background-color: #0f0f0f;"
        "}"
        "headerbar {"
        "  background-color: #1c1c1c;"
        "  color: #ffffff;"
        "  border-bottom: 1px solid #2a2a2a;"
        "  min-height: 56px;"
        "}"
        "headerbar button {"
        "  background-color: transparent;"
        "  border: none;"
        "  color: #ffffff;"
        "  border-radius: 8px;"
        "  padding: 8px;"
        "  min-width: 32px;"
        "}"
        "headerbar button:hover {"
        "  background-color: #2a2a2a;"
        "}"
        "entry {"
        "  background-color: #1c1c1c;"
        "  color: #ffffff;"
        "  border-radius: 20px;"
        "  padding: 10px 20px;"
        "  border: 1px solid #333;"
        "  font-size: 14px;"
        "  min-height: 24px;"
        "}"
        "entry:focus {"
        "  border-color: #ff0000;"
        "}"
        ".video-card {"
        "  background-color: #1c1c1c;"
        "  border-radius: 12px;"
        "  padding: 12px;"
        "}"
        ".video-card:hover {"
        "  background-color: #2a2a2a;"
        "}"
        ".thumbnail {"
        "  border-radius: 8px;"
        "  background-color: #0f0f0f;"
        "}"
        ".duration-badge {"
        "  background-color: rgba(0,0,0,0.8);"
        "  color: #ffffff;"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "  padding: 2px 6px;"
        "  border-radius: 4px;"
        "}"
        ".video-title {"
        "  color: #ffffff;"
        "  font-weight: bold;"
        "  font-size: 14px;"
        "}"
        ".channel-name {"
        "  color: #aaaaaa;"
        "  font-size: 13px;"
        "}"
        ".view-count {"
        "  color: #777777;"
        "  font-size: 12px;"
        "}"
        ".play-btn {"
        "  background-color: #ff0000;"
        "  color: #ffffff;"
        "  border-radius: 18px;"
        "  padding: 6px 14px;"
        "  font-weight: bold;"
        "  font-size: 13px;"
        "  border: none;"
        "  min-height: 24px;"
        "}"
        ".play-btn:hover {"
        "  background-color: #cc0000;"
        "}"
        ".add-btn {"
        "  background-color: #2a2a2a;"
        "  color: #ffffff;"
        "  border-radius: 18px;"
        "  padding: 6px 14px;"
        "  font-weight: bold;"
        "  font-size: 13px;"
        "  border: none;"
        "  min-height: 24px;"
        "}"
        ".add-btn:hover {"
        "  background-color: #3a3a3a;"
        "}"
        ".copy-btn {"
        "  background-color: #2a2a2a;"
        "  color: #ffffff;"
        "  border-radius: 18px;"
        "  padding: 6px 10px;"
        "  font-size: 13px;"
        "  border: none;"
        "  min-height: 24px;"
        "}"
        ".copy-btn:hover {"
        "  background-color: #3a3a3a;"
        "}"
        ".status-bar {"
        "  background-color: #1c1c1c;"
        "  border-top: 1px solid #2a2a2a;"
        "  min-height: 36px;"
        "}"
        ".status-bar label {"
        "  color: #aaaaaa;"
        "  font-size: 13px;"
        "}"
        "scrolledwindow {"
        "  background-color: #0f0f0f;"
        "}"
        "scrollbar {"
        "  background-color: transparent;"
        "  border: none;"
        "}"
        "scrollbar slider {"
        "  background-color: #3a3a3a;"
        "  border-radius: 4px;"
        "  min-width: 8px;"
        "  min-height: 8px;"
        "}";
    
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    
    g_object_unref(provider);
}

/* ========== ACTIVATE ========== */
static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    
    ensure_thumb_dir();
    ensure_config_dir();
    
    /* Main window */
    main_window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(main_window), "RavenMPV Feed");
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1200, 800);
    
    /* Header bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(main_window), header);
    
    GtkWidget *title_label = gtk_label_new("RavenMPV Feed");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_label);
    
    /* Search entry */
    search_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry), "Search YouTube...");
    gtk_widget_set_size_request(search_entry, 400, -1);
    g_signal_connect(search_entry, "activate", G_CALLBACK(on_search_activate), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), search_entry);
    
    /* Refresh button */
    GtkWidget *refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_btn, "Refresh");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), refresh_btn);
    
    /* Main layout */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(main_window), main_box);
    
    /* Scrolled window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(main_box), scrolled);
    
    /* Flow box */
    flow_box = gtk_flow_box_new();
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow_box), FALSE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow_box), 16);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow_box), 16);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box), 4);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow_box), 1);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box), GTK_SELECTION_NONE);
    gtk_widget_set_margin_start(flow_box, 20);
    gtk_widget_set_margin_end(flow_box, 20);
    gtk_widget_set_margin_top(flow_box, 20);
    gtk_widget_set_margin_bottom(flow_box, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), flow_box);
    
    /* Status bar */
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(status_box, "status-bar");
    gtk_box_append(GTK_BOX(main_box), status_box);
    
    status_label = gtk_label_new("Ready. Loading trending videos...");
    gtk_widget_set_halign(status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(status_label, 20);
    gtk_widget_set_margin_end(status_label, 20);
    gtk_widget_set_margin_top(status_label, 8);
    gtk_widget_set_margin_bottom(status_label, 8);
    gtk_box_append(GTK_BOX(status_box), status_label);
    
    load_css();
    gtk_window_present(GTK_WINDOW(main_window));
    
    /* Load default feed */
    search_feed("trending music 2026");
}

/* ========== MAIN ========== */
int main(int argc, char *argv[]) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    
    g_object_unref(app);
    curl_global_cleanup();
    
    return status;
}
