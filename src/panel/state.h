/*
 * state.h - pure decision helpers of the panel (no UI, unit-tested by tests/panel).
 */
#ifndef E1P_STATE_H
#define E1P_STATE_H

#include "../../include/e1fx_params.h"
#include "strings.h"

/* 1 when the render chain processes audio for these parameters (mirrors e1_render_set_params:
   enabled = master || eq_on || scout, and some stage or the output trim is not neutral). */
int   panel_render_processing(const E1RenderParams *r);

/* Status line of the Studio page for these parameters: S_STUDIO_ON, S_STUDIO_OFF (bit-exact),
   S_STUDIO_OFF_EQ or S_STUDIO_OFF_SCOUT. *scout_suffix = 1 when S_SCOUT_FORCED is appended. */
StrId panel_studio_status(const E1RenderParams *r, int *scout_suffix);

/* 1 when (MOD_* mods, virtual key) may become a system-wide hot key: Ctrl, Alt or Win is required
   (Shift alone would swallow ordinary typing such as capital letters), except for F1..F24. */
int   panel_hotkey_allowed(unsigned mods, unsigned vk);

#endif /* E1P_STATE_H */
