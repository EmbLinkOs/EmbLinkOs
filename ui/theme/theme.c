/* Contour: semantic mineral surfaces and desktop geometry. */
#include "theme.h"
#include <math.h>

#define C(r,g,b) ((struct color){(r)/255.f,(g)/255.f,(b)/255.f,1})
static struct ui_theme g_current;
static bool g_init, g_dark=true;
static float g_scale=1.f;
static uint32_t g_regular,g_bold;
static struct color g_accent={.27f,.57f,.52f,1};
static struct ui_appearance g_appearance={.density=1,.transparency=18,.blur=6,.radius=8,.shadows=1};
static struct color mix(struct color a,struct color b,float f) {
    return (struct color){a.r+(b.r-a.r)*f,a.g+(b.g-a.g)*f,a.b+(b.b-a.b)*f,1};
}
static float linear(float c){return c<=.04045f?c/12.92f:powf((c+.055f)/1.055f,2.4f);}
static float luma(struct color c){return .2126f*linear(c.r)+.7152f*linear(c.g)+.0722f*linear(c.b);}
static float ratio(struct color a,struct color b){float x=luma(a),y=luma(b);return x>y?(x+.05f)/(y+.05f):(y+.05f)/(x+.05f);}
static void rebuild(void) {
    struct ui_theme *t=&g_current;*t=(struct ui_theme){0};
    struct color white=C(255,255,255),black=C(14,20,21);
    struct color bg=g_dark?C(24,29,30):C(242,243,240);
    if(g_appearance.surface_style==1)bg=g_dark?C(31,33,31):C(245,243,237);
    if(g_appearance.surface_style==2)bg=g_dark?C(24,28,37):C(239,242,247);
    if(g_appearance.surface_style==3) {
        unsigned rgb=g_appearance.surface_rgb;
        bg=C((rgb>>16)&255,(rgb>>8)&255,rgb&255);
        /* Keep the chosen hue while deriving a readable light/dark ground. */
        float l=luma(bg);
        if(g_dark && l>.18f)bg=mix(bg,black,.75f);
        if(!g_dark && l<.65f)bg=mix(bg,white,.90f);
    }
    t->bg=bg;
    t->surface=mix(bg,white,g_dark?.035f:.70f);
    t->surface_alt=mix(bg,g_dark?white:black,g_dark?.065f:.035f);
    t->sidebar=mix(bg,g_dark?black:white,g_dark?.14f:.24f);
    t->chrome=mix(bg,white,g_dark?.025f:.25f);
    t->border=mix(bg,g_dark?white:black,g_dark?.13f:.14f);
    t->border_strong=mix(bg,g_dark?white:black,g_dark?.26f:.28f);
    t->text=g_dark?C(236,240,236):C(30,39,38);
    t->text_secondary=g_dark?C(179,190,183):C(83,97,91);
    t->text_tertiary=g_dark?C(148,162,154):C(99,112,106);
    if(g_appearance.contrast) {
        t->text=g_dark?white:black;t->text_secondary=t->text;
        t->text_tertiary=g_dark?C(189,201,193):C(65,78,72);
        t->border=mix(bg,g_dark?white:black,g_appearance.contrast==2?.60f:.32f);
        t->border_strong=t->border;
    }
    struct color accent=g_accent;
    for(int i=0;i<24;i++) {
        struct color soft=mix(bg,accent,g_dark?.21f:.13f);
        if(ratio(accent,t->surface)>=4.5f && ratio(accent,bg)>=4.5f && ratio(accent,soft)>=4.5f)break;
        accent=mix(accent,t->text,.12f);
    }
    t->accent=accent;
    t->accent_hover=mix(accent,g_dark?white:black,.12f);
    t->accent_soft=mix(bg,accent,g_dark?.21f:.13f);
    t->selection=mix(bg,accent,g_dark?.55f:.36f);
    float l=luma(accent);
    t->on_accent=(l+.05f)/.05f >= 1.05f/(l+.05f)?C(0,0,0):white;
    t->success=g_dark?C(120,194,149):C(35,117,66);
    t->warning=g_dark?C(229,183,102):C(145,88,17);
    t->danger=g_dark?C(238,136,123):C(181,63,53);
    float den=g_appearance.density==0?.85f:g_appearance.density==2?1.15f:1.f;
    t->sp1=4*den*g_scale;t->sp2=8*den*g_scale;t->sp3=12*den*g_scale;
    t->sp4=16*den*g_scale;t->sp5=24*den*g_scale;t->sp6=32*den*g_scale;t->sp7=48*den*g_scale;
    t->radius_sm=4*g_scale;t->radius_md=6*g_scale;t->radius_lg=g_appearance.radius*g_scale;t->radius_pill=999;
    float type=g_scale*(g_appearance.text_scale>=90?g_appearance.text_scale/100.f:1.f);
    t->text_caption=12*type;t->text_body=13*type;t->text_body_lg=14*type;
    t->text_title=17*type;t->text_heading=23*type;
    t->control_height=(g_appearance.density==0?26:g_appearance.density==2?34:30)*g_scale;
    t->toolbar_height=(g_appearance.density==0?32:g_appearance.density==2?40:36)*g_scale;
    t->control_py=5*den*g_scale;t->control_px=11*den*g_scale;
    t->material_alpha=1-g_appearance.transparency/100.f;
    t->blur=g_appearance.transparency?g_appearance.blur:0;
    t->motion_ms=g_appearance.reduce_motion?0:120;
    t->reduced_motion=g_appearance.reduce_motion;
    t->icon_theme=g_appearance.icon_theme;t->decoration=g_appearance.decoration;
    float shadow=g_appearance.shadows==0?0:g_appearance.shadows==2?1.4f:1.f;
    t->shadow_sm=(struct ui_shadow_spec){0,1,3,{0,0,0,.08f*shadow}};
    t->shadow_md=(struct ui_shadow_spec){0,5,14,{0,0,0,(g_dark?.30f:.12f)*shadow}};
    t->shadow_lg=(struct ui_shadow_spec){0,10,26,{0,0,0,(g_dark?.40f:.18f)*shadow}};
    t->font_regular=g_regular;t->font_bold=g_bold;g_init=true;
}
const struct ui_theme *ui_theme(void){if(!g_init)rebuild();return &g_current;}
void ui_theme_use_dark(bool dark){g_dark=dark;rebuild();}
void ui_theme_set_accent(struct color c){g_accent=c;g_accent.a=1;rebuild();}
void ui_theme_set_scale(float s){if(!(s>=.8f))s=.8f;if(s>1.69f)s=1.69f;g_scale=s;rebuild();}
void ui_theme_set_fonts(uint32_t regular,uint32_t bold){g_regular=regular;g_bold=bold;if(!g_init)rebuild();g_current.font_regular=regular;g_current.font_bold=bold;}
void ui_theme_configure(const struct ui_appearance *a){if(a)g_appearance=*a;rebuild();}
