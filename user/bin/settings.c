/* user/bin/settings.c -- System Settings.
 *
 * The rule this app is built to: every control here CHANGES SOMETHING. A
 * settings window full of switches that only move themselves is worse than no
 * settings window, because it teaches the user that the OS lies to them. So
 * the panes are short and each one is real:
 *
 *   Appearance      accent + light/dark, applied to THIS window the instant
 *                   you pick it, saved in the user's home, and worn by
 *                   every application at launch (em_app_run reads the same
 *                   file -- see user/lib/oscfg.h).
 *   Desktop & Dock  dock icon size and the running indicator, which the
 *                   desktop re-reads live: the dock changes under you.
 *   System          measured, not typed: uptime, resolution, live process
 *                   count, all read from the kernel each second.
 *   About           what this actually is.
 *
 * There is no "Apply" button. A preference that needs confirming is a
 * preference the program was not confident enough to show you.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "oscfg.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "system_status.h"

enum { PANE_APPEARANCE = 0, PANE_DESKTOP, PANE_WIFI, PANE_SOUND, PANE_DISPLAY, PANE_POWER, PANE_SYSTEM, PANE_ABOUT, PANE_N };

static const char *g_pane_name[PANE_N] = { "Appearance", "Desktop & Dock", "Wi-Fi", "Sound", "Displays", "Battery & Power", "System", "About" };
static const int   g_pane_icon[PANE_N] = { IconStar, IconGrid, IconCloud, IconBell, IconGrid, IconBolt, IconList, IconInfo };
static struct embk_display_info g_display;
static struct embk_gpu_info g_graphics;
static struct embk_audio_info g_audio;
static int g_display_rc,g_audio_rc;

static struct embk_wifi_info g_wifi;
static int g_wifi_rc=-19,g_wifi_pending;
static unsigned g_wifi_action;
static char g_wifi_message[160],g_ssid[33],g_password[65];
static uint8_t g_selected[32];
static unsigned g_selected_len;
static struct embk_policy g_policy;
static struct embk_power_info g_power;
static int g_power_rc=-19;
static uint64_t g_sample_next;

static int   g_pane = PANE_APPEARANCE;
static float g_scroll = 0;
static struct oscfg g_cfg;
static bool  g_loaded = false;
static char  g_saved[64] = "";       /* the quiet confirmation line */

/* live system readings, refreshed on the runtime's 1s tick */
static char g_uptime[32] = "--";
static char g_res[32]    = "--";
static char g_procs[32]  = "--";

static void apply_now(void) {
    ui_theme_use_dark(g_cfg.dark != 0);
    ui_theme_set_scale((float)g_cfg.ui_scale / 100.0f);
    const struct oscfg_accent *a = &oscfg_accents[g_cfg.accent];
    ui_theme_set_accent((struct color){ a->r, a->g, a->b, 1.0f });
    em_request_frame();
}

/* Every setter goes through here: change, apply, persist. Splitting those
 * three would eventually let one of them be forgotten. */
static void commit(void) {
    struct embk_audio_info audio;
    if(!embk_audio_get(&audio) && (audio.flags&EMBK_AUDIO_AVAILABLE)) {
        g_cfg.volume=audio.percent;g_cfg.muted=(audio.flags&EMBK_AUDIO_MUTED)!=0;
    }
    apply_now();
    if (oscfg_save(&g_cfg) == 0) snprintf(g_saved, sizeof g_saved, "Saved");
    else snprintf(g_saved, sizeof g_saved, "Could not save preferences");
}

static void sample_system(void) {
    uint64_t ms = embk_uptime_ms();
    if(ms>=g_sample_next) {
        g_sample_next=ms+1000;
        g_wifi_rc=embk_wifi_get(&g_wifi);
        g_power_rc=embk_power_get(&g_power);
        embk_policy_get(&g_policy);
        g_display_rc=embk_display_get(&g_display);g_audio_rc=embk_audio_get(&g_audio);embk_gpu_get(&g_graphics);
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
}

/* ---- shared row shapes ------------------------------------------------- */

/* A settings row is a NAME, an explanation of what it does, and then the
 * control -- in that order, with the control trailing so a column of rows
 * shares one right edge and the eye can run straight down it. The explanation
 * is not optional decoration: a switch labelled only "Running indicator" makes
 * the reader guess, and guessing is what settings windows are infamous for. */
static void setting_label(const char *title, const char *why) {
    VStack(.spacing = 0, .grow = 1, .align = Leading) {
        Text(title).caption();
        if (why && why[0]) Text(why).caption().tertiary();
    }
}

static void pane_appearance(void) {
    const struct ui_theme *t = ui_theme();
    static const char *modes[] = { "Light", "Dark" };
    int dark = g_cfg.dark ? 1 : 0;

    Section("Theme") {
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Appearance", "Light or dark ground for every window.");
            Segmented(modes, 2, &dark);
        }
        if (dark != (g_cfg.dark ? 1 : 0)) { g_cfg.dark = dark; commit(); }
    }

    Section("Interface size") {
        /* The base size everything else is derived from -- text, and so every
         * box measured around text, including the Terminal's character grid.
         * Three steps rather than a slider: this is a legibility decision with
         * two or three right answers, not a continuous quantity, and a slider
         * would invite pixel-hunting for a value that does not exist. */
        static const char *sizes[] = { "Small", "Default", "Large" };
        int step = g_cfg.ui_scale <= 90 ? 0 : g_cfg.ui_scale >= 115 ? 2 : 1;
        int was = step;
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Text and controls",
                          "Scales the whole interface, not just this window.");
            Segmented(sizes, 3, &step);
        }
        if (step != was) {
            g_cfg.ui_scale = step == 0 ? 88 : step == 2 ? 118 : 100;
            commit();
        }
    }

    Section("Accent") {
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Highlight colour",
                          "Selection, focus and active controls -- and nothing else.");
        }
        /* The swatches are the control. A dropdown listing colour NAMES would
         * make you imagine the colour; showing them lets you choose one. */
        HStack(.spacing = 8, .align = Center, .py = 2) {
            for (int i = 0; i < OSCFG_ACCENTS; i++) {
                const struct oscfg_accent *a = &oscfg_accents[i];
                bool on = (g_cfg.accent == i);
                VStack(.spacing = 4, .align = Center, .width = 66) {
                    if (Button(on ? "\xE2\x9C\x93" : " ")
                            .bg((Color){ a->r, a->g, a->b, 1.f })
                            .color(on ? (Color){ 1,1,1,1 } : (Color){ a->r, a->g, a->b, 1.f })
                            .frame(40, 28).corner(7)
                            .id(a->name).clicked()) {
                        g_cfg.accent = i; commit();
                    }
                    Text(a->name).caption().color(on ? t->accent : t->text_secondary);
                }
            }
        }
    }
}

