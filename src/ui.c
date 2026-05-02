/* ============================================================================
 * ui.c — Nuklear Layout & Amber Theme
 * ============================================================================ */

#include "ui.h"
#include "network.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <SDL.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#endif

/* Nuklear declarations are pulled in via ui.h.  The actual implementation
 * lives in nuklear_impl.c so the header-guarded NK_IMPLEMENTATION is only
 * compiled once. */

/* ---------------------------------------------------------------------------
 * Predefined Hub Models (HuggingFace repo IDs)
 * These are resolved via the HF API to find the first .gguf file.
 * Sorted by parameter count (ascending). All four are real, public,
 * instruction-tuned GGUF repos with a working chat template.
 * --------------------------------------------------------------------------- */
static const char *hub_models[WASTELAND_MAX_HUB_MODELS] = {
    "Qwen/Qwen2.5-0.5B-Instruct-GGUF",
    "ggml-org/gemma-3-1b-it-GGUF",
    "Qwen/Qwen2.5-1.5B-Instruct-GGUF",
    "ggml-org/SmolLM2-1.7B-Instruct-GGUF"
};

/* ---------------------------------------------------------------------------
 * Colour palette
 * --------------------------------------------------------------------------- */
static struct nk_color col_amber(void)      { return nk_rgb(0xFF, 0xB0, 0x00); }
static struct nk_color col_black(void)      { return nk_rgb(0x00, 0x00, 0x00); }
static struct nk_color col_dark(void)       { return nk_rgb(0x1A, 0x1A, 0x1A); }
static struct nk_color col_dark_grey(void)  { return nk_rgb(0x33, 0x33, 0x33); }
static struct nk_color col_mid_grey(void)   { return nk_rgb(0x44, 0x44, 0x44); }

/* ---------------------------------------------------------------------------
 * scan_local_models
 *
 * readdir() / FindFirstFile() return entries in arbitrary, filesystem-defined
 * order. We sort lexicographically so the UI listing is stable across
 * launches and across different filesystems (ext4 vs btrfs vs NTFS).
 * --------------------------------------------------------------------------- */
