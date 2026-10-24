/* Contour Settings: searchable navigation, live appearance and bounded preferences. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "embk.h"
#include "appearance.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "system_status.h"

enum { PANE_APPEARANCE = 0, PANE_DESKTOP, PANE_WINDOWS, PANE_ACCESS, PANE_WIFI, PANE_SOUND, PANE_DISPLAY, PANE_POWER, PANE_KEYBOARD, PANE_TIME, PANE_SYSTEM, PANE_ABOUT, PANE_N };

static const char *g_pane_name[PANE_N]={"Appearance","Desktop & Dock","Windows & Workspaces","Accessibility","Wi-Fi","Sound","Displays","Battery & Power","Keyboard","Date & Time","System","About"};
static const int g_pane_icon[PANE_N]={IconPalette,IconGrid,IconWindow,IconUser,IconNetwork,IconVolume,IconDisplay,IconBattery,IconKeyboard,IconClock,IconBolt,IconInfo};
static const char *g_pane_description[PANE_N]={
    "A coordinated workspace, with room for your own taste.","Arrange the command strip, dock and desktop.",
    "Choose how windows look, move and share the display.","Make the desktop easier to read and calmer to use.",
    "Connect and manage your saved networks.","Choose the level for your audio output.",
    "Your screen and its available controls.","Battery readings and energy preferences.",
    "Layout and the shortcuts used across the desktop.","The clock and your local time zone.",
    "Live information about this computer.","EmbLink OS and its system preferences."};
static char g_search[64],g_surface_hex[8],g_accent_hex[8];
static bool g_reset_confirm;
static int   g_pane = PANE_APPEARANCE;
static struct embk_audio_info g_audio;
static struct embk_display_info g_display;
static struct embk_gpu_info g_graphics;
static struct embk_power_info g_power;
static struct embk_policy g_policy;
static int g_audio_rc,g_display_rc,g_power_rc;
static struct embk_wifi_info g_wifi;
static int g_wifi_rc=-19,g_wifi_pending;
static unsigned g_wifi_action;
static char g_wifi_message[160],g_ssid[33],g_password[65];
static uint8_t g_selected[32];
static unsigned g_selected_len;
static uint64_t g_sample_next;
static float g_scroll = 0;
static struct oscfg g_cfg;
static bool  g_loaded = false;
static char  g_saved[64] = "";       /* the quiet confirmation line */

/* live system readings, refreshed on the runtime's 1s tick */
static char g_uptime[32] = "--";
static char g_res[32]    = "--";
static char g_procs[32]  = "--";
static char g_local[48]  = "--";     /* the clock, in the chosen zone */
static char g_utc[48]    = "--";     /* and the clock the kernel actually keeps */

static void apply_now(void) {
    oscfg_clamp(&g_cfg);oscfg_apply_appearance(&g_cfg);
    em_structure_changed();em_request_frame();
}

/* Every setter goes through here: change, apply, persist. Splitting those
 * three would eventually let one of them be forgotten. */
static void commit(void) {
    apply_now();
    if (oscfg_save(&g_cfg) == 0) snprintf(g_saved, sizeof g_saved, "Saved");
    else snprintf(g_saved, sizeof g_saved, "Could not write %s", oscfg_path());
}

static void sample_system(void) {
    uint64_t ms = embk_uptime_ms();
    if(ms>=g_sample_next) {
        g_sample_next=ms+1000;
        g_wifi_rc=embk_wifi_get(&g_wifi);
        g_audio_rc=embk_audio_get(&g_audio);g_display_rc=embk_display_get(&g_display);
        g_power_rc=embk_power_get(&g_power);embk_gpu_get(&g_graphics);embk_policy_get(&g_policy);
        if(!g_wifi_rc && g_wifi_pending && g_wifi.completed_id &&
           ((g_wifi.completed_id-(unsigned)g_wifi_pending)&0x7fffffffu)<0x40000000u) {
            const char *message="Connection preferences changed.";
            if(g_wifi.completed_id==(unsigned)g_wifi_pending) {
                if(g_wifi.request_error) message=system_request_error(g_wifi.request_error);
                else if(g_wifi_action==EMBK_WIFI_SCAN) message="";
                else if(g_wifi_action==EMBK_WIFI_DISCONNECT) message="Disconnected.";
                else if(g_wifi_action==EMBK_WIFI_FORGET) message="Network forgotten.";
                else message="Network saved. Connecting…";
            }
            snprintf(g_wifi_message,sizeof g_wifi_message,"%s",message);
            g_wifi_pending=0;
        }
    }
    unsigned long s = (unsigned long)(ms / 1000);
    if (s >= 3600) snprintf(g_uptime, sizeof g_uptime, "%lu h %lu min", s / 3600, (s % 3600) / 60);
    else           snprintf(g_uptime, sizeof g_uptime, "%lu min %lu s", s / 60, s % 60);

    uint32_t w = 0, h = 0;
    embk_screen_size(&w, &h);
    snprintf(g_res, sizeof g_res, "%u x %u", (unsigned)w, (unsigned)h);

    struct embk_proc_info p[64];
    int n = embk_proc_list(p, 64);
    snprintf(g_procs, sizeof g_procs, "%d", n < 0 ? 0 : n);

    /* BOTH CLOCKS, SIDE BY SIDE, because the difference between them IS the
     * setting. A preview that showed only local time would look identical
     * whether the zone had been applied or silently ignored. */
    time_t now = time(NULL);
    struct tm lt, gt;
    if (localtime_r(&now, &lt))
        strftime(g_local, sizeof g_local, "%a %d %b  %H:%M:%S  %Z", &lt);
    if (gmtime_r(&now, &gt))
        strftime(g_utc, sizeof g_utc, "%a %d %b  %H:%M:%S", &gt);
}

