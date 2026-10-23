/* user/apps/settings/settings.c -- System Settings.
 *
 * The rule this app is built to: every control here CHANGES SOMETHING. A
 * settings window full of switches that only move themselves is worse than no
 * settings window, because it teaches the user that the OS lies to them. So
 * the panes are short and each one is real:
 *
 *   Appearance      accent + light/dark, applied to THIS window the instant
 *                   you pick it, written to /data/settings.conf, and worn by
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
#include <time.h>

#include "embk.h"
#include "oscfg.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "system_status.h"

enum { PANE_APPEARANCE = 0, PANE_DESKTOP, PANE_WIFI, PANE_SOUND, PANE_DISPLAY, PANE_POWER, PANE_KEYBOARD, PANE_TIME, PANE_SYSTEM, PANE_ABOUT, PANE_N };

static const char *g_pane_name[PANE_N] = { "Appearance", "Desktop & Dock", "Wi-Fi", "Sound", "Displays", "Battery & Power", "Keyboard", "Date & Time", "System", "About" };
static const int   g_pane_icon[PANE_N] = { IconStar, IconGrid, IconCloud, IconBell, IconGrid, IconBolt, IconList, IconClock, IconBolt, IconInfo };

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
    ui_theme_use_dark(g_cfg.dark != 0);
    ui_theme_set_scale((float)g_cfg.ui_scale / 100.0f);
    const struct oscfg_accent *a = &oscfg_accents[g_cfg.accent];
    ui_theme_set_accent((struct color){ a->r, a->g, a->b, 1.0f });
    em_request_frame();
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

    /* THE DESKTOP PICTURE. A name and a live preview rather than a name alone:
     * the whole point of a wallpaper is what it looks like, and four words in a
     * row ask you to remember which one "Dusk" was. The preview is the same
     * file the desktop draws, so what is shown here is what you get. */
    Section("Desktop picture") {
        int wpick = (g_cfg.wallpaper >= 0 && g_cfg.wallpaper < OSCFG_WALLPAPERS)
                    ? g_cfg.wallpaper : 0;
        const int NPIC = OSCFG_WALLPAPER_PICTURES;
        const int NCOL = OSCFG_WALLPAPERS - OSCFG_WALLPAPER_PICTURES;

        /* TWO ROWS, ONE VALUE. Eight labels in a single segmented control is
         * eight unreadable labels at this width. Split by what they ARE --
         * photographs, and colours the desktop draws -- and let the row that
         * does not hold the current choice show none selected, which is what
         * `-1` means to the control. */
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Picture", "What the desktop shows behind everything.");
        }
        {
            const char *names[OSCFG_WALLPAPERS];
            for (int i = 0; i < NPIC; i++) names[i] = oscfg_wallpapers[i].label;
            int pick = (wpick < NPIC) ? wpick : -1, was = pick;
            Segmented(names, NPIC, &pick);
            Sync();                 /* the staged element writes pick at the FLUSH */
            if (pick != was && pick >= 0) { g_cfg.wallpaper = pick; commit(); wpick = pick; }
        }
        {
            const char *names[OSCFG_WALLPAPERS];
            for (int i = 0; i < NCOL; i++) names[i] = oscfg_wallpapers[NPIC + i].label;
            int pick = (wpick >= NPIC) ? wpick - NPIC : -1, was = pick;
            Segmented(names, NCOL, &pick);
            Sync();
            if (pick != was && pick >= 0) {
                g_cfg.wallpaper = NPIC + pick; commit(); wpick = NPIC + pick;
            }
        }

        /* The preview is the same thing the desktop draws, by the same rule:
         * the file if there is one, the gradient if there is not. A preview
         * that approximates the result is a preview that can be wrong. */
        {
            const struct oscfg_wallpaper *wp = &oscfg_wallpapers[wpick];
            if (wp->path) {
                Image(wp->path, .height = 132, .corner = 10);
            } else {
                em_flush();
                ui_box_begin(0x5E77B6C0ULL);
                ui_set_paint(em_lgrad3(
                    (Color){ wp->a[0], wp->a[1], wp->a[2], 1.f },
                    (Color){ wp->b[0], wp->b[1], wp->b[2], 1.f },
                    (Color){ wp->c[0], wp->c[1], wp->c[2], 1.f }, wp->angle));
                ui_set_corner_radius(10);
                ui_set_size((struct layout_size){ .mode = SIZE_FLEX, .flex_grow = 1 },
                            (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 132 });
                ui_box_end();
            }
        }
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
        Sync();                 /* the slider writes f when it is EMITTED, not
                                 * when it is called -- read it before the flush
                                 * and the dock size never moves. */
        int want = 28 + (int)(f * 32.0f + 0.5f);
        if (want != g_cfg.dock_size) { g_cfg.dock_size = want; dragging = true; apply_now(); }
        else if (dragging) { dragging = false; commit(); }

        bool dots = g_cfg.dock_dots != 0, was = dots;
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Running indicator",
                          "An app that is open sits in a lit socket, so the dock "
                          "tells the truth about what is running.");
            Toggle("", &dots);
        }
        if (dots != was) { g_cfg.dock_dots = dots ? 1 : 0; commit(); }
    }
    /* THE MENU BAR. Four independent switches rather than a "style" preset:
     * each is a question someone actually has an opinion about, and a preset
     * would invent combinations nobody asked for. Every one of them is read by
     * a DIFFERENT process -- the bar reads the copy the desktop publishes into
     * /run, because its namespace does not name the user's home and should not. */
    Section("Menu bar") {
        bool h24 = g_cfg.bar_24h != 0,     h24_was = h24;
        bool sec = g_cfg.bar_seconds != 0, sec_was = sec;
        bool dat = g_cfg.bar_date != 0,    dat_was = dat;
        bool cpu = g_cfg.bar_cpu != 0,     cpu_was = cpu;

        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("24-hour clock", "13:00 rather than 1 pm.");
            Toggle("", &h24);
        }
        if (h24 != h24_was) { g_cfg.bar_24h = h24 ? 1 : 0; commit(); }

        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Show seconds", "The clock ticks once a second either way.");
            Toggle("", &sec);
        }
        if (sec != sec_was) { g_cfg.bar_seconds = sec ? 1 : 0; commit(); }

        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Show the date", "The day and month beside the time.");
            Toggle("", &dat);
        }
        if (dat != dat_was) { g_cfg.bar_date = dat ? 1 : 0; commit(); }

        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Show processor load",
                          "What fraction of one core the machine is using.");
            Toggle("", &cpu);
        }
        if (cpu != cpu_was) { g_cfg.bar_cpu = cpu ? 1 : 0; commit(); }
    }

    Section("Desktop") {
        HStack(.spacing = 16, .align = Center, .py = 4, .grow = 1) {
            setting_label("Icon arrangement",
                          "Icons stay wherever you drop them; the desktop menu tidies them.");
            Text("Free").caption().secondary();
        }
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
                           .background = on ? (Color){ .r=.24f, .g=.26f, .b=.34f, .a=1.f }
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
                    VStack(.spacing = 4, .align = Fill, .padding = 16) {
                        switch (g_pane) {
                            case PANE_APPEARANCE: pane_appearance(); break;
                            case PANE_DESKTOP:    pane_desktop();    break;
                            case PANE_SOUND:      pane_sound();      break;
                            case PANE_DISPLAY:    pane_display();    break;
                            case PANE_POWER:      pane_power();      break;
                            case PANE_WIFI:       pane_wifi();       break;
                            case PANE_KEYBOARD:   pane_keyboard();   break;
                            case PANE_TIME:       pane_time();       break;
                            case PANE_SYSTEM:     pane_system();     break;
                            default:              pane_about();      break;
                        }
                    }
                }
            }
        }
    }
}

EM_APPLICATION {
    .title      = "Settings",
    .size       = { 860, 600 },
    .theme      = Dark,
    .chrome     = Chromeless,
    .resize     = Resizable,
    .refresh_ms = 1000,     /* the System pane shows live readings */
    .view       = app,
};
