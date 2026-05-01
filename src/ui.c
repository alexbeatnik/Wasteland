/* ============================================================================
 * ui.c — Nuklear Layout & Amber Theme
 * ============================================================================ */

#include "ui.h"
#include "network.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>

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
 * --------------------------------------------------------------------------- */
static const char *hub_models[WASTELAND_MAX_HUB_MODELS] = {
    "ggml-org/gemma-4-E2B-it-GGUF",
    "ggml-org/gemma-4-26B-A4B-it-GGUF",
    "ggml-org/gemma-4-E4B-it-GGUF",
    "ggml-org/gemma-4-31B-it-GGUF"
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
 * --------------------------------------------------------------------------- */
int scan_local_models(char models_list[][WASTELAND_MAX_MODEL_PATH_LEN], int max_models)
{
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA("models\\*.gguf", &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (count < max_models) {
            snprintf(models_list[count], WASTELAND_MAX_MODEL_PATH_LEN,
                     "models/%s", ffd.cFileName);
            count++;
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    FindClose(hFind);
    return count;
#else
    DIR *d = opendir("models");
    if (!d) return 0;

    int count = 0;
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
    return count;
#endif
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
 * count_wrap_lines
 *
 * Estimate how many wrapped lines `nk_label_*_wrap` will produce for `text`
 * given an available pixel width. Uses the active font's own width measure
 * so it stays accurate for non-monospace fonts. Greedy: fits as many chars
 * per line as possible, then prefers to break at the last whitespace.
 * --------------------------------------------------------------------------- */
/* utf8_safe_fit: trim `fit` bytes so we don't cut a multi-byte UTF-8
 * sequence in the middle.  UTF-8 continuation bytes are 0x80..0xBF. */
static int utf8_safe_fit(const char *text, int fit)
{
    while (fit > 0 && (text[fit] & 0xC0) == 0x80) fit--;
    return fit > 0 ? fit : 1;
}

static int count_wrap_lines(const struct nk_user_font *font,
                            const char *text, int len, float panel_w)
{
    if (len <= 0 || !font || !font->width || panel_w <= 1.0f) return 1;
    int lines = 0;
    int off = 0;
    while (off < len) {
        int lo = 1, hi = len - off, fit = 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            float w = font->width(font->userdata, font->height,
                                  text + off, mid);
            if (w <= panel_w) { fit = mid; lo = mid + 1; }
            else              { hi = mid - 1; }
        }
        /* Ensure we don't cut mid-UTF-8-sequence */
        fit = utf8_safe_fit(text + off, fit);
        if (off + fit < len) {
            /* Prefer to break at a space (ASCII 0x20) */
            int i = fit;
            while (i > fit / 2 && text[off + i - 1] != ' ') i--;
            if (i > fit / 2) fit = i;
        }
        off += fit;
        while (off < len && text[off] == ' ') off++;
        lines++;
        if (lines > 200) break; /* sanity */
    }
    return lines > 0 ? lines : 1;
}

/* ---------------------------------------------------------------------------
 * ui_render
 * --------------------------------------------------------------------------- */
void ui_render(struct nk_context *nk, app_state_t *state, int width, int height)
{
    ui_apply_amber_theme(nk);

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
    }

    if (nk_begin(nk, "Wasteland",
                 nk_rect(0, 0, width, height),
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND))
    {
        /* ========================= HEADER ========================= */
        nk_layout_row_dynamic(nk, 30, 1);
        nk_label_colored(nk, "WASTELAND TERMINAL v0.1",
                         NK_TEXT_CENTERED, amber);

        nk_layout_row_dynamic(nk, 20, 3);
        nk_label_colored(nk, "SYS: ONLINE", NK_TEXT_LEFT, amber);

        char status[128];
        if (state->is_generating) {
            snprintf(status, sizeof(status), "INF: GENERATING...");
        } else if (state->network_lockdown) {
            snprintf(status, sizeof(status), "SEC: LOCKDOWN ACTIVE");
        } else {
            snprintf(status, sizeof(status), "SEC: UNLOCKED");
        }
        nk_label_colored(nk, status, NK_TEXT_CENTERED, amber);

        if (state->network_lockdown) {
            nk_label_colored(nk, "NET: DISCONNECTED",
                             NK_TEXT_RIGHT, amber);
        } else {
            nk_label_colored(nk, "NET: AVAILABLE",
                             NK_TEXT_RIGHT, amber);
        }

        /* =================== MAIN SPLIT AREA =================== */
        nk_layout_row_dynamic(nk, height - 70, 2);

        /* --------------------- LEFT PANEL --------------------- */
        if (nk_group_begin(nk, "LeftPanel", NK_WINDOW_BORDER)) {

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
                        pthread_t tid;
                        pthread_create(&tid, NULL,
                                       download_thread_fn, state);
                        pthread_detach(tid);
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
                    nk_layout_row_dynamic(nk, 26, 2);

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

                    if (nk_button_label(nk, "[ DELETE ]") && !load_busy && !gen_busy) {
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

            if (state->status_msg[0]) {
                nk_layout_row_dynamic(nk, 20, 1);
                nk_label_colored(nk, state->status_msg,
                                 NK_TEXT_CENTERED, amber);
            }

            nk_group_end(nk);
        }

        /* --------------------- RIGHT PANEL --------------------- */
        if (nk_group_begin(nk, "RightPanel", NK_WINDOW_BORDER)) {
            /* Chat history rendered into a scrollable group of labels.
             * nk_edit_string provides no public scroll API, so we draw
             * the lines ourselves and auto-pin scroll to the bottom
             * whenever new tokens arrive. */
            size_t chat_len = strlen(state->chat_history);
            if (chat_len > state->chat_last_len) {
                /* New text appeared — clamp scroll to the bottom. */
                state->chat_scroll_y = (nk_uint)0x7FFFFFFF;
            }
            state->chat_last_len = chat_len;

            /* Chat area: total right-panel minus header/footer controls.
             * Controls below consume: spacer(10)+label(25)+input(30)+btn(30)=95px
             * plus Nuklear group padding (~20px) → reserve 200px total. */
            nk_layout_row_dynamic(nk, height - 200, 1);
            nk_uint sx = state->chat_scroll_x;
            nk_uint sy = state->chat_scroll_y;
            if (nk_group_scrolled_offset_begin(nk, &sx, &sy,
                                               "ChatHistory",
                                               NK_WINDOW_BORDER))
            {
                /* Probe the available content width inside the group by
                 * placing one tiny invisible row and reading its bounds.
                 * Subsequent rows use the same width via row_dynamic. */
                const struct nk_user_font *font = nk->style.font;
                float font_h = font ? font->height : 14.0f;

                nk_layout_row_dynamic(nk, 1, 1);
                struct nk_rect probe = nk_widget_bounds(nk);
                nk_spacing(nk, 1);
                /* Reserve a few px so the wrap measure is not flush with
                 * the right edge — Nuklear's wrapper has its own padding. */
                float panel_w = probe.w - 8.0f;
                if (panel_w < 32.0f) panel_w = 32.0f;

                /* -------------------------------------------------------
                 * Chat rendering state machine:
                 *   - detects ```code blocks``` → [COPY CODE] button
                 *   - detects assistant response blocks → [COPY] button
                 * All copy buttons live inside the scroll group so the
                 * input area below is never affected.
                 * ------------------------------------------------------- */

                /* Persistent per-frame accumulators (static = no stack pressure) */
                static char s_code_buf[8192];
                static char s_asst_buf[8192];
                int s_code_len = 0;
                int s_asst_len = 0;
                s_code_buf[0] = '\0';
                s_asst_buf[0] = '\0';

                int in_code = 0; /* currently inside a ```...``` block */

                /* amber_dim: slightly dimmer colour for code-block chrome */
                struct nk_color amber_dim = nk_rgb(0xCC, 0x8C, 0x00);

                const char *p   = state->chat_history;
                const char *end = p + chat_len;
                while (p < end) {
                    const char *eol = memchr(p, '\n', (size_t)(end - p));
                    size_t llen = eol ? (size_t)(eol - p)
                                      : (size_t)(end - p);

                    char line[4096];
                    if (llen >= sizeof(line)) llen = sizeof(line) - 1;
                    memcpy(line, p, llen);
                    line[llen] = '\0';

                    /* Is this a ``` fence line? */
                    int is_fence = (llen >= 3 &&
                                    line[0] == '`' &&
                                    line[1] == '`' &&
                                    line[2] == '`');

                    /* Is this a user-prompt line?  ("> text") */
                    int is_user = (!in_code && llen >= 2 &&
                                   line[0] == '>' && line[1] == ' ');

                    if (is_fence) {
                        if (!in_code) {
                            /* ---- Opening fence ---- */
                            /* Flush any pending assistant block first */
                            if (s_asst_len > 0) {
                                /* keep a snapshot so the button closure is safe */
                                static char snap_asst[8192];
                                strncpy(snap_asst, s_asst_buf,
                                        sizeof(snap_asst) - 1);
                                snap_asst[sizeof(snap_asst) - 1] = '\0';
                                nk_layout_row_dynamic(nk, 22, 1);
                                if (nk_button_label(nk, "[ COPY ]")) {
                                    SDL_SetClipboardText(snap_asst);
                                    snprintf(state->status_msg,
                                             sizeof(state->status_msg),
                                             "Response copied to clipboard.");
                                }
                                s_asst_len = 0;
                                s_asst_buf[0] = '\0';
                            }
                            in_code = 1;
                            s_code_len = 0;
                            s_code_buf[0] = '\0';
                            /* Thin separator + language hint */
                            nk_layout_row_dynamic(nk, 2, 1);
                            nk_button_color(nk, amber_dim);
                            char hdr[80];
                            if (llen > 3) {
                                char lang[48] = "";
                                size_t hl = llen - 3;
                                if (hl >= sizeof(lang)) hl = sizeof(lang) - 1;
                                memcpy(lang, line + 3, hl);
                                lang[hl] = '\0';
                                snprintf(hdr, sizeof(hdr),
                                         "-- CODE: %s --", lang);
                            } else {
                                snprintf(hdr, sizeof(hdr), "-- CODE --");
                            }
                            nk_layout_row_dynamic(nk, font_h + 2, 1);
                            nk_label_colored(nk, hdr, NK_TEXT_LEFT, amber_dim);
                        } else {
                            /* ---- Closing fence ---- */
                            in_code = 0;
                            /* Copy button for this code block */
                            static char snap_code[8192];
                            strncpy(snap_code, s_code_buf,
                                    sizeof(snap_code) - 1);
                            snap_code[sizeof(snap_code) - 1] = '\0';
                            nk_layout_row_dynamic(nk, 24, 1);
                            if (nk_button_label(nk, "[ COPY CODE ]")) {
                                SDL_SetClipboardText(snap_code);
                                snprintf(state->status_msg,
                                         sizeof(state->status_msg),
                                         "Code block copied to clipboard.");
                            }
                            nk_layout_row_dynamic(nk, 2, 1);
                            nk_button_color(nk, amber_dim);
                        }
                    } else if (in_code) {
                        /* Accumulate code content */
                        if (s_code_len + (int)llen + 1 < (int)sizeof(s_code_buf)) {
                            memcpy(s_code_buf + s_code_len, line, llen);
                            s_code_len += (int)llen;
                            s_code_buf[s_code_len++] = '\n';
                            s_code_buf[s_code_len]   = '\0';
                        }
                        /* Render code line with slight indent marker */
                        char indented[4098];
                        snprintf(indented, sizeof(indented), "  %s", line);
                        int ilen = (int)strlen(indented);
                        int wraps = count_wrap_lines(font, indented,
                                                     ilen, panel_w);
                        nk_layout_row_dynamic(nk,
                                              font_h * (float)wraps + 2.0f,
                                              1);
                        nk_label_colored_wrap(nk, indented, amber_dim);
                    } else if (is_user) {
                        /* Flush any previous assistant block */
                        if (s_asst_len > 0) {
                            static char snap_asst[8192];
                            strncpy(snap_asst, s_asst_buf,
                                    sizeof(snap_asst) - 1);
                            snap_asst[sizeof(snap_asst) - 1] = '\0';
                            nk_layout_row_dynamic(nk, 22, 1);
                            if (nk_button_label(nk, "[ COPY ]")) {
                                SDL_SetClipboardText(snap_asst);
                                snprintf(state->status_msg,
                                         sizeof(state->status_msg),
                                         "Response copied to clipboard.");
                            }
                            s_asst_len = 0;
                            s_asst_buf[0] = '\0';
                        }
                        /* Render user line */
                        if (llen == 0) {
                            nk_layout_row_dynamic(nk, font_h, 1);
                            nk_label_colored(nk, "", NK_TEXT_LEFT, amber);
                        } else {
                            int wraps = count_wrap_lines(font, line,
                                                         (int)llen, panel_w);
                            nk_layout_row_dynamic(nk,
                                                  font_h * (float)wraps + 2.0f,
                                                  1);
                            nk_label_colored_wrap(nk, line, amber);
                        }
                    } else {
                        /* Normal assistant line — accumulate + render */
                        if (llen > 0 &&
                            s_asst_len + (int)llen + 1 < (int)sizeof(s_asst_buf)) {
                            memcpy(s_asst_buf + s_asst_len, line, llen);
                            s_asst_len += (int)llen;
                            s_asst_buf[s_asst_len++] = '\n';
                            s_asst_buf[s_asst_len]   = '\0';
                        }
                        if (llen == 0) {
                            nk_layout_row_dynamic(nk, font_h, 1);
                            nk_label_colored(nk, "", NK_TEXT_LEFT, amber);
                        } else {
                            int wraps = count_wrap_lines(font, line,
                                                         (int)llen, panel_w);
                            nk_layout_row_dynamic(nk,
                                                  font_h * (float)wraps + 2.0f,
                                                  1);
                            nk_label_colored_wrap(nk, line, amber);
                        }
                    }
                    p = eol ? eol + 1 : end;
                }

                /* Flush final assistant block if any */
                if (s_asst_len > 0 && !in_code) {
                    static char snap_asst[8192];
                    strncpy(snap_asst, s_asst_buf, sizeof(snap_asst) - 1);
                    snap_asst[sizeof(snap_asst) - 1] = '\0';
                    nk_layout_row_dynamic(nk, 22, 1);
                    if (nk_button_label(nk, "[ COPY ]")) {
                        SDL_SetClipboardText(snap_asst);
                        snprintf(state->status_msg,
                                 sizeof(state->status_msg),
                                 "Response copied to clipboard.");
                    }
                }

                nk_group_scrolled_end(nk);
            }
            state->chat_scroll_x = sx;
            state->chat_scroll_y = sy;

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
                    pthread_mutex_lock(&state->chat_mutex);
                    size_t hlen = strlen(state->chat_history);
                    size_t room = WASTELAND_MAX_CHAT_HISTORY - hlen - 1;
                    if (room > 0) {
                        snprintf(state->chat_history + hlen, room,
                                 "\n> %s\n", state->input_buffer);
                    }
                    pthread_mutex_unlock(&state->chat_mutex);

                    inference_submit_prompt(state->inference,
                                            state->input_buffer);
                    state->input_buffer[0] = '\0';
                }
            }

            nk_layout_row_end(nk);


            nk_group_end(nk);
        }
    }
    nk_end(nk);
}
