/* ============================================================================
 * capability.c — Agent capability presets and policy enforcement.
 * ============================================================================ */

#include "capability.h"
#include <string.h>
#include <stdio.h>

static int g_custom_bitmask = 0;

int capability_preset_label(cap_preset_t preset, char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    const char *label;
    switch (preset) {
    case CAP_PRESET_OFF:        label = "OFF"; break;
    case CAP_PRESET_READ_ONLY:  label = "READ ONLY"; break;
    case CAP_PRESET_READ_WRITE: label = "READ + WRITE"; break;
    case CAP_PRESET_CUSTOM:     label = "CUSTOM"; break;
    default:                    label = "UNKNOWN"; break;
    }
    snprintf(out, out_size, "%s", label);
    return 0;
}

int capability_tool_allowed(cap_preset_t preset, agent_tool_t tool)
{
    switch (preset) {
    case CAP_PRESET_OFF:
        return 0;
    case CAP_PRESET_READ_ONLY:
        return (tool == AGENT_TOOL_READ_FILE || tool == AGENT_TOOL_LIST_DIR);
    case CAP_PRESET_READ_WRITE:
        return (tool == AGENT_TOOL_READ_FILE || tool == AGENT_TOOL_LIST_DIR ||
                tool == AGENT_TOOL_WRITE_FILE || tool == AGENT_TOOL_APPLY_EDIT);
    case CAP_PRESET_CUSTOM:
        return (g_custom_bitmask & (1 << (int)tool)) != 0;
    }
    return 0;
}

void capability_custom_set(int bitmask)
{
    g_custom_bitmask = bitmask;
}