static int model_path_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int scan_local_models(char models_list[][WASTELAND_MAX_MODEL_PATH_LEN], int max_models)
{
    int count = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA("models\\*.gguf", &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    do {
        if (count < max_models) {
            snprintf(models_list[count], WASTELAND_MAX_MODEL_PATH_LEN,
                     "models/%s", ffd.cFileName);
            count++;
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    FindClose(hFind);
#else
    DIR *d = opendir("models");
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_models) {
        size_t len = strlen(ent->d_name);
        if (len > 5 && strcmp(ent->d_name + len - 5, ".gguf") == 0) {
            snprintf(models_list[count], WASTELAND_MAX_MODEL_PATH_LEN,
                     "models/%s", ent->d_name);
            count++;
        }
    }
    closedir(d);
#endif
    if (count > 1) {
        qsort(models_list, (size_t)count,
              WASTELAND_MAX_MODEL_PATH_LEN, model_path_cmp);
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Chat persistence
 * --------------------------------------------------------------------------- */
static int chat_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int scan_local_chats(char chats_list[][WASTELAND_CHAT_NAME_LEN], int max_chats)
{
    int count = 0;
#ifdef _WIN32
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile("chats\\*.txt", &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            count < max_chats) {
            snprintf(chats_list[count], WASTELAND_CHAT_NAME_LEN,
                     "%s", fd.cFileName);
            count++;
        }
    } while (FindNextFile(hFind, &fd) != 0);
    FindClose(hFind);
#else
    DIR *d = opendir("chats");
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_chats) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcmp(ent->d_name + len - 4, ".txt") == 0) {
            snprintf(chats_list[count], WASTELAND_CHAT_NAME_LEN,
                     "%s", ent->d_name);
            count++;
        }
    }
    closedir(d);
#endif
    if (count > 1) {
        qsort(chats_list, (size_t)count,
              WASTELAND_CHAT_NAME_LEN, chat_name_cmp);
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Simple RC4 stream cipher for chat file encryption.
 * Protects chats from casual inspection in a file manager / text editor.
 * --------------------------------------------------------------------------- */
static const unsigned char chat_key[] = {
    0x7a, 0x13, 0x4f, 0x9e, 0x2b, 0x81, 0xcc, 0x55,
    0x3d, 0x67, 0x10, 0xf8, 0x99, 0x44, 0xae, 0x2e,
    0x5c, 0x77, 0x18, 0xd3, 0xb6, 0x91, 0x0a, 0xe4,
    0xcf, 0x28, 0x83, 0xfb, 0x41, 0x6d, 0x35, 0x1c
};

static void rc4_crypt_buffer(unsigned char *data, size_t len)
{
    unsigned char s[256];
    int i, j;
    for (i = 0; i < 256; i++) s[i] = (unsigned char)i;
    j = 0;
    size_t key_len = sizeof(chat_key);
    for (i = 0; i < 256; i++) {
        j = (j + s[i] + chat_key[i % key_len]) & 255;
        unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
    }
    i = j = 0;
    for (size_t k = 0; k < len; k++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
        data[k] ^= s[(s[i] + s[j]) & 255];
    }
}

void save_chat_history(const char *chat_name, const char *history)
{
    if (!chat_name || !chat_name[0]) return;
    char path[512];
    snprintf(path, sizeof(path), "chats/%s", chat_name);
    FILE *f = fopen(path, "wb");
    if (f) {
        size_t len = strlen(history);
        if (len > 0) {
            unsigned char *buf = (unsigned char *)malloc(len);
            if (buf) {
                memcpy(buf, history, len);
                rc4_crypt_buffer(buf, len);
                fwrite("WSTL", 1, 4, f);
                fwrite(buf, 1, len, f);
                free(buf);
            }
        } else {
            fwrite("WSTL", 1, 4, f);
        }
        fclose(f);
    }
}

void load_chat_history(const char *chat_name, char *history, size_t max_len)
{
    history[0] = '\0';
    if (!chat_name || !chat_name[0]) return;
    char path[512];
    snprintf(path, sizeof(path), "chats/%s", chat_name);
    FILE *f = fopen(path, "rb");
    if (f) {
        char magic[4];
        if (fread(magic, 1, 4, f) == 4 && memcmp(magic, "WSTL", 4) == 0) {
            size_t n = fread(history, 1, max_len - 1, f);
            if (n > 0) {
                rc4_crypt_buffer((unsigned char *)history, n);
                history[n] = '\0';
            } else {
                history[0] = '\0';
            }
        } else {
            rewind(f);
            size_t n = fread(history, 1, max_len - 1, f);
            history[n] = '\0';
        }
        fclose(f);
    }
}

static void generate_chat_name_from_prompt(const char *prompt, char *out_name, size_t out_len)
{
    char base[64] = {0};
    int b = 0;
    while (*prompt == ' ') prompt++;
    
    for (int i = 0; prompt[i] && b < 40; i++) {
        unsigned char c = (unsigned char)prompt[i];
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            base[b++] = (char)c;
        } else if (c == ' ' || c == '-') {
            if (b > 0 && base[b-1] != ' ') {
                base[b++] = ' ';
            }
        }
    }
    while (b > 0 && base[b-1] == ' ') b--;
    base[b] = '\0';
    
    if (b == 0) strcpy(base, "Session");
    
    snprintf(out_name, out_len, "%s.txt", base);
    
    char path[512];
    snprintf(path, sizeof(path), "chats/%s", out_name);
    struct stat st;
    int suffix = 1;
    while (stat(path, &st) == 0 && suffix < 100) {
        snprintf(out_name, out_len, "%s_%d.txt", base, suffix);
        snprintf(path, sizeof(path), "chats/%s", out_name);
        suffix++;
    }
}

/* ---------------------------------------------------------------------------
 * format_file_size
 * --------------------------------------------------------------------------- */
static void format_file_size(off_t bytes, char *out, size_t out_size)
{
    if (bytes >= 1024LL * 1024 * 1024 * 1024)
        snprintf(out, out_size, "%.2f TB", bytes / (1024.0 * 1024 * 1024 * 1024));
    else if (bytes >= 1024LL * 1024 * 1024)
        snprintf(out, out_size, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(out, out_size, "%.2f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(out, out_size, "%.2f KB", bytes / 1024.0);
    else
        snprintf(out, out_size, "%ld B", (long)bytes);
}

/* ---------------------------------------------------------------------------
 * ui_apply_amber_theme
 * --------------------------------------------------------------------------- */
void ui_apply_amber_theme(struct nk_context *nk)
{
    struct nk_color amber      = col_amber();
    struct nk_color black      = col_black();
    struct nk_color dark       = col_dark();
    struct nk_color dark_grey  = col_dark_grey();
    struct nk_color mid_grey   = col_mid_grey();

    struct nk_style *s = &nk->style;

    /* Window */
    s->window.background             = dark;
    s->window.fixed_background       = nk_style_item_color(dark);
    s->window.border_color           = amber;
    s->window.popup_border_color     = amber;
    s->window.combo_border_color     = amber;
    s->window.contextual_border_color= amber;
    s->window.menu_border_color      = amber;
    s->window.group_border_color     = amber;
    s->window.tooltip_border_color   = amber;
    s->window.scrollbar_size         = nk_vec2(10, 10);
    s->window.padding                = nk_vec2(8, 8);

    /* Text */
    s->text.color = amber;

    /* Button */
    s->button.normal        = nk_style_item_color(dark_grey);
    s->button.hover         = nk_style_item_color(mid_grey);
    s->button.active        = nk_style_item_color(black);
    s->button.border_color  = amber;
    s->button.text_normal   = amber;
    s->button.text_hover    = amber;
    s->button.text_active   = amber;

    /* Edit box / input field */
    s->edit.normal          = nk_style_item_color(black);
    s->edit.hover           = nk_style_item_color(black);
    s->edit.active          = nk_style_item_color(black);
    s->edit.border_color    = amber;
    s->edit.text_normal     = amber;
    s->edit.text_hover      = amber;
    s->edit.text_active     = amber;
    s->edit.cursor_normal   = amber;
    s->edit.cursor_hover    = amber;
    s->edit.selected_normal = amber;
    s->edit.selected_hover  = amber;
    s->edit.selected_text_normal = black;
    s->edit.selected_text_hover  = black;

    /* Checkbox */
    s->checkbox.normal         = nk_style_item_color(dark_grey);
    s->checkbox.hover          = nk_style_item_color(mid_grey);
    s->checkbox.active         = nk_style_item_color(black);
    s->checkbox.cursor_normal  = nk_style_item_color(amber);
    s->checkbox.cursor_hover   = nk_style_item_color(amber);
    s->checkbox.text_normal    = amber;
    s->checkbox.text_hover     = amber;
    s->checkbox.text_active    = amber;

    /* Progress */
    s->progress.normal         = nk_style_item_color(dark_grey);
    s->progress.hover          = nk_style_item_color(mid_grey);
    s->progress.active         = nk_style_item_color(black);
    s->progress.cursor_normal  = nk_style_item_color(amber);
    s->progress.cursor_hover   = nk_style_item_color(amber);
    s->progress.cursor_active  = nk_style_item_color(amber);
    s->progress.border_color   = amber;

    /* Scrollbars */
    s->scrollh.normal          = nk_style_item_color(dark_grey);
    s->scrollh.hover           = nk_style_item_color(mid_grey);
    s->scrollh.active          = nk_style_item_color(black);
    s->scrollh.cursor_normal   = nk_style_item_color(amber);
    s->scrollh.cursor_hover    = nk_style_item_color(amber);
    s->scrollh.cursor_active   = nk_style_item_color(amber);
    s->scrollh.border_color    = amber;
    s->scrollv = s->scrollh;

    /* Selectable (list items, etc.) */
    s->selectable.normal        = nk_style_item_color(dark);
    s->selectable.hover         = nk_style_item_color(dark_grey);
    s->selectable.pressed       = nk_style_item_color(mid_grey);
    s->selectable.text_normal   = amber;
    s->selectable.text_hover    = amber;
    s->selectable.text_pressed  = amber;
}

/* ---------------------------------------------------------------------------
 * Background download thread
 * --------------------------------------------------------------------------- */
static void* download_thread_fn(void *arg)
{
    app_state_t *state = (app_state_t *)arg;

    /* Snapshot the ID locally so the main thread can overwrite hf_model_id
       immediately after kicking us off. */
    char model_id[256];
    strncpy(model_id, state->hf_model_id, sizeof(model_id) - 1);
    model_id[sizeof(model_id) - 1] = '\0';

    state->download_cancel = 0;
    int ret = network_download_model(model_id,
                                     "models",
                                     &state->download_progress,
                                     &state->download_active,
                                     &state->download_cancel);
    state->download_success     = (ret == 0);
    state->download_complete_flag = 1;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * wrap_text_into
 *
 * `nk_edit_string` (multi-line) doesn't word-wrap — long lines just keep
 * extending and the widget grows a horizontal scrollbar. To make the chat
 * read naturally we walk the input line by line, measure prefixes against
 * the actual font's width(), and inject soft `\n` breaks where a line would
 * overflow. UTF-8-aware: never cuts mid-codepoint. Prefers breaking at the
 * last space in the fitted prefix, falls back to a hard cut if there's no
 * space (e.g., a long URL).
 *
 * The result lives in the per-frame chat_view_buf, so original chat_history
 * is never modified — copying selected text from the widget yields the
 * wrapped form (with extra newlines), which is the usual TUI trade-off.
 * --------------------------------------------------------------------------- */
static void wrap_text_into(char *out, size_t out_size,
                           const char *in,
                           const struct nk_user_font *font, float panel_w)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!in) return;

    if (!font || !font->width || panel_w <= 8.0f) {
        /* No way to measure — copy verbatim. */
        size_t inlen = strlen(in);
        if (inlen >= out_size) inlen = out_size - 1;
        memcpy(out, in, inlen);
        out[inlen] = '\0';
        return;
    }

    size_t out_pos = 0;
    const char *p = in;
    while (*p && out_pos + 2 < out_size) {
        const char *eol = strchr(p, '\n');
        size_t      line_len = eol ? (size_t)(eol - p) : strlen(p);

        /* Wrap this single logical line into one-or-more visual lines. */
        const char *seg = p;
        size_t      rem = line_len;
        while (rem > 0 && out_pos + 2 < out_size) {
            /* Binary-search the largest byte prefix that fits in panel_w. */
            int lo = 1, hi = (int)rem, fit = 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                float w = font->width(font->userdata, font->height, seg, mid);
                if (w <= panel_w) { fit = mid; lo = mid + 1; }
                else              { hi = mid - 1; }
            }
            /* Don't cut a multi-byte UTF-8 sequence (continuation = 10xxxxxx) */
            while (fit > 0 && fit < (int)rem &&
                   ((unsigned char)seg[fit] & 0xC0) == 0x80) fit--;
            if (fit < 1) fit = 1;

            /* Prefer breaking at the last space inside the fitted prefix —
             * but only if it leaves at least half of the prefix on this line
             * (otherwise we'd produce ugly tiny stubs on long words). */
            if ((size_t)fit < rem) {
                int i = fit;
                while (i > fit / 2 && seg[i - 1] != ' ') i--;
                if (i > fit / 2) fit = i;
            }

            size_t copy = (size_t)fit;
            if (out_pos + copy + 1 >= out_size) copy = out_size - out_pos - 2;
            memcpy(out + out_pos, seg, copy);
            out_pos += copy;
            seg += copy;
            rem -= copy;

            if (rem > 0 && out_pos + 1 < out_size) {
                out[out_pos++] = '\n';
                /* Skip a single space at the start of the next visual line so
                 * the wrapped text doesn't begin with leading whitespace. */
                while (rem > 0 && *seg == ' ') { seg++; rem--; }
            }
        }

        if (eol) {
            if (out_pos + 1 < out_size) out[out_pos++] = '\n';
            p = eol + 1;
        } else {
            break;
        }
    }
    out[out_pos] = '\0';
}

/* ---------------------------------------------------------------------------
 * compact_chat_history
 *
 * Remove the oldest N user/assistant pairs from the flat history string.
 * --------------------------------------------------------------------------- */
static void compact_chat_history(app_state_t *state, int pairs)
{
    for (int i = 0; i < pairs; i++) {
        char *next = strstr(state->chat_history, "\n> ");
        if (!next) {
            /* Only one message left — can't compact further. */
            break;
        }
        size_t len = strlen(next + 1);
        memmove(state->chat_history, next + 1, len + 1);
    }
    state->chat_last_len = strlen(state->chat_history);
}

/* ---------------------------------------------------------------------------
 * ui_render
 * --------------------------------------------------------------------------- */
void ui_render(struct nk_context *nk, app_state_t *state, int width, int height)
{
    ui_apply_amber_theme(nk);

    /* Track status_msg changes to reset timer */
    if (strcmp(state->status_msg, state->last_status_msg) != 0) {
        strncpy(state->last_status_msg, state->status_msg, sizeof(state->last_status_msg) - 1);
        state->last_status_msg[sizeof(state->last_status_msg) - 1] = '\0';
        state->status_timer = SDL_GetTicks();
    }

    /* Auto-clear status message after 3000ms */
    if (state->status_msg[0] != '\0' && (SDL_GetTicks() - state->status_timer) > 3000) {
        state->status_msg[0] = '\0';
        state->last_status_msg[0] = '\0';
    }

    /* Auto-compact context when a generation finishes and usage > 80 % */
    static int was_generating = 0;
    if (was_generating && !state->is_generating) {
        if (inference_get_context_stats(state->inference,
                                        state->chat_history,
                                        &state->context_tokens,
                                        &state->context_max) == 0) {
            if (state->context_max > 0 &&
                state->context_tokens > (int)(state->context_max * 0.80f)) {
                compact_chat_history(state, 1);
                inference_get_context_stats(state->inference,
                                            state->chat_history,
                                            &state->context_tokens,
                                            &state->context_max);
                snprintf(state->status_msg, sizeof(state->status_msg),
                         "Context auto-compacted.");
            }
        }
    }
    was_generating = state->is_generating;

    struct nk_color amber = col_amber();

    /* Poll async model load completion. Runs every frame so the UI can
     * react the moment the load thread finishes. */
    if (state->loading_model_index >= 0 &&
        !inference_is_loading(state->inference)) {
        int idx = state->loading_model_index;
        const char *p = state->models[idx];
        const char *slash = strrchr(p, '/');
        const char *basename = slash ? slash + 1 : p;
        struct stat fst;
        char sz_str[32] = "?";
        if (stat(p, &fst) == 0)
            format_file_size(fst.st_size, sz_str, sizeof(sz_str));

        int result = inference_take_load_result(state->inference);
        if (result == 1) {
            state->selected_model = idx;
            if (!state->network_lockdown) {
                if (lockdown_network() == 0) {
                    state->network_lockdown = 1;
                    snprintf(state->status_msg, sizeof(state->status_msg),
                             "Loaded %s (%s). Lockdown active.",
                             basename, sz_str);
                } else {
                    snprintf(state->status_msg, sizeof(state->status_msg),
                             "Loaded %s. Lockdown FAILED.", basename);
                }
            } else {
                snprintf(state->status_msg, sizeof(state->status_msg),
                         "Switched to %s (%s).", basename, sz_str);
            }
        } else {
            snprintf(state->status_msg, sizeof(state->status_msg),
                     "Failed to load %s.", basename);
            state->selected_model = -1;
        }
        state->loading_model_index = -1;
        if (inference_get_context_stats(state->inference,
                                        state->chat_history,
                                        &state->context_tokens,
                                        &state->context_max) != 0) {
            state->context_tokens = 0;
            state->context_max = 0;
        }
    }

    if (nk_begin(nk, "Wasteland",
                 nk_rect(0, 0, width, height),
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND))
    {
        /* ========================= HEADER ========================= */
        nk_layout_row_dynamic(nk, 30, 1);
        nk_label_colored(nk, "WASTELAND TERMINAL v0.1",
                         NK_TEXT_CENTERED, amber);

        /* Status row: [toggle] | SYS | status | NET
         * \xc2\xab = U+00AB << (collapse)  \xc2\xbb = U+00BB >> (expand) */
        nk_layout_row_begin(nk, NK_DYNAMIC, 20, 4);
        nk_layout_row_push(nk, 0.08f);
        if (nk_button_label(nk, state->left_panel_collapsed
                                ? "\xc2\xbb" : "\xc2\xab")) {
            state->left_panel_collapsed = !state->left_panel_collapsed;
        }
        nk_layout_row_push(nk, 0.25f);
        nk_label_colored(nk, "SYS: ONLINE", NK_TEXT_LEFT, amber);

        nk_layout_row_push(nk, 0.40f);
        char status[128];
        if (state->is_generating) {
            snprintf(status, sizeof(status), "INF: GENERATING...");
        } else if (state->network_lockdown) {
            snprintf(status, sizeof(status), "SEC: LOCKDOWN ACTIVE");
        } else {
            snprintf(status, sizeof(status), "SEC: UNLOCKED");
        }
        nk_label_colored(nk, status, NK_TEXT_CENTERED, amber);

        nk_layout_row_push(nk, 0.27f);
        if (state->network_lockdown) {
            nk_label_colored(nk, "NET: DISCONNECTED",
                             NK_TEXT_RIGHT, amber);
        } else {
            nk_label_colored(nk, "NET: AVAILABLE",
                             NK_TEXT_RIGHT, amber);
        }
        nk_layout_row_end(nk);

        /* =================== MAIN SPLIT AREA =================== */
        if (state->left_panel_collapsed) {
            nk_layout_row_dynamic(nk, height - 70, 1);
        } else {
            nk_layout_row_dynamic(nk, height - 70, 2);
        }

        /* --------------------- LEFT PANEL --------------------- */
        if (!state->left_panel_collapsed &&
            nk_group_begin(nk, "LeftPanel", NK_WINDOW_BORDER)) {

            /* ===== CHATS ===== */
            nk_layout_row_dynamic(nk, 20, 1);
            nk_label_colored(nk, "CHATS", NK_TEXT_LEFT, amber);
            nk_layout_row_dynamic(nk, 2, 1);
            nk_button_color(nk, amber);

            nk_layout_row_dynamic(nk, 20, 1);
            if (nk_button_label(nk, "[ NEW CHAT ]")) {
                char new_chat[WASTELAND_CHAT_NAME_LEN];
                char base[] = "New Chat";
                snprintf(new_chat, sizeof(new_chat), "%s.txt", base);
                char path[512];
                snprintf(path, sizeof(path), "chats/%s", new_chat);
                struct stat st;
                int suffix = 1;
                while (stat(path, &st) == 0 && suffix < 100) {
                    snprintf(new_chat, sizeof(new_chat), "%s_%d.txt", base, suffix);
                    snprintf(path, sizeof(path), "chats/%s", new_chat);
                    suffix++;
                }
                
                if (state->selected_chat >= 0 && state->selected_chat < state->chat_count) {
                    save_chat_history(state->chats[state->selected_chat], state->chat_history);
                }
                
                state->chat_history[0] = '\0';
                state->chat_last_len = 0;
                state->context_tokens = 0;
                state->context_max = 0;
                
                if (state->chat_count < WASTELAND_MAX_CHATS) {
                    snprintf(state->chats[state->chat_count],
                             WASTELAND_CHAT_NAME_LEN, "%s", new_chat);
                    state->selected_chat = state->chat_count;
                    state->chat_count++;
                    save_chat_history(new_chat, "");
                }
            }

            if (state->chat_count == 0) {
                nk_layout_row_dynamic(nk, 20, 1);
                nk_label_colored(nk, "No chats found.", NK_TEXT_CENTERED, col_dark_grey());
            } else {
                for (int i = 0; i < state->chat_count; i++) {
                    int is_loaded = (state->selected_chat == i);
                    char chat_display[256];
                    strncpy(chat_display, state->chats[i], sizeof(chat_display) - 1);
                    chat_display[sizeof(chat_display) - 1] = '\0';
                    size_t cdl = strlen(chat_display);
                    if (cdl > 4 && strcmp(chat_display + cdl - 4, ".txt") == 0) {
                        chat_display[cdl - 4] = '\0';
                    }
                    nk_layout_row_dynamic(nk, 20, 1);
                    if (is_loaded) {
                        nk_label_colored(nk, chat_display, NK_TEXT_CENTERED, amber);
                    } else {
                        nk_label_colored(nk, chat_display, NK_TEXT_CENTERED, col_dark_grey());
                    }

                    nk_layout_row_begin(nk, NK_DYNAMIC, 20, 2);
                    char btn_label[128];
                    if (is_loaded) {
                        snprintf(btn_label, sizeof(btn_label), "[ ACTIVE ]");
                    } else {
                        snprintf(btn_label, sizeof(btn_label), "[ LOAD ]");
                    }
                    nk_layout_row_push(nk, 0.85f);
                    if (nk_button_label(nk, btn_label)) {
                        if (!is_loaded) {
                            if (state->selected_chat >= 0) {
                                save_chat_history(state->chats[state->selected_chat], state->chat_history);
                            }
                            load_chat_history(state->chats[i], state->chat_history, WASTELAND_MAX_CHAT_HISTORY);
                            if (inference_get_context_stats(state->inference,
                                                            state->chat_history,
                                                            &state->context_tokens,
                                                            &state->context_max) != 0) {
                                state->context_tokens = 0;
                                state->context_max = 0;
                            }
                            state->selected_chat = i;
                            state->chat_last_len = 0;
                            state->chat_scroll_y = (nk_uint)0x7FFFFFFF;
                        }
                    }
                    nk_layout_row_push(nk, 0.15f);
                    if (nk_button_label(nk, "\xc3\x97")) {
                        char path[512];
                        snprintf(path, sizeof(path), "chats/%s", state->chats[i]);
                        remove(path);
                        if (state->selected_chat == i) {
                            state->chat_history[0] = '\0';
                            state->selected_chat = -1;
                        } else if (state->selected_chat > i) {
                            state->selected_chat--;
                        }
                        for (int j = i; j < state->chat_count - 1; j++) {
                            strcpy(state->chats[j], state->chats[j+1]);
                        }
                        state->chat_count--;
                        i--;
                    }
                    nk_layout_row_end(nk);
                }
            }

            /* Spacer */
            nk_layout_row_dynamic(nk, 10, 1);
            nk_spacing(nk, 1);

            /* ===== HUB MODELS ===== */
            nk_layout_row_dynamic(nk, 20, 1);
            nk_label_colored(nk, "HUB MODELS", NK_TEXT_LEFT, amber);
            nk_layout_row_dynamic(nk, 2, 1);
            nk_button_color(nk, amber); /* horizontal rule */

            if (!state->network_lockdown) {
                /* --- Predefined hub models (radio buttons) --- */
                for (int i = 0; i < WASTELAND_MAX_HUB_MODELS; i++) {
                    nk_layout_row_dynamic(nk, 22, 1);
                    if (nk_option_label(nk, hub_models[i],
                                        state->selected_hub_model == i)) {
                        state->selected_hub_model = i;
                        state->custom_hf_id[0] = '\0';
                        strncpy(state->hf_model_id, hub_models[i],
                                sizeof(state->hf_model_id) - 1);
                        state->hf_model_id[sizeof(state->hf_model_id)-1] = '\0';
                    }
                }

                /* --- Custom ID input --- */
                nk_layout_row_dynamic(nk, 22, 1);
                nk_label_colored(nk, "Custom ID or URL:", NK_TEXT_LEFT, amber);
                nk_layout_row_dynamic(nk, 28, 1);
                int custom_len = (int)strlen(state->custom_hf_id);
                nk_edit_string_zero_terminated(nk, NK_EDIT_FIELD,
                    state->custom_hf_id,
                    sizeof(state->custom_hf_id),
                    nk_filter_default);
                /* If user types into custom field, drop hub selection */
                if (custom_len > 0 && state->selected_hub_model >= 0) {
                    state->selected_hub_model = -1;
                    state->hf_model_id[0] = '\0';
                }

                /* --- Resolve download target --- */
                const char *target_id = NULL;
                if (state->selected_hub_model >= 0) {
                    target_id = hub_models[state->selected_hub_model];
                } else if (state->custom_hf_id[0]) {
                    target_id = state->custom_hf_id;
                }

                /* --- Show current target --- */
                nk_layout_row_dynamic(nk, 20, 1);
                char target_label[320];
                if (target_id) {
                    snprintf(target_label, sizeof(target_label),
                             "TARGET: %s", target_id);
                } else {
                    snprintf(target_label, sizeof(target_label),
                             "TARGET: (none selected)");
                }
                nk_label_colored(nk, target_label, NK_TEXT_LEFT, amber);

                /* --- Download button --- */
                nk_layout_row_dynamic(nk, 30, 1);
                if (nk_button_label(nk, "[ DOWNLOAD ]")) {
                    if (!target_id) {
                        snprintf(state->status_msg, sizeof(state->status_msg),
                                 "ERROR: Select a model or enter a Custom ID.");
                    } else if (state->download_active) {
                        snprintf(state->status_msg, sizeof(state->status_msg),
                                 "ERROR: Download already in progress.");
                    } else {
                        strncpy(state->hf_model_id, target_id,
                                sizeof(state->hf_model_id) - 1);
                        state->hf_model_id[sizeof(state->hf_model_id)-1] = '\0';
                        /* Claim the download slot here, before pthread_create.
                         * If we deferred this to network_download_model() a
                         * fast double-click would slip through and spawn two
                         * concurrent downloaders writing the same file. */
                        state->download_active   = 1;
                        state->download_progress = 0;
                        state->download_cancel   = 0;
                        pthread_t tid;
                        if (pthread_create(&tid, NULL,
                                           download_thread_fn, state) != 0) {
                            state->download_active = 0;
                            snprintf(state->status_msg, sizeof(state->status_msg),
                                     "ERROR: Failed to spawn download thread.");
                        } else {
                            pthread_detach(tid);
                        }
                    }
                }

                if (state->download_active) {
                    /* Show filename + percent */
                    const char *fn = strrchr(state->hf_model_id, '/');
                    fn = fn ? fn + 1 : state->hf_model_id;
                    char dl_label[320];
                    snprintf(dl_label, sizeof(dl_label),
                             "DOWNLOADING: %s (%d%%)", fn, state->download_progress);
                    nk_layout_row_dynamic(nk, 20, 1);
                    nk_label_colored(nk, dl_label, NK_TEXT_LEFT, amber);

                    nk_layout_row_dynamic(nk, 20, 1);
                    nk_size prog = (nk_size)state->download_progress;
                    nk_progress(nk, &prog, 100, NK_FIXED);
                    state->download_progress = (int)prog;

                    nk_layout_row_dynamic(nk, 28, 1);
                    if (nk_button_label(nk, "[ CANCEL ]")) {
                        state->download_cancel = 1;
                        snprintf(state->status_msg, sizeof(state->status_msg),
                                 "Cancelling download...");
                    }
                } else {
                    nk_layout_row_dynamic(nk, 20, 1);
                    nk_label_colored(nk, "STATUS: READY TO DOWNLOAD",
                                     NK_TEXT_LEFT, amber);
                }
            } else {
                nk_layout_row_dynamic(nk, 20, 1);
                nk_label_colored(nk, "DOWNLOAD DISABLED",
                                 NK_TEXT_CENTERED, amber);
                nk_layout_row_dynamic(nk, 20, 1);
                nk_label_colored(nk, "LOCKDOWN ACTIVE",
                                 NK_TEXT_CENTERED, amber);
            }

            /* Spacer */
            nk_layout_row_dynamic(nk, 10, 1);
            nk_spacing(nk, 1);

            /* ===== LOCAL VAULT ===== */
            nk_layout_row_dynamic(nk, 20, 1);
            nk_label_colored(nk, "-- LOCAL VAULT --", NK_TEXT_LEFT, amber);
            nk_layout_row_dynamic(nk, 2, 1);
            nk_button_color(nk, amber); /* horizontal rule */

            nk_layout_row_dynamic(nk, 28, 1);
            if (nk_button_label(nk, "[ REFRESH ]")) {
                state->model_count = scan_local_models(
                    state->models, WASTELAND_MAX_MODELS);
                snprintf(state->status_msg, sizeof(state->status_msg),
                         "Vault refreshed. %d model(s) found.",
                         state->model_count);
            }

            if (state->model_count == 0) {
                nk_layout_row_dynamic(nk, 20, 1);
                nk_label_colored(nk, "No local models.",
                                 NK_TEXT_CENTERED, amber);
            } else {
                for (int i = 0; i < state->model_count; i++) {
                    const char *slash = strrchr(state->models[i], '/');
                    const char *basename = slash ? slash + 1 : state->models[i];

                    /* Row with LOAD button + DELETE button */
                    nk_layout_row_begin(nk, NK_DYNAMIC, 26, 2);

                    struct stat fst;
                    char sz_str[32] = "?";
                    if (stat(state->models[i], &fst) == 0) {
                        format_file_size(fst.st_size, sz_str, sizeof(sz_str));
                    }

                    char btn_label[WASTELAND_MAX_MODEL_PATH_LEN + 32];
                    int is_loaded  = (state->selected_model == i);
                    int is_loading = (state->loading_model_index == i);
                    if (is_loading) {
                        snprintf(btn_label, sizeof(btn_label),
                                 "[ LOADING: %s | %s ... ]", basename, sz_str);
                    } else if (is_loaded) {
                        snprintf(btn_label, sizeof(btn_label),
                                 "[ UNLOAD: %s | %s ]", basename, sz_str);
                    } else {
                        snprintf(btn_label, sizeof(btn_label),
                                 "[ LOAD: %s | %s ]", basename, sz_str);
                    }

                    int load_busy = (state->loading_model_index >= 0);
                    int gen_busy  = inference_is_generating(state->inference);
                    nk_layout_row_push(nk, 0.92f);
                    if (nk_button_label(nk, btn_label) && !load_busy && !gen_busy) {
                        if (is_loaded) {
                            inference_unload_model(state->inference);
                            state->selected_model = -1;
                            snprintf(state->status_msg,
                                     sizeof(state->status_msg),
                                     "Unloaded %s.", basename);
                        } else {
                            snprintf(state->status_msg,
                                     sizeof(state->status_msg),
                                     "Loading %s (%s)...", basename, sz_str);
                            if (inference_load_model_async(state->inference,
                                                           state->models[i]) == 0) {
                                state->loading_model_index = i;
                            } else {
                                snprintf(state->status_msg,
                                         sizeof(state->status_msg),
                                         "Failed to start load for %s.",
                                         basename);
                            }
                        }
                    }

                    nk_layout_row_push(nk, 0.08f);
                    if (nk_button_label(nk, "\xc3\x97") && !load_busy && !gen_busy) {
                        if (remove(state->models[i]) == 0) {
                            /* If deleted model was loaded, unload it */
                            if (state->selected_model == i) {
                                inference_unload_model(state->inference);
                                state->selected_model = -1;
                            } else if (state->selected_model > i) {
                                /* Shift index down since we're removing an element before it */
                                state->selected_model--;
                            }
                            /* Shift remaining entries */
                            for (int j = i; j < state->model_count - 1; j++) {
                                strcpy(state->models[j], state->models[j + 1]);
                            }
                            state->model_count--;
                            /* Decrement i so we process the new element at this position */
                            i--;
                            snprintf(state->status_msg, sizeof(state->status_msg),
                                     "Deleted '%s'.", basename);
                        } else {
                            snprintf(state->status_msg, sizeof(state->status_msg),
                                     "Failed to delete '%s'.", basename);
                        }
                    }
                }
            }

            /* Spacer */
            nk_layout_row_dynamic(nk, 10, 1);
            nk_spacing(nk, 1);

            /* ===== AGENT MODE ===== */
            nk_layout_row_dynamic(nk, 20, 1);
            nk_label_colored(nk, "AGENT MODE", NK_TEXT_LEFT, amber);
            nk_layout_row_dynamic(nk, 2, 1);
            nk_button_color(nk, amber);

            nk_layout_row_dynamic(nk, 24, 1);
            nk_checkbox_label(nk, state->agent_mode
                                  ? "AGENT MODE: ON  (model can read/edit files)"
                                  : "AGENT MODE: OFF (chat only)",
                              &state->agent_mode);

            if (state->agent_mode) {
                nk_layout_row_dynamic(nk, 18, 1);
                nk_label_colored(nk, "Workspace (sandbox root):",
                                 NK_TEXT_LEFT, amber);
                nk_layout_row_dynamic(nk, 26, 1);
                int wlen = (int)strlen(state->agent_workspace);
                nk_edit_string(nk, NK_EDIT_FIELD,
                               state->agent_workspace, &wlen,
                               sizeof(state->agent_workspace),
                               nk_filter_default);
                state->agent_workspace[wlen] = '\0';

                /* Quick visual feedback: does the path exist as a directory? */
                /* Expand ~/foo and $HOME/foo before stat'ing so the user
                 * sees OK immediately after typing a portable path. The
                 * agent module does the same expansion at execute time. */
                char ws_check[1280];
                ws_check[0] = '\0';
                if (state->agent_workspace[0] != '\0') {
                    const char *home = getenv("HOME");
                    if (home && state->agent_workspace[0] == '~' &&
                        (state->agent_workspace[1] == '/' ||
                         state->agent_workspace[1] == '\0'))
                    {
                        snprintf(ws_check, sizeof(ws_check), "%s%s",
                                 home, state->agent_workspace + 1);
                    } else if (home &&
                               strncmp(state->agent_workspace, "$HOME", 5) == 0 &&
                               (state->agent_workspace[5] == '/' ||
                                state->agent_workspace[5] == '\0'))
                    {
                        snprintf(ws_check, sizeof(ws_check), "%s%s",
                                 home, state->agent_workspace + 5);
                    } else {
                        snprintf(ws_check, sizeof(ws_check), "%s",
                                 state->agent_workspace);
                    }
                }
                struct stat wst;
                int valid = (ws_check[0] != '\0' &&
                             stat(ws_check, &wst) == 0 &&
                             S_ISDIR(wst.st_mode));
                nk_layout_row_dynamic(nk, 18, 1);
                if (state->agent_workspace[0] == '\0') {
                    nk_label_colored(nk, "(no workspace set)",
                                     NK_TEXT_LEFT, col_dark_grey());
                } else if (!valid) {
                    nk_label_colored(nk, "INVALID: not a directory",
                                     NK_TEXT_LEFT, nk_rgb(0xCC, 0x44, 0x00));
                } else {
                    nk_label_colored(nk, "OK", NK_TEXT_LEFT, amber);
                }
            }

            /* Spacer */
            nk_layout_row_dynamic(nk, 10, 1);
            nk_spacing(nk, 1);

            /* ===== SYSTEM PROMPT ===== */
            nk_layout_row_dynamic(nk, 20, 1);
            nk_label_colored(nk, "SYSTEM PROMPT", NK_TEXT_LEFT, amber);
            nk_layout_row_dynamic(nk, 2, 1);
            nk_button_color(nk, amber);

            nk_layout_row_dynamic(nk, 100, 1);
            int sys_len = (int)strlen(state->system_prompt);
            nk_edit_string(nk, NK_EDIT_BOX | NK_EDIT_MULTILINE,
                           state->system_prompt, &sys_len, 1024,
                           nk_filter_default);
            state->system_prompt[sys_len] = '\0';

            /* Spacer */
            nk_layout_row_dynamic(nk, 10, 1);
            nk_spacing(nk, 1);

            /* --- Network status footer --- */
            nk_layout_row_dynamic(nk, 2, 1);
            nk_button_color(nk, amber);
            nk_layout_row_dynamic(nk, 20, 1);
            if (state->network_lockdown) {
                nk_label_colored(nk, "NET: LOCKDOWN ACTIVE",
                                 NK_TEXT_CENTERED, amber);
            } else {
                nk_label_colored(nk, "NET: DISCONNECTED (READY)",
                                 NK_TEXT_CENTERED, amber);
            }

            nk_group_end(nk);
        }

        /* --------------------- RIGHT PANEL --------------------- */
        if (nk_group_begin(nk, "RightPanel", NK_WINDOW_BORDER)) {
            /* ---------- Agent pending-approval banner ----------
             * If the worker is waiting for user APPLY/REJECT on a mutating
             * tool call, render a compact diff/preview panel at the top of
             * the right panel with two action buttons. */
            const char *p_path = NULL, *p_content = NULL,
                       *p_search = NULL, *p_replace = NULL;
            int pending = (state->agent_mode && state->inference)
                ? inference_get_pending(state->inference,
                                        &p_path, &p_content,
                                        &p_search, &p_replace)
                : 0;
            int pending_panel_h = pending ? 200 : 0;

            if (pending) {
                struct nk_color warn  = nk_rgb(0xFF, 0xB0, 0x00);
                struct nk_color rej_c = nk_rgb(0xCC, 0x44, 0x00);
                struct nk_color add_c = nk_rgb(0xAA, 0xCC, 0x00);

                nk_layout_row_dynamic(nk, 18, 1);
                nk_label_colored(nk, "▼ AGENT PROPOSAL — REVIEW",
                                 NK_TEXT_LEFT, warn);

                char title[640];
                snprintf(title, sizeof(title), "%s → %s",
                         pending == 1 ? "WRITE_FILE" : "APPLY_EDIT",
                         p_path ? p_path : "");
                nk_layout_row_dynamic(nk, 16, 1);
                nk_label_colored(nk, title, NK_TEXT_LEFT, warn);

                /* Preview area: ~110px scrollable group */
                nk_layout_row_dynamic(nk, 110, 1);
                if (nk_group_begin(nk, "PendingPreview", NK_WINDOW_BORDER)) {
                    if (pending == 1) {
                        /* write_file: show full new content (truncated). */
                        const char *txt = p_content ? p_content : "(empty)";
                        char buf[2048];
                        size_t tl = strlen(txt);
                        if (tl >= sizeof(buf)) {
                            memcpy(buf, txt, sizeof(buf) - 5);
                            memcpy(buf + sizeof(buf) - 5, "...\n", 5);
                            txt = buf;
                            tl  = sizeof(buf) - 1;
                        }
                        nk_layout_row_dynamic(nk, 14, 1);
                        nk_label_colored_wrap(nk, txt, add_c);
                    } else {
                        /* apply_edit: show SEARCH then REPLACE blocks. */
                        nk_layout_row_dynamic(nk, 14, 1);
                        nk_label_colored(nk, "FIND:", NK_TEXT_LEFT, warn);
                        nk_label_colored_wrap(nk,
                            p_search ? p_search : "(empty)", rej_c);
                        nk_label_colored(nk, "REPLACE WITH:", NK_TEXT_LEFT, warn);
                        nk_label_colored_wrap(nk,
                            p_replace ? p_replace : "(empty)", add_c);
                    }
                    nk_group_end(nk);
                }

                nk_layout_row_dynamic(nk, 28, 2);
                if (nk_button_label(nk, "[ APPLY ]")) {
                    inference_set_pending_approval(state->inference, +1);
                }
                if (nk_button_label(nk, "[ REJECT ]")) {
                    inference_set_pending_approval(state->inference, -1);
                }
            }

            /* Chat history — single read-only multi-line edit box. We need
             * an edit (not labels) so the user can mouse-select and Ctrl+C
             * the text. NK_EDIT_READ_ONLY would kill input → no selection,
             * so we get the same effect by writing into a per-frame copy
             * of chat_history: any stray typing is overwritten on the next
             * render. The amber theme already paints text/cursor, so the
             * box visually matches the rest of the terminal. */
            size_t chat_len = strlen(state->chat_history);
            /* Auto-scroll to bottom whenever new tokens arrive. The outer
             * scrolled group (below) holds many sub-edit-boxes — none of
             * them know about the cursor — so we steer the OUTER scrollbar
             * by setting chat_scroll_y to a max sentinel; Nuklear clamps it
             * down to the real max on next layout, which is exactly the
             * "pinned to bottom" behaviour the user expects while the model
             * streams its reply. We re-arm on every growth so even a slow
             * trickle of bytes keeps the view stuck at the bottom. */
            if (chat_len > state->chat_last_len) {
                state->chat_scroll_y = (nk_uint)0x7FFFFFFF;
            }
            state->chat_last_len = chat_len;

            /* Chat area: right-panel minus header/footer controls.
             * Spacer(6)+label(18)+input(34)+status(18) ≈ 80px below the chat,
             * plus Nuklear group padding (~20px) → reserve 200px total.
             * Subtract pending-approval banner if present. */
            int chat_h = height - 200 - pending_panel_h;
            if (chat_h < 60) chat_h = 60;

            {
                /* Two-tier rendering for "thinking" highlighting:
                 *   - Outer scrolled group → one unified scrollbar.
                 *   - Inside, walk chat_history splitting on `-- THINK --` /
                 *     `-- END THINK --` markers into sections.
                 *   - Each section gets its own nk_edit_string with the
                 *     style.edit.text_normal colour temporarily overridden
                 *     (full amber for assistant/user, amber_dim for thoughts).
                 * Each section sized to fit its wrapped content height so
                 * there's no per-section scrollbar — only the outer one
                 * scrolls. Trade-off: mouse-drag selection works inside
                 * one block but not across blocks; copying a single answer
                 * is still trivial. */

                const struct nk_user_font *font = nk->style.font;
                float font_h = font ? font->height : 14.0f;

                struct nk_color amber_normal = nk_rgb(0xFF, 0xB0, 0x00);
                struct nk_color amber_dim    = nk_rgb(0xA0, 0x68, 0x00);

                /* Probe the width an upcoming row will get so we can
                 * soft-wrap. Reserve ~24px for edit padding + scrollbar. */
                nk_layout_row_dynamic(nk, 1, 1);
                struct nk_rect probe = nk_widget_bounds(nk);
                nk_spacing(nk, 1);
                float panel_w = probe.w - 36.0f;
                if (panel_w < 64.0f) panel_w = 64.0f;

                nk_layout_row_dynamic(nk, (float)chat_h, 1);
                nk_uint sx = state->chat_scroll_x;
                nk_uint sy = state->chat_scroll_y;
                if (nk_group_scrolled_offset_begin(nk, &sx, &sy,
                                                   "ChatScroll",
                                                   NK_WINDOW_BORDER))
                {
                    static char section_buf[WASTELAND_MAX_CHAT_HISTORY];
                    static char wrapped   [WASTELAND_MAX_CHAT_HISTORY * 2];

                    int section_pos = 0;
                    int in_think    = 0;

                    const char *p   = state->chat_history;
                    const char *end = p + chat_len;
                    int finished    = 0;

                    while (!finished) {
                        const char *eol = (p < end)
                            ? memchr(p, '\n', (size_t)(end - p)) : NULL;
                        size_t llen = (p < end)
                            ? (eol ? (size_t)(eol - p) : (size_t)(end - p))
                            : 0;

                        int is_th_start = (p < end && llen >= 11 &&
                            strncmp(p, "-- THINK --",     11) == 0);
                        int is_th_end   = (p < end && llen >= 15 &&
                            strncmp(p, "-- END THINK --", 15) == 0);
                        int at_eof      = (p >= end);

                        /* Flush the buffered section whenever we hit a
                         * boundary marker or EOF. */
                        if ((is_th_start || is_th_end || at_eof) &&
                            section_pos > 0)
                        {
                            section_buf[section_pos] = '\0';

                            wrap_text_into(wrapped, sizeof(wrapped),
                                           section_buf, font, panel_w);

                            /* Count visual lines for height. */
                            int vlines = 1;
                            for (const char *q = wrapped; *q; q++)
                                if (*q == '\n') vlines++;
                            float sec_h = (float)vlines * (font_h + 2.0f)
                                          + 10.0f; /* edit-box padding */

                            /* Section header so the user sees where the
                             * model's reasoning starts and ends. Only think
                             * blocks get a label — normal output is
                             * obvious. */
                            if (in_think) {
                                nk_layout_row_dynamic(nk, font_h + 2.0f, 1);
                                nk_label_colored(nk, "▒ thinking",
                                                 NK_TEXT_LEFT, amber_dim);
                            }

                            /* Override edit text colour for this section. */
                            struct nk_color saved =
                                nk->style.edit.text_normal;
                            nk->style.edit.text_normal =
                                in_think ? amber_dim : amber_normal;

                            int rlen = (int)strlen(wrapped);
                            nk_layout_row_dynamic(nk, sec_h, 1);
                            nk_edit_string(nk,
                                NK_EDIT_BOX | NK_EDIT_MULTILINE,
                                wrapped, &rlen,
                                (int)sizeof(wrapped),
                                nk_filter_default);

                            nk->style.edit.text_normal = saved;
                            section_pos = 0;
                        }

                        if (at_eof) { finished = 1; break; }

                        if (is_th_start) {
                            in_think = 1;
                        } else if (is_th_end) {
                            in_think = 0;
                        } else {
                            /* Append this line + newline to current
                             * section buffer. */
                            if ((size_t)section_pos + llen + 2
                                < sizeof(section_buf))
                            {
                                memcpy(section_buf + section_pos, p, llen);
                                section_pos += (int)llen;
                                section_buf[section_pos++] = '\n';
                            }
                        }

                        p = eol ? eol + 1 : end;
                    }

                    nk_group_scrolled_end(nk);
                }
                state->chat_scroll_x = sx;
                state->chat_scroll_y = sy;
            }

            /* Context bar */
            int ctx_pct = (state->context_max > 0)
                ? (state->context_tokens * 100 / state->context_max)
                : 0;
            char ctx_label[64];
            snprintf(ctx_label, sizeof(ctx_label), "CTX: %d / %d (%d%%)",
                     state->context_tokens, state->context_max, ctx_pct);

            nk_layout_row_begin(nk, NK_DYNAMIC, 18, 3);
            nk_layout_row_push(nk, 0.55f);
            struct nk_color ctx_color = amber;
            if (ctx_pct > 90) ctx_color = nk_rgb(0xCC, 0x44, 0x00);
            else if (ctx_pct > 75) ctx_color = nk_rgb(0xFF, 0x80, 0x00);
            nk_label_colored(nk, ctx_label, NK_TEXT_LEFT, ctx_color);

            nk_layout_row_push(nk, 0.27f);
            nk_size prog = (nk_size)ctx_pct;
            nk_progress(nk, &prog, 100, NK_FIXED);

            nk_layout_row_push(nk, 0.18f);
            if (nk_button_label(nk, "[ COMPACT ]")) {
                compact_chat_history(state, 1);
                if (inference_get_context_stats(state->inference,
                                                state->chat_history,
                                                &state->context_tokens,
                                                &state->context_max) != 0) {
                    state->context_tokens = 0;
                    state->context_max = 0;
                }
            }
            nk_layout_row_end(nk);

            /* Spacer */
            nk_layout_row_dynamic(nk, 6, 1);
            nk_spacing(nk, 1);

            /* Prompt label */
            nk_layout_row_dynamic(nk, 18, 1);
            nk_label_colored(nk, ">", NK_TEXT_LEFT, amber);

            /* Input field + icon button in one row.
             *   \xe2\x96\xb6 = U+25B6 BLACK RIGHT-POINTING TRIANGLE (▶) — send
             *   \xe2\x96\xa0 = U+25A0 BLACK SQUARE (■)                 — stop */
            nk_layout_row_begin(nk, NK_DYNAMIC, 34, 2);

            nk_layout_row_push(nk, 0.87f);
            int input_len = (int)strlen(state->input_buffer);
            nk_flags active = nk_edit_string(nk,
                NK_EDIT_FIELD | NK_EDIT_SIG_ENTER,
                state->input_buffer,
                &input_len,
                WASTELAND_MAX_PROMPT_LEN,
                nk_filter_default);
            state->input_buffer[input_len] = '\0';

            nk_layout_row_push(nk, 0.13f);
            if (state->is_generating) {
                /* ■ — click to cancel */
                if (nk_button_label(nk, "\xe2\x96\xa0")) {
                    inference_cancel_generation(state->inference);
                }
            } else {
                /* ▶ — click (or Enter) to send */
                int send = (active & NK_EDIT_COMMITED) ||
                           nk_button_label(nk, "\xe2\x96\xb6");
                if (send && state->input_buffer[0] &&
                    inference_is_model_loaded(state->inference))
                {
                    /* Pre-send compact if context is near full */
                    if (state->context_max > 0 &&
                        state->context_tokens > (int)(state->context_max * 0.75f)) {
                        compact_chat_history(state, 1);
                        inference_get_context_stats(state->inference,
                                                    state->chat_history,
                                                    &state->context_tokens,
                                                    &state->context_max);
                    }

                    /* Auto-create chat if none active */
                    if (state->selected_chat == -1 &&
                        state->chat_count < WASTELAND_MAX_CHATS) {
                        char new_chat[WASTELAND_CHAT_NAME_LEN];
                        generate_chat_name_from_prompt(state->input_buffer,
                                                       new_chat,
                                                       sizeof(new_chat));
                        snprintf(state->chats[state->chat_count],
                                 WASTELAND_CHAT_NAME_LEN, "%s", new_chat);
                        state->selected_chat = state->chat_count;
                        state->chat_count++;
                        save_chat_history(new_chat, "");
                    }

                    pthread_mutex_lock(&state->chat_mutex);
                    size_t hlen = strlen(state->chat_history);
                    size_t room = WASTELAND_MAX_CHAT_HISTORY - hlen - 1;
                    if (room > 0) {
                        /* Append user prompt */
                        char prompt[WASTELAND_MAX_PROMPT_LEN + 8];
                        snprintf(prompt, sizeof(prompt), "> %s\n", state->input_buffer);
                        size_t plen = strlen(prompt);
                        if (plen > room) plen = room;
                        memcpy(state->chat_history + hlen, prompt, plen);
                        state->chat_history[hlen + plen] = '\0';
                    }
                    pthread_mutex_unlock(&state->chat_mutex);

                    inference_set_chat_history(state->inference,
                                               state->chat_history);
                    inference_submit_prompt(state->inference,
                                            state->system_prompt,
                                            state->input_buffer);
                    state->input_buffer[0] = '\0';
                    state->chat_scroll_y = (nk_uint)0x7FFFFFFF;
                }
            }
            nk_layout_row_end(nk);

            /* Status message row */
            if (state->status_msg[0]) {
                nk_layout_row_dynamic(nk, 18, 1);
                nk_label_colored(nk, state->status_msg, NK_TEXT_CENTERED, amber);
            }

            nk_group_end(nk);
        }
    }
    nk_end(nk);
}
