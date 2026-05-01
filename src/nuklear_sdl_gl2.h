#ifndef WASTELAND_NK_SDL_GL2_H
#define WASTELAND_NK_SDL_GL2_H

/* ---------------------------------------------------------------------------
 * Nuklear SDL2 + OpenGL 2.1 Backend
 * ---------------------------------------------------------------------------
 * This header provides the glue between Nuklear (immediate-mode GUI),
 * SDL2 (windowing / events), and OpenGL 2.1 (rendering).
 *
 * In **exactly one** .c file define NK_SDL_GL2_IMPLEMENTATION before
 * including this header so the implementation is compiled once.
 * --------------------------------------------------------------------------- */

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <GL/gl.h>

/* Nuklear forward declarations (assumes <nuklear.h> is included before this) */
NK_API struct nk_context* nk_sdl_init(SDL_Window *win);
NK_API void               nk_sdl_shutdown(void);
NK_API void               nk_sdl_handle_event(SDL_Event *evt);
NK_API void               nk_sdl_render(enum nk_anti_aliasing AA,
                                        int max_vertex_buffer,
                                        int max_element_buffer);

#endif /* WASTELAND_NK_SDL_GL2_H */

/* ========================================================================= */
#ifdef NK_SDL_GL2_IMPLEMENTATION
/* ========================================================================= */

#include <string.h>
#include <stdlib.h>

struct nk_sdl_device {
    struct nk_buffer cmds;
    struct nk_draw_null_texture null;
    GLuint font_tex;
};

struct nk_sdl_vertex {
    float position[2];
    float uv[2];
    nk_byte col[4];
};

static struct nk_sdl {
    SDL_Window *win;
    struct nk_context ctx;
    struct nk_font_atlas atlas;
    struct nk_sdl_device dev;
} sdl;

NK_API struct nk_context*
nk_sdl_init(SDL_Window *win)
{
    sdl.win = win;
    nk_init_default(&sdl.ctx, 0);
    nk_buffer_init_default(&sdl.dev.cmds);
    sdl.dev.font_tex = 0;

    /* --- Font baking --- */
    nk_font_atlas_init_default(&sdl.atlas);
    nk_font_atlas_begin(&sdl.atlas);

    struct nk_font_config cfg = nk_font_config(14);
    struct nk_font *font = nk_font_atlas_add_default(&sdl.atlas, 14, &cfg);

    int w, h;
    const void *image = nk_font_atlas_bake(&sdl.atlas, &w, &h, NK_FONT_ATLAS_RGBA32);

    glGenTextures(1, &sdl.dev.font_tex);
    glBindTexture(GL_TEXTURE_2D, sdl.dev.font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, image);

    nk_font_atlas_end(&sdl.atlas, nk_handle_id((int)sdl.dev.font_tex), &sdl.dev.null);
    if (font)
        nk_style_set_font(&sdl.ctx, &font->handle);

    return &sdl.ctx;
}

NK_API void
nk_sdl_shutdown(void)
{
    nk_font_atlas_clear(&sdl.atlas);
    nk_free(&sdl.ctx);
    glDeleteTextures(1, &sdl.dev.font_tex);
    nk_buffer_free(&sdl.dev.cmds);
    memset(&sdl, 0, sizeof(sdl));
}

NK_API void
nk_sdl_handle_event(SDL_Event *evt)
{
    struct nk_context *ctx = &sdl.ctx;

    if (evt->type == SDL_KEYUP || evt->type == SDL_KEYDOWN) {
        int down = (evt->type == SDL_KEYDOWN);
        SDL_Keycode sym = evt->key.keysym.sym;

        if (sym == SDLK_RSHIFT || sym == SDLK_LSHIFT)
            nk_input_key(ctx, NK_KEY_SHIFT, down);
        else if (sym == SDLK_DELETE)
            nk_input_key(ctx, NK_KEY_DEL, down);
        else if (sym == SDLK_RETURN)
            nk_input_key(ctx, NK_KEY_ENTER, down);
        else if (sym == SDLK_TAB)
            nk_input_key(ctx, NK_KEY_TAB, down);
        else if (sym == SDLK_BACKSPACE)
            nk_input_key(ctx, NK_KEY_BACKSPACE, down);
        else if (sym == SDLK_HOME)
            nk_input_key(ctx, NK_KEY_TEXT_START, down);
        else if (sym == SDLK_END)
            nk_input_key(ctx, NK_KEY_TEXT_END, down);
        else if (sym == SDLK_z)
            nk_input_key(ctx, NK_KEY_TEXT_UNDO,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_r)
            nk_input_key(ctx, NK_KEY_TEXT_REDO,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_c)
            nk_input_key(ctx, NK_KEY_COPY,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_v)
            nk_input_key(ctx, NK_KEY_PASTE,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_x)
            nk_input_key(ctx, NK_KEY_CUT,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_b)
            nk_input_key(ctx, NK_KEY_TEXT_LINE_START,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_e)
            nk_input_key(ctx, NK_KEY_TEXT_LINE_END,
                         down && (evt->key.keysym.mod & KMOD_CTRL));
        else if (sym == SDLK_UP)
            nk_input_key(ctx, NK_KEY_UP, down);
        else if (sym == SDLK_DOWN)
            nk_input_key(ctx, NK_KEY_DOWN, down);
        else if (sym == SDLK_LEFT)
            nk_input_key(ctx, NK_KEY_LEFT, down);
        else if (sym == SDLK_RIGHT)
            nk_input_key(ctx, NK_KEY_RIGHT, down);
    }
    else if (evt->type == SDL_TEXTINPUT) {
        const char *text = evt->text.text;
        nk_rune unicode;
        int text_len = (int)strlen(text);
        int glyph_len = nk_utf_decode(text, &unicode, text_len);
        while (glyph_len > 0 && text_len > 0) {
            nk_input_unicode(ctx, unicode);
            text += glyph_len;
            text_len -= glyph_len;
            glyph_len = nk_utf_decode(text, &unicode, text_len);
        }
    }
    else if (evt->type == SDL_MOUSEMOTION) {
        nk_input_motion(ctx, evt->motion.x, evt->motion.y);
    }
    else if (evt->type == SDL_MOUSEBUTTONDOWN || evt->type == SDL_MOUSEBUTTONUP) {
        int down = (evt->type == SDL_MOUSEBUTTONDOWN);
        int x = evt->button.x;
        int y = evt->button.y;
        if (evt->button.button == SDL_BUTTON_LEFT)
            nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down);
        if (evt->button.button == SDL_BUTTON_MIDDLE)
            nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down);
        if (evt->button.button == SDL_BUTTON_RIGHT)
            nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down);
    }
    else if (evt->type == SDL_MOUSEWHEEL) {
        /* Nuklear scroll Y is positive for scrolling down,
           SDL wheel Y is positive for scrolling up. */
        nk_input_scroll(ctx, nk_vec2((float)evt->wheel.x,
                                     (float)evt->wheel.y * -1.0f));
    }
}

