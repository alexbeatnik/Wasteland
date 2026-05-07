#ifndef WASTELAND_CAPABILITY_H
#define WASTELAND_CAPABILITY_H

/* ---------------------------------------------------------------------------
 * capability.h — Agent capability presets and policy enforcement.
 *
 * Replaces the binary agent_mode toggle with a tiered policy system.
 * --------------------------------------------------------------------------- */

#include "agent.h"

typedef enum {
    CAP_PRESET_OFF = 0,
    CAP_PRESET_READ_ONLY,   /* read_file + list_dir */
    CAP_PRESET_READ_WRITE,  /* all four tools */
    CAP_PRESET_CUSTOM       /* per-tool checkboxes */
} cap_preset_t;

/**
 * @brief Map a preset enum to a human-readable label.
 * @return 0 on success, -1 if preset is invalid.
 */
int capability_preset_label(cap_preset_t preset, char *out, size_t out_size);

/**
 * @brief Check whether `tool` is permitted under the given preset.
 *
 * For CAP_PRESET_CUSTOM the state is queried from the last
 * capability_custom_set() call.
 *
 * @return 1 if allowed, 0 if denied.
 */
int capability_tool_allowed(cap_preset_t preset, agent_tool_t tool);

/**
 * @brief Configure the custom bitmask (used when preset is CUSTOM).
 *
 * Bits correspond to agent_tool_t values (1<<1, 1<<2, 1<<3, 1<<4).
 */
void capability_custom_set(int bitmask);

#endif /* WASTELAND_CAPABILITY_H */