/* ---- shared row shapes ------------------------------------------------- */

/* A settings row is a NAME, an explanation of what it does, and then the
 * control -- in that order, with the control trailing so a column of rows
 * shares one right edge and the eye can run straight down it. The explanation
 * is not optional decoration: a switch labelled only "Running indicator" makes
 * the reader guess, and guessing is what settings windows are infamous for. */
static void setting_label(const char *title, const char *why) {
    VStack(.spacing = 0, .grow = 1, .align = Fill) {
        Text(title).caption();
        if (why && why[0]) Text(why).caption().tertiary();
    }
}

/* Bind controls and flush before reading staged DSL values. */
static void choice(const char *title,const char *detail,const char *const *labels,int n,int *value) {
    HStack(.spacing=16,.align=Center,.py=4,.grow=1,.key=title) {
        setting_label(title,detail);
        int pick=*value;Segmented(labels,n,&pick);Sync();
        if(pick!=*value) {
            *value=pick;
            uintptr_t target=(uintptr_t)value,begin=(uintptr_t)&g_cfg;
            if(target>=begin && target<begin+sizeof g_cfg)commit();
        }
    }
}
static void switch_row(const char *title,const char *detail,int *value) {
    HStack(.spacing=16,.align=Center,.py=4,.grow=1,.key=title) {
        setting_label(title,detail);bool on=!!*value;Toggle("",&on);Sync();
        if(on!=!!*value){*value=on;commit();}
    }
}
static int rgb_parse(const char *s,int *value) {
    if(strlen(s)!=6)return 0;
    for(int i=0;i<6;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='A'&&s[i]<='F')||(s[i]>='a'&&s[i]<='f')))return 0;
    *value=(int)strtoul(s,0,16);return 1;
}
static void colour_editor(const char *label,char *buffer,int *value,int *enabled) {
    HStack(.spacing=12,.align=Center,.key=label) {
        setting_label(label,"Six hexadecimal digits, without #.");
        TextField(buffer,8,"RRGGBB").width(112);
        if(Button("Use colour").secondary().clicked()) {
            int rgb;
            if(rgb_parse(buffer,&rgb)){*value=rgb;if(enabled)*enabled=1;commit();}
            else snprintf(g_saved,sizeof g_saved,"Enter six hexadecimal digits");
        }
    }
}
static void apply_preset(int preset) {
    oscfg_preset(&g_cfg,preset);
    snprintf(g_surface_hex,sizeof g_surface_hex,"%06X",g_cfg.custom_surface);
    snprintf(g_accent_hex,sizeof g_accent_hex,"%06X",g_cfg.custom_accent);
    commit();
}
static void appearance_preview(void) {
    const struct ui_theme *t=ui_theme();
    VStack(.spacing=EmZero,.background=t->surface,.border=1,.border_color=t->border,.corner=8,.align=Fill) {
        HStack(.background=t->chrome,.px=14,.py=8,.spacing=8) {
            Icon(IconWindow).color(t->accent);Text("Your workspace").caption();Spacer();Text("Live preview").caption().secondary();
        }
        Divider();
        HStack(.spacing=18,.padding=16,.align=Center) {
            VStack(.spacing=4,.grow=1,.align=Fill) {Text("A clear place to begin").title();Text("Readable text, quiet surfaces and a considered accent.").caption().secondary();}
            Badge("Selected");
        }
    }
}
static void pane_appearance(void) {
    static const char *modes[]={"Light","Dark"};
    static const char *surfaces[]={"Mineral","Linen","Slate","Custom"};
    static const char *fonts[]={"Sans","Condensed","Serif"};
    static const char *density[]={"Compact","Balanced","Comfortable"};
    static const char *scales[]={"90%","100%","115%","130%"};
    Section("Curated workspaces") {
        HStack(.spacing=8) {
            for(int i=0;i<OSCFG_PRESETS;i++)if(Button(oscfg_preset_names[i]).secondary().clicked()) {
                apply_preset(i);
            }
        }
        Text("A complete starting point. Your sound, time and keyboard choices are kept.").caption().secondary();
        appearance_preview();
    }
    Divider();
    Section("Palette") {
        choice("Appearance","A light or dark foundation.",modes,2,&g_cfg.dark);
        choice("Surfaces","Coordinate every window and temporary panel.",surfaces,4,&g_cfg.surface_style);
        if(g_cfg.surface_style==3)colour_editor("Surface colour",g_surface_hex,&g_cfg.custom_surface,0);
        HStack(.spacing=8,.align=Center,.key="accent-swatches") {
            setting_label("Accent","Coordinated for readable selection, focus and actions.");
            for(int i=0;i<OSCFG_ACCENTS;i++) {
                const struct oscfg_accent *a=&oscfg_accents[i];Color c={a->r,a->g,a->b,1};
                if(Button(i==g_cfg.accent&&!g_cfg.accent_custom?"•":" ").width(25).height(25).px(EmZero).py(3).bg(c).corner(5).clicked()) {
                    g_cfg.accent=i;g_cfg.accent_custom=0;commit();
                }
            }
        }
        colour_editor("Custom accent",g_accent_hex,&g_cfg.custom_accent,&g_cfg.accent_custom);
    }
    Divider();
    Section("Typography & rhythm") {
        choice("Interface font","Real regular and bold faces.",fonts,3,&g_cfg.font);
        int scale=g_cfg.ui_scale>=125?3:g_cfg.ui_scale>=110?2:g_cfg.ui_scale<=95?0:1,was=scale;
        choice("Interface scale","Scale the shared type and control metrics.",scales,4,&scale);
        if(scale!=was){static const int values[]={90,100,115,130};g_cfg.ui_scale=values[scale];commit();}
        choice("Density","Control padding and the space between shared controls.",density,3,&g_cfg.density);
    }
    Divider();
    Section("Materials") {
        static const char *trans[]={"Opaque","Subtle","Translucent"};
        int opacity=g_cfg.transparency==0?0:g_cfg.transparency<=20?1:2,was=opacity;
        choice("Transparency","An opaque fallback is always available.",trans,3,&opacity);
        if(opacity!=was){g_cfg.transparency=opacity==0?0:opacity==1?18:35;commit();}
        static const char *blur[]={"Off","6 px","12 px"};int step=g_cfg.blur==0?0:g_cfg.blur<=6?1:2,old=step;
        choice("Backdrop blur","Bounded to temporary panels; no full-screen blur.",blur,3,&step);
        if(step!=old){g_cfg.blur=step==0?0:step==1?6:12;commit();}
    }
    Divider();
    if(Button("Reset appearance…").secondary().clicked())g_reset_confirm=true;
}

