/* ============================================================================
 * nuklear_impl.c — Single Compilation Unit for Nuklear + SDL/GL2 Backend
 * ============================================================================
 *
 * Nuklear is a stb-style header library. The implementation must be compiled
 * in exactly one .c file. This translation unit serves that purpose.
 * ============================================================================ */

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT

#define NK_IMPLEMENTATION
#include <nuklear.h>

#define NK_SDL_GL2_IMPLEMENTATION
#include "nuklear_sdl_gl2.h"