static void pane_desktop(void) {
    Section("Dock") {
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Icon size", "How large the apps in the dock are drawn.");
            char v[16]; snprintf(v, sizeof v, "%d px", g_cfg.dock_size);
            Text(v).caption().secondary();
        }
        /* A slider, because size is a quantity you tune by eye, not a value you
         * type. It commits on release rather than per-pixel: writing the file
         * on every frame of a drag would be a hundred writes for one decision. */
        float f = (float)(g_cfg.dock_size - 28) / 32.0f;
        static bool dragging = false;
        Slider(&f);
        em_flush(); /* bindings are applied when the staged view is emitted */
        int want = 28 + (int)(f * 32.0f + 0.5f);
        if (want != g_cfg.dock_size) { g_cfg.dock_size = want; dragging = true; apply_now(); }
        else if (dragging && !ui_pointer_down()) { dragging = false; commit(); }

        bool dots = g_cfg.dock_dots != 0, was = dots;
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Running indicator",
                          "A dot under an app that is open, so the dock tells the truth.");
            Toggle("", &dots);
        }
        if (dots != was) { g_cfg.dock_dots = dots ? 1 : 0; commit(); }
    }
    Section("Desktop") {
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Icon arrangement",
                          "Icons stay wherever you drop them; the desktop menu tidies them.");
            Text("Free").caption().secondary();
        }
    }
}

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
        if(embk_session(EMBK_SESSION_STATUS,0)&EMBK_SESSION_LIVE)
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
        Text("Your appearance and sound preferences are saved for your account.")
            .caption().tertiary();
    }
}

static void app(void) {
    if (!g_loaded) { oscfg_load(&g_cfg); g_loaded = true; apply_now(); }
    sample_system();

    Window("Settings") {
        AppBar("Settings") {
            if (g_saved[0]) Text(g_saved).caption().tertiary();
        }
        Split(206) {
            SidebarPane() {
                const struct ui_theme *t = ui_theme();
                for (int i = 0; i < PANE_N; i++) {
                    bool on = (g_pane == i);
                    HStack(.spacing = 7, .align = Center, .px = 7, .py = 1, .corner = 5,
                           .grow = 1,
                           .background = on ? (Color){ .r=t->accent.r, .g=t->accent.g, .b=t->accent.b, .a=.16f }
                                            : (Color){ 0, 0, 0, 0 }) {
                        Icon(g_pane_icon[i]).caption().color(on ? t->accent : t->text_secondary);
                        if (Button(g_pane_name[i]).ghost().font(Caption).py(1)
                                .color(on ? t->text : t->text_secondary)
                                .grow().leading().clicked()) { g_pane = i; g_scroll = 0; g_saved[0] = 0; }
                    }
                }
                Spacer();
                Text("EmbLink OS").caption().tertiary();
            }
            ContentPane(.padding = 0) {
                /* measured, not guessed -- see the note in files.c */
                ScrollView(&g_scroll, em_viewport_height() - 58.0f) {
                    /* No page heading. The window's title bar already says
                     * "Settings" and the sidebar says which pane -- a third
                     * copy in 26pt type is the Android pattern of restating
                     * the screen name inside the screen. */
                    /* Each pane owns a different widget tree. A stable pane
                     * key prevents retained slider state becoming a button. */
                    VStack(.spacing = 4, .align = Fill, .padding = 16, .key=g_pane_name[g_pane]) {
                        switch (g_pane) {
                            case PANE_APPEARANCE: pane_appearance(); break;
                            case PANE_DESKTOP:    pane_desktop();    break;
                            case PANE_WIFI:       pane_wifi();       break;
                            case PANE_SOUND:      pane_sound();      break;
                            case PANE_DISPLAY:    pane_display();    break;
                            case PANE_POWER:      pane_power();      break;
                            case PANE_SYSTEM:     pane_system();     break;
                            default:              pane_about();      break;
                        }
                    }
                }
            }
        }
    }
}

static EmApp settings_app;
int main(int argc,char **argv) {
    if(argc>1) {
        if(!strcmp(argv[1],"--wifi")) g_pane=PANE_WIFI;
        else if(!strcmp(argv[1],"--sound")) g_pane=PANE_SOUND;
        else if(!strcmp(argv[1],"--display")) g_pane=PANE_DISPLAY;
        else if(!strcmp(argv[1],"--power")) g_pane=PANE_POWER;
    }
    return em_app_run(&settings_app);
}
static EmApp settings_app={
    .title      = "Settings",
    .size       = { 860, 600 },
    .theme      = Dark,
    .chrome     = Chromeless,
    .resize     = Resizable,
    .refresh_ms = 1000,     /* the System pane shows live readings */
    .view       = app,
};