static void pane_desktop(void) {
    static const char *edges[]={"Bottom","Left","Right"};
    static const char *sizes[]={"Small","Medium","Large"};
    static const char *bar_edges[]={"Top","Bottom"};
    static const char *bar_sizes[]={"Compact","Balanced","Tall"};
    static const char *order[]={"Status → Clock","Clock → Status"};
    static const char *icons[]={"Colour","Monochrome"};
    Section("Dock") {
        choice("Placement","Reserve one edge for your applications.",edges,3,&g_cfg.dock_edge);
        int size=g_cfg.dock_size<36?0:g_cfg.dock_size>46?2:1,was=size;
        choice("Icon size","Stable targets; no magnification under the pointer.",sizes,3,&size);
        if(size!=was){g_cfg.dock_size=size==0?30:size==1?38:52;commit();}
        switch_row("Auto-hide","Reveal when the pointer enters the reserved dock band.",&g_cfg.dock_autohide);
        switch_row("Running indicators","A short accent rail marks open applications.",&g_cfg.dock_dots);
        switch_row("Application labels","Show the name beside the hovered icon.",&g_cfg.dock_labels);
    }
    Divider();
    Section("Command strip") {
        switch_row("Auto-hide strip","Reveal at its screen edge; menus stay visible while open.",&g_cfg.bar_autohide);
        choice("Screen edge","Menus open toward the display's interior.",bar_edges,2,&g_cfg.bar_edge);
        int size=g_cfg.bar_size<=30?0:g_cfg.bar_size>=40?2:1,was=size;
        choice("Strip height","Compact controls, with a readable material behind them.",bar_sizes,3,&size);
        if(size!=was){g_cfg.bar_size=size==0?28:size==1?34:42;commit();}
        choice("Status order","Choose which group sits at the outer edge.",order,2,&g_cfg.bar_order);
        switch_row("Workspaces","Switch spaces and move the current window.",&g_cfg.bar_workspaces);
        switch_row("Network","Show the network menu.",&g_cfg.bar_network);
        switch_row("Sound","Show output controls.",&g_cfg.bar_sound);
        switch_row("Power","Show battery availability and the lock command.",&g_cfg.bar_power);
        switch_row("Processor load","Measured use of one CPU core; hidden on narrow screens.",&g_cfg.bar_cpu);
        switch_row("24-hour time","Use 13:00 rather than 1 pm.",&g_cfg.bar_24h);
        switch_row("Show date","Include the current day and month.",&g_cfg.bar_date);
        switch_row("Show seconds","Update the clock once a second.",&g_cfg.bar_seconds);
    }
    Divider();
    Section("Desktop") {
        choice("Application icons","One original family, in colour or a single ink.",icons,2,&g_cfg.icon_theme);
        switch_row("Desktop icons","Keep desktop items visible without changing their layout.",&g_cfg.desktop_icons);
        int size=g_cfg.desktop_icon_size<=36?0:g_cfg.desktop_icon_size>=60?2:1,was=size;
        choice("Desktop icon size","Independent of the dock.",sizes,3,&size);
        if(size!=was){g_cfg.desktop_icon_size=size==0?32:size==1?48:64;commit();}
        const char *labels[OSCFG_WALLPAPERS];for(int i=0;i<OSCFG_WALLPAPERS;i++)labels[i]=oscfg_wallpapers[i].label;
        Text("Desktop background").caption();
        Dropdown(labels,OSCFG_WALLPAPERS,&g_cfg.wallpaper);Sync();
        static int previous=-1;
        if(previous!=g_cfg.wallpaper){if(previous>=0)commit();previous=g_cfg.wallpaper;}
        const struct oscfg_wallpaper *wp=&oscfg_wallpapers[g_cfg.wallpaper];
        if(wp->path)Image(wp->path,.height=120,.corner=6);
    }
}
static void pane_windows(void) {
    static const char *decorations[]={"Contour","Classic","Outline"};
    static const char *radii[]={"Square","Subtle","Rounded"};
    static const char *shadows[]={"None","Soft","Defined"};
    static const char *counts[]={"1","2","3","4","5","6"};
    Section("Window frame") {
        choice("Controls","A coherent frame across native applications.",decorations,3,&g_cfg.decoration);
        int radius=g_cfg.window_radius==0?0:g_cfg.window_radius<=8?1:2,was=radius;
        choice("Corners","Shape applied by the toolkit and compositor.",radii,3,&radius);
        if(radius!=was){g_cfg.window_radius=radius==0?0:radius==1?8:12;commit();}
        choice("Window shadows","Keep overlapping windows distinct.",shadows,3,&g_cfg.shadows);
        switch_row("Edge snapping","Snap a dragged window at work-area edges.",&g_cfg.snap);
    }
    Divider();
    Section("Workspaces") {
        int count=g_cfg.workspace_count-1,was=count;
        choice("Number of spaces","Keep separate groups of open windows.",counts,6,&count);
        if(count!=was){g_cfg.workspace_count=count+1;commit();}
        switch_row("Wrap workspace navigation","Cycle from the last space to the first with keyboard navigation.",&g_cfg.workspace_wrap);
        Text("The command strip switches spaces and moves the current window. Reducing the count gathers windows into the last remaining space.").caption().secondary();
    }
    Divider();
    Section("Motion") {switch_row("Reduce motion","Show windows and menus without animated transitions.",&g_cfg.reduce_motion);}
}
static void pane_access(void) {
    static const char *contrast[]={"Standard","Increased","High"};
    static const char *cursors[]={"Light","Dark","Accent"};
    static const char *cursor_sizes[]={"Standard","Large"};
    static const char *text_sizes[]={"100%","115%","130%"};
    Section("Readability") {
        choice("Contrast","Strengthen text and separators without changing the accent.",contrast,3,&g_cfg.contrast);
        int size=g_cfg.text_scale>=125?2:g_cfg.text_scale>=110?1:0,was=size;
        choice("Text size","Increase the shared type scale independently.",text_sizes,3,&size);
        if(size!=was){g_cfg.text_scale=size==0?100:size==1?115:130;commit();}
        choice("Pointer colour","A contrasting outline keeps the pointer visible.",cursors,3,&g_cfg.cursor_style);
        choice("Pointer size","Same hotspot, larger rendered silhouette.",cursor_sizes,2,&g_cfg.cursor_size);
    }
    Divider();
    Section("Calmer effects") {
        switch_row("Reduce motion","Disable menu and window animations.",&g_cfg.reduce_motion);
        if(Button("Use opaque surfaces").secondary().clicked()){g_cfg.transparency=0;g_cfg.blur=0;commit();}
    }
    Divider();
    Section("Keyboard navigation") {
        static const char *modifiers[]={"Super","Ctrl + Alt"};
        choice("System modifier","Choose the modifier used for system shortcuts.",modifiers,2,&g_cfg.shortcut_modifier);
        Text("Tab moves between text fields; Super+A selects text. The system modifier with Tab switches windows, W closes and Q quits. Modifier+Space opens Applications; modifier+1…6 selects a workspace, Shift moves the current window there, and [ / ] walks spaces. Editor document commands use Ctrl+S, Ctrl+O and Ctrl+N.").caption().secondary();
    }
}