NK_API void
nk_sdl_render(enum nk_anti_aliasing AA, int max_vertex_buffer, int max_element_buffer)
{
    struct nk_context *ctx = &sdl.ctx;
    struct nk_sdl_device *dev = &sdl.dev;

    int width, height;
    int display_width, display_height;
    SDL_GetWindowSize(sdl.win, &width, &height);
    SDL_GL_GetDrawableSize(sdl.win, &display_width, &display_height);

    struct nk_vec2 scale;
    scale.x = (float)display_width / (float)width;
    scale.y = (float)display_height / (float)height;

    /* ---- Setup global GL state ---- */
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushMatrix();

    glViewport(0, 0, display_width, display_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_SCISSOR_TEST);

    glLoadIdentity();
    glOrtho(0.0f, (double)width, (double)height, 0.0f, -1.0f, 1.0f);

    /* ---- Convert Nuklear commands to vertex buffers ---- */
    {
        const struct nk_draw_command *cmd;
        void *vertices, *elements;
        nk_size offset = 0;

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);

        {
            struct nk_convert_config config;
            static const struct nk_draw_vertex_layout_element vertex_layout[] = {
                {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_sdl_vertex, position)},
                {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_sdl_vertex, uv)},
                {NK_VERTEX_COLOR,    NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nk_sdl_vertex, col)},
                {NK_VERTEX_LAYOUT_END}
            };
            memset(&config, 0, sizeof(config));
            config.vertex_layout = vertex_layout;
            config.vertex_size = sizeof(struct nk_sdl_vertex);
            config.vertex_alignment = NK_ALIGNOF(struct nk_sdl_vertex);
            config.tex_null = dev->null;
            config.circle_segment_count = 22;
            config.curve_segment_count = 22;
            config.arc_segment_count = 22;
            config.global_alpha = 1.0f;
            config.shape_AA = AA;
            config.line_AA = AA;

            struct nk_buffer vbuf, ebuf;
            nk_buffer_init_fixed(&vbuf, malloc((size_t)max_vertex_buffer),
                                 (nk_size)max_vertex_buffer);
            nk_buffer_init_fixed(&ebuf, malloc((size_t)max_element_buffer),
                                 (nk_size)max_element_buffer);

            nk_convert(ctx, &dev->cmds, &vbuf, &ebuf, &config);

            vertices = nk_buffer_memory(&vbuf);
            elements = nk_buffer_memory(&ebuf);

            glVertexPointer(2, GL_FLOAT, (GLsizei)sizeof(struct nk_sdl_vertex),
                (void *)((size_t)vertices + NK_OFFSETOF(struct nk_sdl_vertex, position)));
            glTexCoordPointer(2, GL_FLOAT, (GLsizei)sizeof(struct nk_sdl_vertex),
                (void *)((size_t)vertices + NK_OFFSETOF(struct nk_sdl_vertex, uv)));
            glColorPointer(4, GL_UNSIGNED_BYTE, (GLsizei)sizeof(struct nk_sdl_vertex),
                (void *)((size_t)vertices + NK_OFFSETOF(struct nk_sdl_vertex, col)));

            /* Determine element type based on sizeof(nk_draw_index) */
            GLenum index_type = (sizeof(nk_draw_index) == 2) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;

            nk_draw_foreach(cmd, ctx, &dev->cmds)
            {
                if (!cmd->elem_count) continue;

                glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
                glScissor(
                    (GLint)(cmd->clip_rect.x * scale.x),
                    (GLint)((display_height - (cmd->clip_rect.y + cmd->clip_rect.h)) * scale.y),
                    (GLint)(cmd->clip_rect.w * scale.x),
                    (GLint)(cmd->clip_rect.h * scale.y));

                glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, index_type,
                    (const void *)((size_t)elements + offset * sizeof(nk_draw_index)));

                offset += cmd->elem_count;
            }

            nk_clear(ctx);
            nk_buffer_clear(&dev->cmds);
            free(nk_buffer_memory(&vbuf));
            free(nk_buffer_memory(&ebuf));
        }

        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();
    glPopAttrib();
}

#endif /* NK_SDL_GL2_IMPLEMENTATION */
