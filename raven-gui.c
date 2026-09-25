/*
 * RavenMPV GUI - GTK4 Edition
 * Version: 1.0.0
 * License: MIT
 * 
 * YouTube-like feed interface for RavenMPV
 * 
 * Features:
 * - Grid layout with real YouTube thumbnails
 * - Dark theme (YouTube-style)
 * - Search bar
 * - Play/Add buttons
 * - Async loading
 * - Hover effects
 * 
 * Compile:
 *   gcc -O2 -o raven-gui raven_gui.c $(pkg-config --cflags --libs gtk4) -lcurl -lpthread
 * 
 * Dependencies:
 *   Ubuntu/Debian: sudo apt install libgtk-4-dev libcurl4-openssl-dev
 *   Arch:          sudo pacman -S gtk4 curl
 *   Fedora:        sudo dnf install gtk4-devel libcurl-devel
 */

#include <gtk/gtk.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

#define APP_ID "com.ravenmpv.gui"
#define VERSION "1.0.0"
#define THUMB_DIR "/tmp/raven_thumbs"
#define FEED_FILE "/tmp/raven_gui_feed.txt"
#define MAX_ITEMS 30

/* ========== DATA STRUCTURES ========== */
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

/* ========== UTILITIES ========== */
static void ensure_thumb_dir(void) {
    struct stat st;
    if (stat(THUMB_DIR, &st) != 0) {
        mkdir(THUMB_DIR, 0755);
    }
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
        snprintf(output, size, "No views");
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
static void play_video(const char *video_id, const char *title) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "gnome-terminal -- bash -c \"./ravenmpv play 'https://youtu.be/%s'; exec bash\" "
             "2>/dev/null || "
             "xterm -e \"./ravenmpv play 'https://youtu.be/%s'\" "
             "2>/dev/null &",
             video_id, video_id);
    system(cmd);
    
    char msg[512];
    snprintf(msg, sizeof(msg), "Playing: %s", title);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
}

static void on_play_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    video_item *item = (video_item *)user_data;
    play_video(item->id, item->title);
}

static void on_add_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    video_item *item = (video_item *)user_data;
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "./ravenmpv add 'https://youtu.be/%s' >/dev/null 2>&1 &",
             item->id);
    system(cmd);
    
    char msg[512];
    snprintf(msg, sizeof(msg), "Added: %s", item->title);
    gtk_label_set_text(GTK_LABEL(status_label), msg);
}

static void on_copy_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    video_item *item = (video_item *)user_data;
    
    char url[256];
    snprintf(url, sizeof(url), "https://youtu.be/%s", item->id);
    
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(clipboard, url);
    
    gtk_label_set_text(GTK_LABEL(status_label), "URL copied!");
}

/* ========== CREATE VIDEO CARD ========== */
static GtkWidget *create_video_card(video_item *item) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(card, 320, 320);
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
        
        /* Thumbnail */
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
        "  overflow: hidden;"
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
    
    status_label = gtk_label_new("Ready. Search for videos or wait for trending feed.");
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