/* THE KEYBOARD. The one preference on this machine that decides whether its
 * owner can write their own language: the driver shipped for a long time with
 * no AZERTY at all, because a 7-bit character stream cannot spell é è ç à ù.
 * It can now, so this is a list you pick from rather than a gap explained in a
 * comment.
 *
 * Applied through the KERNEL (embk_kbd_layout) as well as saved, because the
 * layout is the driver's policy -- writing the file alone would change what
 * the machine remembers and not what it types. */
static void wifi_action(unsigned action,const uint8_t *ssid,unsigned n) {
    struct embk_wifi_request r={.version=EMBK_WIFI_VERSION,.action=action,.ssid_len=n};
    if(n) memcpy(r.ssid,ssid,n);
    if(action==EMBK_WIFI_CONNECT) {r.secret_len=strlen(g_password);memcpy(r.secret,g_password,r.secret_len);}
    int rc=embk_wifi_change(&r);
    volatile unsigned char *p=(unsigned char *)&r;for(unsigned i=0;i<sizeof r;i++) p[i]=0;
    p=(unsigned char *)g_password;for(unsigned i=0;i<sizeof g_password;i++) p[i]=0;
    if(rc>0) {g_wifi_pending=rc;g_wifi_action=action;snprintf(g_wifi_message,sizeof g_wifi_message,"Updating connection preferences…");}
    else snprintf(g_wifi_message,sizeof g_wifi_message,"%s",system_request_error(rc));
    g_sample_next=0;
}
static void ipv4_text(char out[20],uint32_t ip) {
    snprintf(out,20,"%u.%u.%u.%u",ip>>24,(ip>>16)&255,(ip>>8)&255,ip&255);
}
static void pane_wifi(void) {
    Section("Connection",.key="wifi-connection") {
        Text(g_wifi_rc?"Network status is unavailable.":system_wifi_status(&g_wifi)).body();
        if(g_wifi.ssid_len) {
            char name[129];system_ssid(name,g_wifi.ssid,g_wifi.ssid_len);
            ListRow(IconCloud,"Network",name);
            em_flush();
        }
        if(g_wifi.flags&EMBK_WIFI_ADDRESS) {
            char ip[20],dns[20];ipv4_text(ip,g_wifi.ip);ipv4_text(dns,g_wifi.dns);
            ListRow(IconInfo,"IP address",ip);ListRow(IconInfo,"DNS",dns);em_flush();
        }
        if(g_wifi_message[0]) Text(g_wifi_message).caption().secondary();
        if(!g_wifi_rc && (g_wifi.flags&EMBK_WIFI_READY) && !g_wifi_pending && !(g_wifi.flags&EMBK_WIFI_BUSY)) {
            if(g_wifi.state || (g_wifi.flags&EMBK_WIFI_AUTOCONNECT)) {
                if(Button("Disconnect").ghost().clicked()) wifi_action(EMBK_WIFI_DISCONNECT,NULL,0);
            } else if(g_wifi.saved_count && Button("Reconnect").clicked()) wifi_action(EMBK_WIFI_RECONNECT,NULL,0);
        }
    }
    if(g_wifi_rc || !(g_wifi.flags&EMBK_WIFI_READY) || (g_wifi.flags&EMBK_WIFI_BLOCKED)) return;
    if(g_wifi_pending || (g_wifi.flags&EMBK_WIFI_BUSY)) return;
    Section("Nearby networks",.key="wifi-nearby") {
        Text("WPA2 Personal networks are supported.").caption().secondary();
        if(!g_wifi.network_count) Text((g_wifi.flags&EMBK_WIFI_SCANNING)?"Scanning…":"No networks from a recent scan.").caption().secondary();
        for(unsigned i=0;i<g_wifi.network_count && i<EMBK_WIFI_NETWORKS;i++) {
            const struct embk_wifi_network *net=&g_wifi.networks[i];char name[129],id[16];
            system_ssid(name,net->ssid,net->ssid_len);snprintf(id,sizeof id,"near%u",i);
            HStack(.spacing=12,.grow=1,.align=Center) {
                if(net->security==EMBK_WIFI_SEC_WPA2) {
                    if(Button(name).id(id).ghost().grow().leading().clicked()) {
                        memcpy(g_selected,net->ssid,net->ssid_len);g_selected_len=net->ssid_len;
                        g_password[0]=0;g_wifi_message[0]=0;
                    }
                } else {Text(name).caption();Spacer();Text("Unsupported security").caption().tertiary();}
            }
        }
        if(!(g_wifi.flags&EMBK_WIFI_SCANNING) && (g_wifi.state==0 || g_wifi.state==6)) {
            if(Button("Scan again").clicked()) wifi_action(EMBK_WIFI_SCAN,NULL,0);
        } else if(g_wifi.state>=2 && g_wifi.state<=5) {
            Text("Disconnect before scanning other channels.").caption().tertiary();
        }
    }
    Section("Join a network",.key="wifi-join") {
        if(g_selected_len) {
            char name[129];system_ssid(name,g_selected,g_selected_len);
            ListRow(IconCloud,"Selected network",name);
            if(Button("Enter another name").ghost().clicked()) {g_selected_len=0;g_password[0]=0;}
        } else TextField(g_ssid,sizeof g_ssid,"Network name").id("wifi-name");
        PasswordField(g_password,sizeof g_password,"Password").id("wifi-password");
        Text("Remember this network and connect automatically after restart.").caption().secondary();
        if(Button("Connect & save").primary().clicked()) {
            unsigned n=g_selected_len?g_selected_len:(unsigned)strlen(g_ssid);
            unsigned pn=strlen(g_password);
            if(!n || pn<8) snprintf(g_wifi_message,sizeof g_wifi_message,"Enter a network name and a password of at least 8 characters.");
            else wifi_action(EMBK_WIFI_CONNECT,g_selected_len?g_selected:(const uint8_t *)g_ssid,n);
        }
    }
    if(g_wifi.saved_count) Section("Saved networks",.key="wifi-saved") {
        for(unsigned i=0;i<g_wifi.saved_count && i<EMBK_WIFI_SAVED;i++) {
            const struct embk_wifi_network *net=&g_wifi.saved[i];char name[129],join[20],forget[20];
            system_ssid(name,net->ssid,net->ssid_len);snprintf(join,sizeof join,"join%u",i);snprintf(forget,sizeof forget,"forget%u",i);
            HStack(.spacing=12,.align=Center,.grow=1) {
                Text(name).caption();Spacer();
                if(Button("Join").id(join).clicked()) wifi_action(EMBK_WIFI_RECONNECT,net->ssid,net->ssid_len);
                if(Button("Forget").id(forget).ghost().clicked()) wifi_action(EMBK_WIFI_FORGET,net->ssid,net->ssid_len);
            }
        }
    }
}
static void pane_sound(void) {
    Section("Output") {
        if(g_audio_rc || !(g_audio.flags&EMBK_AUDIO_AVAILABLE)) {Text("No supported audio output.").body().secondary();}
        else {
            ListRow(IconBell,"Device",g_audio.output);
            static float level=-1;static int dragging;
            if(level<0 || !dragging) level=g_audio.percent/100.f;
            char pct[16];snprintf(pct,sizeof pct,"%u%%",g_audio.percent);
            setting_label("Output volume",pct);
            float previous=level;Slider(&level);em_flush();
            if(level!=previous) {
                unsigned volume=(unsigned)(level*100+0.5f);
                int rc=embk_audio_volume(volume,(g_audio.flags&EMBK_AUDIO_MUTED)!=0);
                if(rc) snprintf(g_saved,sizeof g_saved,"Volume change failed (%d)",rc);
                else {g_audio.percent=volume;g_cfg.volume=volume;dragging=1;}
            } else if(dragging && !ui_pointer_down()) {dragging=0;commit();}
            bool muted=(g_audio.flags&EMBK_AUDIO_MUTED)!=0,was=muted;
            Toggle("Mute",&muted);
            em_flush();
            if(muted!=was) {
                int rc=embk_audio_volume(g_audio.percent,muted);
                if(rc) snprintf(g_saved,sizeof g_saved,"Mute change failed (%d)",rc);
                else {g_cfg.muted=muted;g_cfg.volume=g_audio.percent;commit();g_sample_next=0;}
            }
        }
    }
    Section("Input") {Text("Microphone recording is not available yet.").caption().secondary();}
}
static void pane_display(void) {
    Section("Display") {
        ListRow(IconGrid,"Resolution",g_res);
        ListRow(IconInfo,"Graphics",g_graphics.driver);
        ListRow(IconInfo,"Rendering","Software");
    }
    Section("Brightness") {
        if(g_display.flags&EMBK_DISPLAY_NATIVE)Text("Native panel brightness").caption().secondary();
        if(g_display_rc || !(g_display.flags&EMBK_DISPLAY_BACKLIGHT)) Text("Brightness control is unavailable on this display.").body().secondary();
        else {
            static float level=-1;static int dragging;
            if(level<0 || !dragging) level=g_display.percent/100.f;
            if(g_display.flags&EMBK_DISPLAY_KNOWN) {char pct[16];snprintf(pct,sizeof pct,"%u%%",g_display.percent);Text(pct).caption();em_flush();}
            else Text("Current brightness is unavailable.").caption().secondary();
            if(g_display.flags&EMBK_DISPLAY_BUSY) Text("Adjusting brightness…").caption();
            else {
                float was=level;Slider(&level);em_flush();
                if(was!=level) dragging=1;
                else if(dragging && !ui_pointer_down()) {
                    dragging=0;int rc=embk_display_brightness((unsigned)(level*100+0.5f));
                    if(rc) snprintf(g_saved,sizeof g_saved,"Brightness failed (%d)",rc);
                    g_sample_next=0;
                }
            }
            if(g_display.last_error) Text("The display could not apply the brightness change.").caption();
        }
    }
}
static void save_policy(void) {
    struct embk_policy p={.version=EMBK_POLICY_VERSION,.flags=g_cfg.lid_lock?EMBK_POLICY_LID_LOCK:0,
        .idle_dim_seconds=g_cfg.idle_dim_seconds,.idle_lock_seconds=g_cfg.idle_lock_seconds,.profile=g_cfg.power_profile};
    int rc=embk_policy_set(&p);
    if(rc)snprintf(g_saved,sizeof g_saved,"Power preference failed (%d)",rc);
    else commit();
}
static void pane_power(void) {
    Section("Battery") {
        if(g_power_rc) Text("Power information is unavailable.").body().secondary();
        else if(!(g_power.flags&EMBK_POWER_BATTERY)) Text("No battery reported.").body().secondary();
        else {
            char pct[24];
            if(g_power.percent==EMBK_POWER_UNKNOWN || embk_uptime_ms()>g_power.sampled_ms+10000) snprintf(pct,sizeof pct,"Unavailable");
            else snprintf(pct,sizeof pct,"%u%%",g_power.percent);
            ListRow(IconBolt,"Charge",pct);
            const char *state="Unavailable";
            if((g_power.flags&(EMBK_POWER_CHARGING|EMBK_POWER_DISCHARGING))==(EMBK_POWER_CHARGING|EMBK_POWER_DISCHARGING)) state="Mixed battery states";
            else if(g_power.flags&EMBK_POWER_CHARGING) state="Charging";
            else if(g_power.flags&EMBK_POWER_DISCHARGING) state="On battery";
            else if((g_power.flags&(EMBK_POWER_AC_KNOWN|EMBK_POWER_AC_ONLINE))==(EMBK_POWER_AC_KNOWN|EMBK_POWER_AC_ONLINE)) state="Connected to power";
            if(embk_uptime_ms()>g_power.sampled_ms+10000) state="Unavailable";
            ListRow(IconInfo,"Status",state);
            if((g_power.flags&EMBK_POWER_CRITICAL) || (g_power.percent<=10 && (g_power.flags&EMBK_POWER_DISCHARGING)))
                Text("Battery is low. Connect your charger and save your work.").body();
        }
    }
    Section("Automatic locking") {
        static const char *times[]={"Never","5 min","15 min"};
        static const int seconds[]={0,300,900};
        int selected=g_cfg.idle_lock_seconds==0?0:g_cfg.idle_lock_seconds<=300?1:2,previous=selected;
        Segmented(times,3,&selected);em_flush();
        if(selected!=previous) {g_cfg.idle_lock_seconds=seconds[selected];save_policy();}
        bool lid=g_cfg.lid_lock!=0,old=lid;
        Toggle("Lock when the lid closes",&lid);em_flush();
        if(lid!=old){g_cfg.lid_lock=lid;save_policy();}
        bool dim=g_cfg.idle_dim_seconds!=0,was=dim;
        Toggle("Dim brightness after two minutes idle",&dim);em_flush();
        if(dim!=was){g_cfg.idle_dim_seconds=dim?120:0;save_policy();}
        int session_status=embk_session(EMBK_SESSION_STATUS,0);
        if(session_status>=0 && (session_status&EMBK_SESSION_LIVE))
            Text("Live sessions have no password lock.").caption().secondary();
    }
    Section("Processor energy") {
        if(g_policy.flags&EMBK_POLICY_HWP) {
            static const char *profiles[]={"Balanced","Save energy","Performance"};
            int profile=g_cfg.power_profile,old=profile;Segmented(profiles,3,&profile);em_flush();
            if(profile!=old){g_cfg.power_profile=profile;save_policy();}
        } else Text("Processor energy profiles are unavailable on this CPU.").caption().secondary();
    }
    Section("Power") {
        if(g_power.flags&EMBK_POWER_LID_KNOWN) ListRow(IconInfo,"Lid",(g_power.flags&EMBK_POWER_LID_CLOSED)?"Closed":"Open");
        Text("Sleep and automatic sleep when closing the lid are not available yet.").caption().secondary();
        Text("Save your work before restarting or shutting down.").caption().secondary();
        if(g_power.flags&EMBK_POWER_BUSY) Text("Preparing to shut down…").body();
        else HStack(.spacing=12) {
            if(Button("Restart").clicked()) {int rc=embk_power_request(EMBK_POWER_REBOOT);if(rc) snprintf(g_saved,sizeof g_saved,"Restart failed (%d)",rc);}
            if(Button("Shut down").clicked()) {int rc=embk_power_request(EMBK_POWER_OFF);if(rc) snprintf(g_saved,sizeof g_saved,"Shutdown failed (%d)",rc);}
        }
        if(g_power.last_error) Text("The last power request failed. Your session is still running.").caption();
    }
}
static void pane_keyboard(void) {
    Section("Layout") {
        const char *names[OSCFG_KEYMAPS];
        for (int i = 0; i < OSCFG_KEYMAPS; i++) names[i] = oscfg_keymaps[i].label;
        int pick = (g_cfg.keymap >= 0 && g_cfg.keymap < OSCFG_KEYMAPS) ? g_cfg.keymap : 0;
        int was  = pick;
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Keyboard layout",
                          "What the keys type. AZERTY reaches é è ç à ù directly, "
                          "^ and ¨ compose (^ then e is ê), and AltGr types @ # { } [ ] | €.");
        }
        Segmented(names, OSCFG_KEYMAPS, &pick);
        Sync();                 /* SAME TRAP, and this one cost a whole day:
                                 * without it `pick` is read a flush early, the
                                 * `if` never fires, and the layout cannot be
                                 * changed from Settings however correct the
                                 * syscall below is. The Theme and Interface
                                 * panes look identical and work only because
                                 * their Segmented sits inside an HStack whose
                                 * closing brace flushes first. */
        if (pick != was) {
            g_cfg.keymap = pick;
            embk_kbd_layout(oscfg_keymaps[pick].name, 0, 0);   /* take effect now */
            commit();                                          /* and next boot */
        }
    }
    Section("Try it") {
        static char sample[64];
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Type here",
                          "Whatever this field shows is what the keyboard really produced.");
        }
        TextField(sample, sizeof sample, "café, €, ça va ?");
    }
}

