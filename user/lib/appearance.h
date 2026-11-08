#ifndef EMBLINK_APPEARANCE_H
#define EMBLINK_APPEARANCE_H
#include "oscfg.h"
#include "em.h"
/* One application of preferences, shared by apps and restricted shell parts.
 * Font/image handles are cached by EmUI. No I/O is performed by the theme. */
static inline void oscfg_apply_appearance(const struct oscfg *c) {
    oscfg_apply_tz(c);
    struct ui_appearance a={.surface_style=c->surface_style,.density=c->density,
        .contrast=c->contrast,.transparency=c->transparency,.blur=c->blur,
        .reduce_motion=c->reduce_motion,.radius=c->window_radius,.shadows=c->shadows,
        .icon_theme=c->icon_theme,.decoration=c->decoration,.text_scale=c->text_scale,.surface_rgb=(unsigned)c->custom_surface};
    ui_theme_configure(&a);
    ui_theme_use_dark(c->dark!=0);
    ui_theme_set_scale(c->ui_scale/100.f);
    const struct oscfg_accent *accent=&oscfg_accents[c->accent];
    unsigned rgb=(unsigned)c->custom_accent;
    Color col=c->accent_custom?(Color){((rgb>>16)&255)/255.f,((rgb>>8)&255)/255.f,(rgb&255)/255.f,1}:
        (Color){accent->r,accent->g,accent->b,1};
    ui_theme_set_accent(col);
    static const char *regular[]={"/system/fonts/font.ttf","/system/fonts/condensed.ttf","/system/fonts/serif.ttf"};
    static const char *bold[]={"/system/fonts/bold.ttf","/system/fonts/condensed-bold.ttf","/system/fonts/serif-bold.ttf"};
    ui_theme_set_fonts(em_font(regular[c->font]),em_font(bold[c->font]));
}
static inline unsigned oscfg_rgb(Color c) {
    return ((unsigned)(c.r*255+.5f)<<16)|((unsigned)(c.g*255+.5f)<<8)|(unsigned)(c.b*255+.5f);
}
static inline int oscfg_publish_desktop(const struct oscfg *c) {
    const struct ui_theme *t=ui_theme();int l,top,r,b;oscfg_insets(c,&l,&top,&r,&b);
    struct embk_desktop d={.version=1,.flags=(c->reduce_motion?EMBK_DESKTOP_REDUCED_MOTION:0)|
        (c->snap?EMBK_DESKTOP_SNAP:0)|(c->workspace_wrap?EMBK_DESKTOP_WRAP:0)|
        (c->effects==1?EMBK_DESKTOP_EFFECTS_FULL:c->effects==2?EMBK_DESKTOP_EFFECTS_REDUCED:0),
        .left=l,.top=top,.right=r,.bottom=b,.background=oscfg_rgb(t->chrome),.text=oscfg_rgb(t->text),.accent=oscfg_rgb(t->accent),.danger=oscfg_rgb(t->danger),.secondary=oscfg_rgb(t->text_tertiary),
        .radius=c->window_radius,.shadows=c->shadows,.decoration=c->decoration,.cursor_style=c->cursor_style,
        .cursor_size=c->cursor_size,.workspace_count=c->workspace_count,.shortcut_modifier=c->shortcut_modifier,.blur=c->transparency?c->blur:0,.opacity=(unsigned)(255*(100-c->transparency)/100)};
    return embk_desktop_control(EMBK_DESKTOP_CONFIGURE,&d);
}
#endif
