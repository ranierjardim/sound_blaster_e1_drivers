/*
 * strings.c - see strings.h.
 */
#include <windows.h>
#include <string.h>
#include "strings.h"

static const wchar_t *const k_en[S__COUNT] = {
#define X_EN(id, en, pt) en,
    STRING_TABLE(X_EN)
#undef X_EN
};
static const wchar_t *const k_pt[S__COUNT] = {
#define X_PT(id, en, pt) pt,
    STRING_TABLE(X_PT)
#undef X_PT
};

static int g_lang = LANG_EN_;

void str_set_lang(int lang) { g_lang = (lang == LANG_PT_) ? LANG_PT_ : LANG_EN_; }
int  str_lang(void) { return g_lang; }

int str_detect_lang(void)
{
    LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_PORTUGUESE ? LANG_PT_ : LANG_EN_;
}

const wchar_t *tr(StrId id)
{
    if ((unsigned)id >= (unsigned)S__COUNT) return L"";
    return g_lang == LANG_PT_ ? k_pt[id] : k_en[id];
}

static const StrId k_named[] = {
    S_EQP_FLAT, S_EQP_ACOUSTIC, S_EQP_CLASSICAL, S_EQP_COUNTRY, S_EQP_DANCE, S_EQP_JAZZ, S_EQP_NEWAGE, S_EQP_POP,
    S_EQP_ROCK, S_EQP_VOCAL, S_VFX_NEUTRAL, S_VFX_F2M, S_VFX_M2F, S_VFX_KID, S_VFX_ELDERLY, S_VFX_ORC, S_VFX_ELF,
    S_VFX_DWARF, S_VFX_ALIEN_BRUTE, S_VFX_ALIEN_INF, S_VFX_ROBOT, S_VFX_MARINE, S_VFX_EMO, S_VFX_UNSTABLE,
    S_VFX_NORTH, S_VFX_DEEP, S_VFX_MUNCHKIN, S_VFX_DEMON,
};

const wchar_t *tr_preset_name(const char *english)
{
    static wchar_t fallback[8][64];
    static int slot;
    wchar_t w[64];
    if (!english) return L"";
    if (!MultiByteToWideChar(CP_UTF8, 0, english, -1, w, 64)) return L"";
    for (size_t i = 0; i < sizeof k_named / sizeof k_named[0]; ++i)
        if (CompareStringOrdinal(k_en[k_named[i]], -1, w, -1, TRUE) == CSTR_EQUAL) return tr(k_named[i]);
    /* Unknown name (table changed): show it untranslated. Small ring of buffers keeps the returned
       pointer valid for a few calls, which is all the callers need. */
    slot = (slot + 1) & 7;
    lstrcpynW(fallback[slot], w, 64);
    return fallback[slot];
}

void str_decimal(wchar_t *s)
{
    if (!s || g_lang != LANG_PT_) return;
    for (size_t i = 1; s[i] && s[i + 1]; ++i)
        if (s[i] == L'.' && s[i - 1] >= L'0' && s[i - 1] <= L'9' && s[i + 1] >= L'0' && s[i + 1] <= L'9') s[i] = L',';
}

const wchar_t *tr_factory(int i)
{
    static const StrId ids[4] = {S_FACTORY_1, S_FACTORY_2, S_FACTORY_3, S_FACTORY_4};
    return (i >= 0 && i < 4) ? tr(ids[i]) : L"";
}