/* DATE & TIME. The kernel keeps UTC and will keep keeping it -- every inode
 * and every log line is stamped in it, which is the only way two machines can
 * compare notes. This pane is about the OTHER clock: the one a person reads.
 *
 * A DROPDOWN, NOT A SEGMENTED CONTROL. Twenty labels in a row of buttons is
 * twenty labels nobody can read; this is the one preference in this window
 * with more choices than fit across it.
 *
 * The zone is applied HERE as well as saved, for the same reason the keyboard
 * layout is: writing the file alone would change what the machine remembers
 * and not what it shows. */
static void pane_time(void) {
    int cur = (g_cfg.tz >= 0 && g_cfg.tz < OSCFG_TIMEZONES) ? g_cfg.tz : 0;

    /* WHICH HALF OF THE WORLD, then which city in it. Not a taxonomy for its
     * own sake: a dropdown of all twenty opens past the bottom of the window
     * and the last eight zones cannot be reached at all -- measured, the list
     * ended at Athens. Seven at a time fits.
     *
     * The group starts as the one the current zone is in and then belongs to
     * the user: browsing Asia must not quietly move a machine to Tokyo, so
     * changing the group changes what the list OFFERS and nothing else. The
     * rows underneath always show what is actually in force. */
    static int group = -1;
    if (group < 0) group = oscfg_timezones[cur].group;

    Section("Time zone") {
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Region", "Which clock everything you see shows.");
        }
        Segmented(oscfg_tz_group_names, OSCFG_TZ_GROUPS, &group);
        Sync();                 /* the staged element writes at the FLUSH */
        if (group < 0 || group >= OSCFG_TZ_GROUPS) group = 0;

        const char *names[OSCFG_TIMEZONES];
        int         idx[OSCFG_TIMEZONES];
        int n = 0, sel = -1;
        for (int i = 0; i < OSCFG_TIMEZONES; i++) {
            if (oscfg_timezones[i].group != group) continue;
            if (i == cur) sel = n;
            names[n] = oscfg_timezones[i].label;
            idx[n++] = i;
        }
        if (sel < 0) sel = 0;           /* a group that does not hold the current
                                         * zone offers its first, and commits
                                         * nothing until the user picks. */
        int was = sel;
        Dropdown(names, n, &sel);
        Sync();
        if (sel != was && sel >= 0 && sel < n) {
            g_cfg.tz = idx[sel];
            oscfg_apply_tz(&g_cfg);      /* this window, now */
            commit();                    /* everything else, from its next launch */
        }
    }
    Section("Now") {
        ListRow(IconGear,  "Zone",       oscfg_tz_label(&g_cfg));
        ListRow(IconClock, "Local time", g_local);
        ListRow(IconInfo,  "UTC",        g_utc);
        ListRow(IconList,  "Rule",       oscfg_tz_string(&g_cfg));
    }
    Section("About the change") {
        Text("Open applications keep the zone they started with.")
            .caption().tertiary();
    }
}

