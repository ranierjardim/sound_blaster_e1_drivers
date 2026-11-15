/*
 * state.c - see state.h.
 */
#include <windows.h>
#include "state.h"

int panel_render_processing(const E1RenderParams *r)
{
    if (!r) return 0;
    const int master = r->master_on != 0, scout = r->scout_on != 0;
    if (!(master || r->eq_on || scout)) return 0;
    if (scout) return 1;                                   /* Scout EQ + surround */
    if (r->output_gain_db != 0.f) return 1;
    int eq = r->eq_on && r->eq_preamp_db != 0.f;
    for (int b = 0; b < E1FX_EQ_BANDS && !eq; ++b)
        if (r->eq_on && r->eq_gain_db[b] != 0.f) eq = 1;
    if (eq) return 1;
    if (!master) return 0;
    return r->surround_on || r->crystal_on || r->bass_on || r->svm_on || r->dialog_on;
}

StrId panel_studio_status(const E1RenderParams *r, int *scout_suffix)
{
    if (scout_suffix) *scout_suffix = 0;
    if (!r) return S_STUDIO_OFF;
    if (r->master_on) {
        if (scout_suffix) *scout_suffix = r->scout_on != 0;
        return S_STUDIO_ON;
    }
    if (r->scout_on) return S_STUDIO_OFF_SCOUT;
    return panel_render_processing(r) ? S_STUDIO_OFF_EQ : S_STUDIO_OFF;
}

int panel_hotkey_allowed(unsigned mods, unsigned vk)
{
    if (vk >= VK_F1 && vk <= VK_F24) return 1;
    return (mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0;
}
