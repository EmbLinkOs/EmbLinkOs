/* Host contract checks: palette contrast, old config migration, complete bounded
 * round trips and presets that leave non-appearance preferences alone. */
#include "oscfg.h"
#include "theme.h"
#include <math.h>
#include <limits.h>
#include <stdio.h>
static int failures,checks;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static float linear(float v){return v<=.04045f?v/12.92f:powf((v+.055f)/1.055f,2.4f);}
static float lum(struct color c){return .2126f*linear(c.r)+.7152f*linear(c.g)+.0722f*linear(c.b);}
static float contrast(struct color a,struct color b){float x=lum(a),y=lum(b);return x>y?(x+.05f)/(y+.05f):(y+.05f)/(x+.05f);}
int main(void) {
    struct oscfg c,copy;oscfg_defaults(&c);
    char old[]="dark 0\naccent 5\ndock_size 55\nbar_cpu 1\n";oscfg_parse(&c,old);oscfg_clamp(&c);
    CHECK(c.dark==0 && c.accent==5 && c.dock_size==55 && c.bar_cpu==1 && c.workspace_count==4);
    char out[2048];int n=oscfg_format(&c,out,sizeof out);CHECK(n>640 && n<(int)sizeof out);
    oscfg_defaults(&copy);oscfg_parse(&copy,out);oscfg_clamp(&copy);CHECK(!memcmp(&copy,&c,sizeof c));
    CHECK(oscfg_format(&c,out,20)<0);
    c.display_width=1920;c.display_height=1080;
    n=oscfg_format(&c,out,sizeof out);CHECK(n>0);
    oscfg_defaults(&copy);oscfg_parse(&copy,out);oscfg_clamp(&copy);
    CHECK(copy.display_width==1920 && copy.display_height==1080);
    for(int i=0;i<OSCFG_PRESETS;i++){oscfg_preset(&copy,i);CHECK(copy.display_width==1920 && copy.display_height==1080);}
    copy.display_width=INT_MAX;oscfg_clamp(&copy);CHECK(!copy.display_width && !copy.display_height);
    copy.display_width=1280;copy.display_height=0;oscfg_clamp(&copy);CHECK(!copy.display_width && !copy.display_height);
    #define SET_MAX(name,value,lo,hi) c.name=INT_MAX;
    OSCFG_CONTOUR_FIELDS(SET_MAX)
    #undef SET_MAX
    oscfg_clamp(&c);
    #define CHECK_MAX(name,value,lo,hi) CHECK(c.name==hi);
    OSCFG_CONTOUR_FIELDS(CHECK_MAX)
    #undef CHECK_MAX
    #define SET_MIN(name,value,lo,hi) c.name=INT_MIN;
    OSCFG_CONTOUR_FIELDS(SET_MIN)
    #undef SET_MIN
    oscfg_clamp(&c);
    #define CHECK_MIN(name,value,lo,hi) CHECK(c.name==lo);
    OSCFG_CONTOUR_FIELDS(CHECK_MIN)
    #undef CHECK_MIN
    oscfg_defaults(&c);c.volume=23;c.muted=1;c.tz=9;c.keymap=2;c.shortcut_modifier=1;
    for(int i=0;i<OSCFG_PRESETS;i++){oscfg_preset(&c,i);CHECK(c.volume==23&&c.muted==1&&c.tz==9&&c.keymap==2&&c.shortcut_modifier==1);}
    for(int edge=0;edge<3;edge++)for(int bar=0;bar<2;bar++){
        c.dock_edge=edge;c.bar_edge=bar;int l,t,r,b;oscfg_insets(&c,&l,&t,&r,&b);
        CHECK(l+r+b+t==oscfg_dock_band(&c)+c.bar_size);
        CHECK((edge==1)==(l>0));CHECK((edge==2)==(r>0));
    }
    ui_theme_set_fonts(11,22);
    for(int dark=0;dark<2;dark++)for(int surface=0;surface<4;surface++)for(int high=0;high<3;high++){
        struct ui_appearance a={.surface_style=surface,.surface_rgb=0xff7700,.density=1,.contrast=high,.transparency=0,.reduce_motion=1,.radius=8,.shadows=1};
        ui_theme_configure(&a);ui_theme_use_dark(dark);ui_theme_set_scale(1.15f);
        for(int i=0;i<OSCFG_ACCENTS;i++){
            const struct oscfg_accent *accent=&oscfg_accents[i];ui_theme_set_accent((struct color){accent->r,accent->g,accent->b,1});
            const struct ui_theme *t=ui_theme();
            CHECK(contrast(t->text,t->bg)>=7.f);
            CHECK(contrast(t->text_secondary,t->surface)>=4.5f);
            CHECK(contrast(t->on_accent,t->accent)>=4.5f);
            CHECK(contrast(t->accent,t->accent_soft)>=4.5f);
            CHECK(contrast(t->text_tertiary,t->bg)>=4.5f);
            CHECK(t->font_regular==11&&t->font_bold==22);
            CHECK(t->material_alpha==1&&t->blur==0&&t->motion_ms==0);
        }
    }
    printf("contour-test: %d checks, %d failures\n",checks,failures);return !!failures;
}