static void pane_system(void) {
    Section("This machine") {
        ListRow(IconBolt, "Uptime",        g_uptime);
        ListRow(IconGrid, "Display",       g_res);
        ListRow(IconList, "Live processes", g_procs);
    }
    Section("Software") {
        ListRow(IconInfo, "System",   "EmbLink OS");
        ListRow(IconInfo, "Kernel",   "EmbLink, x86-64");
        ListRow(IconInfo, "Toolkit",  "EmUI");
        ListRow(IconInfo, "Shell",    "EmbLink shell");
    }
}

static void pane_about(void) {
    VStack(.spacing = 8, .align = Leading, .py = 6) {
        Text("EmbLink OS").title();
        Text("An operating system written from the boot sector up: its own kernel, "
             "filesystem, network stack, TLS, package manager, compiler toolchain "
             "and this user interface.").body().secondary();
        Divider();
        { static char where[224];
          snprintf(where, sizeof where,
                   "Preferences are stored in %s as plain text. "
                   "Editing that file by hand is a supported way to change them.",
                   oscfg_path());
          Text(where).caption().tertiary(); }
    }
}

static void app(void) {
    if (!g_loaded) { oscfg_load(&g_cfg); g_loaded = true; apply_now(); }
    sample_system();

    if(!g_surface_hex[0])snprintf(g_surface_hex,sizeof g_surface_hex,"%06X",g_cfg.custom_surface);
    if(!g_accent_hex[0])snprintf(g_accent_hex,sizeof g_accent_hex,"%06X",g_cfg.custom_accent);
    Window("Settings") {
        AppBar("Settings") {if(g_saved[0])Text(g_saved).caption().secondary();}
        Split(em_viewport_width()<760?184:226) {
            SidebarPane() {
                SearchField(g_search,sizeof g_search,"Find a setting");
                Divider();
                for(int i=0;i<PANE_N;i++) {
                    if(g_search[0]) {
                        int match=0;for(const char *p=g_pane_name[i];*p;p++) {
                            const char *a=p,*b=g_search;
                            while(*a&&*b&&((*a|32)==(*b|32))){a++;b++;}if(!*b){match=1;break;}
                        }
                        if(!match)continue;
                    }
                    bool on=g_pane==i;
                    HStack(.spacing=9,.align=Center,.px=9,.py=7,.corner=5,.grow=1,
                           .background=on?T.accent_soft:T.clear,.border=EmZero,.key=g_pane_name[i]) {
                        struct instance_handle row=ui_open();
                        Icon(g_pane_icon[i]).caption().color(on?T.accent:T.secondary);
                        Text(g_pane_name[i]).caption().color(on?T.text:T.secondary);Spacer();
                        if(ui_control_interact(row)){g_pane=i;g_scroll=0;g_saved[0]=0;em_structure_changed();}
                    }
                }
                Spacer();Text("Contour · EmbLink OS").caption().secondary();
            }
            ContentPane(.padding=EmZero) {
                ScrollView(&g_scroll,em_viewport_height()-48) {
                    VStack(.spacing=16,.padding=22,.align=Fill,.key=g_pane_name[g_pane]) {
                        VStack(.spacing=4,.align=Fill) {Text(g_pane_name[g_pane]).title();Text(g_pane_description[g_pane]).caption().secondary();}
                        Divider();
                        switch(g_pane) {
                        case PANE_APPEARANCE:pane_appearance();break;
                        case PANE_DESKTOP:pane_desktop();break;
                        case PANE_WINDOWS:pane_windows();break;
                        case PANE_ACCESS:pane_access();break;
                        case PANE_WIFI:pane_wifi();break;
                        case PANE_SOUND:pane_sound();break;
                        case PANE_DISPLAY:pane_display();break;
                        case PANE_POWER:pane_power();break;
                        case PANE_KEYBOARD:pane_keyboard();break;
                        case PANE_TIME:pane_time();break;
                        case PANE_SYSTEM:pane_system();break;
                        default:pane_about();break;
                        }
                    }
                }
            }
        }
        if(g_reset_confirm) {
          Overlay() {
            Dialog(.width=400,.padding=22,.spacing=14) {
                Text("Reset appearance?").title();
                Text("Restore the Contour palette, layout, typography and effects. Other system preferences are kept.").body().secondary();
                HStack(.spacing=10) {
                    Spacer();if(Button("Cancel").secondary().clicked())g_reset_confirm=false;
                    if(Button("Reset").primary().clicked()){apply_preset(0);g_reset_confirm=false;}
                }
            }
          }
          if(OverlayDismissed())g_reset_confirm=false;
        }
    }
}

EM_APPLICATION {
    .title      = "Settings",
    .min_size = { 640, 420 },
    .size       = { 980, 660 },
    .theme      = Dark,
    .chrome     = Chromeless,
    .resize     = Resizable,
    .refresh_ms = 1000,     /* the System pane shows live readings */
    .view       = app,
};
