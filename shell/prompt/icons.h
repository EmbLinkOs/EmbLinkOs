/* icons.h -- Powerlevel10k's icon table, per POWERLEVEL9K_MODE (icons.c is
 * generated from it by tools/p10k_icons.py). A value's \001 is the icon
 * spacing $s and \002 its legacy counterpart $q; NULL: the mode has none. */
#ifndef __EMBK_P10K_ICONS_H__
#define __EMBK_P10K_ICONS_H__

enum { P10K_ICON_FLAT, P10K_ICON_AWESOME_FONTCONFIG, P10K_ICON_NERDFONT_V3, P10K_ICON_NERDFONT_COMPLETE,
       P10K_ICON_ASCII, P10K_ICON_DEFAULT, P10K_ICON_MODES };
struct p10k_icon { const char *name; const char *value[P10K_ICON_MODES]; };
extern const char *const p10k_icon_modes[P10K_ICON_MODES];
extern const struct p10k_icon p10k_icons[];
extern const unsigned p10k_icon_count;

#endif
