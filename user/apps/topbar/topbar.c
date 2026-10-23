/* Contour command strip. A readable material, compact commands, independent
 * status groups and a real workspace switcher. Shell snapshots never block a frame. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "embk.h"
#include "emnotify.h"   /* About speaks through the notifier */
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "appearance.h"   /* the menu bar wears the user's preferences too */

/* Which machine this is, decided where it is actually known -- at compile
 * time. A binary cannot be wrong about its own architecture. */
#if defined(__aarch64__)
#define ARCH_NAME "aarch64"
#elif defined(__x86_64__)
#define ARCH_NAME "x86-64"
#else
#define ARCH_NAME "unknown"
#endif

#define BAR_W 1024   /* replaced at startup by the real display width */
/* Thin like a real menu bar: the strip is chrome, not a panel. */
#define BAR_H 34

/* status-chip ids (icon codepoints); the user reorders / removes these live */
/* WHAT THE MACHINE IS DOING, and it is measured.
 *
 * What used to trail this bar was four glyphs -- a star, a lightning bolt, a
 * gear and a heart -- that were not connected to anything. They were there
 * because a menu bar has status icons on its right, which is a description of
 * someone else's menu bar, not a reason. (The clock beside them had the same
 * problem once: it read a hard-coded 9:41, Apple's marketing time, until
 * somebody noticed the OS could just say what time it is.)
 *
 * So this bar reports the one thing this OS is in a position to know better
 * than it knows anything else: how much of the machine is being used, right
 * now, by the user's own programs.
 *
 * PER CORE, like every `top` ever written: cpu_ns is time spent EXECUTING, so
 * the delta over a wall-clock second is the fraction of ONE core, and four
 * busy cores read 400%. That is a real number with a real unit, and it needs
 * no idea of how many cores the machine has -- which userspace has no way to
 * ask for anyway, and which is not worth inventing a lie about.
 *
 * KERNEL THREADS ARE EXCLUDED. They are the machine's own overhead, not the
 * session's work, and the idle threads are kernel threads -- counting those
 * would report a quiet machine as a busy one.
 *
 * The one thing it cannot see: time charged to a process that EXITED between
 * two samples leaves the total, which would make the delta negative. The
 * sample is skipped rather than reported as a wild number, so the reading
 * holds its last value for one second after something quits. 64 rows is the
 * kernel's whole table (MAX_PROCESSES), so nothing is missed for being past
 * the end of the buffer. */
static int   g_cpu_pct = 0;
static uint64_t g_cpu_last_ns = 0, g_cpu_last_ms = 0;

static void cpu_sample(void) {
    static struct embk_proc_info ps[64];
    int n = embk_proc_list(ps, 64);
    if (n <= 0) return;

    uint64_t busy = 0;
    for (int i = 0; i < n; i++)
        if (!ps[i].is_kthread) busy += ps[i].cpu_ns;

    uint64_t now = embk_uptime_ms();
    if (g_cpu_last_ms && now > g_cpu_last_ms && busy >= g_cpu_last_ns) {
        uint64_t dns = busy - g_cpu_last_ns;
        uint64_t dms = now - g_cpu_last_ms;
        /* ns per ms = fraction of one core x 1e6; x100 for a percentage. */
        g_cpu_pct = (int)(dns / (dms * 10000ull));
        if (g_cpu_pct > 999) g_cpu_pct = 999;
    }
    g_cpu_last_ns = busy;
    g_cpu_last_ms = now;
}

/* LABELLED, and that is not decoration. A bare percentage in the corner of a
 * menu bar is read as a battery by everyone who has ever used a computer --
 * and this OS has no battery driver at all yet, so the one number it can
 * honestly show there had better say what it is. */
static const char *cpu_text(void) {
    static char buf[24];
    snprintf(buf, sizeof buf, "CPU %d%%", g_cpu_pct);
    return buf;
}

/* A 24px rule that fills with the load. Small enough to read as punctuation
 * next to the number rather than as a gauge competing with it -- the number is
 * the reading; this is the shape of the reading, for the glance that does not
 * stop to read.
 *
 * FULL SCALE IS ONE CORE, not the whole machine. Scaled to four, an ordinary
 * desktop (measured: 45% while a terminal and the shell were up) filled three
 * pixels of twenty-four and the meter was a dot -- technically true and
 * useless at a glance. One core is the threshold that actually means
 * something: full means "something is working hard", and a machine with more
 * cores keeps working past it rather than the meter rescaling underfoot. */
/* THE NETWORK, sampled on a clock like everything else in this bar.
 *
 * The stack has always known this and nothing could ask, so the bar could not
 * say whether the machine was online -- and a machine that cannot tell you it
 * is offline looks broken when it is merely unplugged. Two seconds: a link
 * coming up is not something anyone watches for at frame rate. */
static struct embk_net_status g_net;
static int      g_net_ok;
static uint64_t g_net_next;

static void net_sample(void) {
    uint64_t now = embk_uptime_ms();
    if (now < g_net_next) return;
    g_net_next = now + 2000;
    g_net_ok = (embk_net_status(&g_net) == 0);
}

/* "10.0.2.15", or the empty string when there is no address to show. */
static const char *ip_text(uint32_t ip) {
    static char b[20];
    if (!ip) return "";
    snprintf(b, sizeof b, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xFF,
             (unsigned)(ip >> 16) & 0xFF, (unsigned)(ip >> 8) & 0xFF,
             (unsigned)ip & 0xFF);
    return b;
}

/* Live date + time. The bar used to read a hard-coded "9:41" -- Apple's
 * marketing time -- which is exactly the kind of decorative lie the rest of the
 * shell avoids. The OS can report the real clock, so it does. */
/* The menu bar's own preferences, fetched ON A THREAD OF ITS OWN.
 *
 * It cannot open the user's settings file -- its namespace does not name the
 * home directory, deliberately -- so it asks the desktop over /run. That is a
 * channel round trip, and embk_chan_recv BLOCKS: there is no timeout and no
 * poll on it. Doing that from the render loop, which is where this started,
 * means the bar stops drawing and stops taking clicks for as long as the
 * desktop takes to answer. On x86 it was invisible. On aarch64 the whole menu
 * bar went silent, and the acceptance test said so in one line: "the launcher
 * button did nothing -- the top bar could not reach the desktop through /run".
 *
 * A status bar must never wait for anybody. So the waiting happens here, off
 * the render path, and the loop just reads whatever the last answer was.
 * The worker publishes a complete snapshot under a mutex. Only the render
 * thread reads g_bar_cfg and changes the theme, so a settings update cannot
 * race a frame or leave the dropdown on a different palette from the desktop. */
static struct oscfg g_bar_cfg, g_bar_pending;
static embk_mutex g_bar_cfg_lock = EMBK_MUTEX_INIT;
static void bar_cfg_apply(void) {
    struct oscfg next;
    embk_mutex_lock(&g_bar_cfg_lock);
    next = g_bar_pending;
    embk_mutex_unlock(&g_bar_cfg_lock);
    static int applied;
    int changed=!applied || memcmp(&next,&g_bar_cfg,sizeof next);
    g_bar_cfg=next;
    if(changed){applied=1;oscfg_apply_appearance(&next);em_structure_changed();}

}
static void bar_cfg_thread(long arg) {
    (void)arg;
    for (;;) {
        struct oscfg c;
        oscfg_snapshot(&c);
        embk_mutex_lock(&g_bar_cfg_lock);
        g_bar_pending = c;
        embk_mutex_unlock(&g_bar_cfg_lock);
        embk_sleep_ms(1000);
    }
}


/* THE MONTH, for the menu behind the clock.
 *
 * A date in a menu bar answers "what is today"; the thing people open it for is
 * "what day of the week is the 26th", and that question needs the grid. It is
 * built from localtime and nothing else -- no calendar store, no events -- so
 * it cannot be out of date and cannot be wrong about a month it has never seen.
 *
 * Rendered as text rows rather than a grid of boxes because a menu item is the
 * only thing a dropdown can hold, and a week is exactly one line of it. Today
 * is bracketed, which survives being read at a glance and does not need colour
 * the menu may not have. */
static int days_in_month(int y, int m) {          /* m: 0..11 */
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m != 1) return d[m];
    int leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    return leap ? 29 : 28;
}

/* Fills `out` with up to 6 week-lines, returns how many. */
static int month_grid(const struct tm *tm, char out[6][32]) {
    int mdays = days_in_month(tm->tm_year + 1900, tm->tm_mon);
    /* The weekday the 1st fell on, derived from today rather than computed from
     * scratch: today's weekday is known and correct, and counting back from a
     * known point cannot disagree with the clock. */
    int first = ((tm->tm_wday - ((tm->tm_mday - 1) % 7)) % 7 + 7) % 7;
    int rows = 0, day = 1;
    while (day <= mdays && rows < 6) {
        int o = 0;
        for (int col = 0; col < 7; col++) {
            int cell = (rows == 0 && col < first) ? 0 : (day <= mdays ? day++ : 0);
            if (!cell) { out[rows][o++] = ' '; out[rows][o++] = ' ';
                         out[rows][o++] = ' '; out[rows][o++] = ' '; continue; }
            char l = (cell == tm->tm_mday) ? '[' : ' ';
            char r = (cell == tm->tm_mday) ? ']' : ' ';
            out[rows][o++] = l;
            if (cell < 10) { out[rows][o++] = ' '; out[rows][o++] = (char)('0' + cell); }
            else { out[rows][o++] = (char)('0' + cell / 10);
                   out[rows][o++] = (char)('0' + cell % 10); }
            out[rows][o++] = r;
        }
        out[rows][o] = 0;
        rows++;
    }
    return rows;
}

static const char *bar_clock(void) {
    static char buf[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) { snprintf(buf, sizeof buf, "--:--"); return buf; }

    /* Standard conversions only: newlib's strftime does not implement the GNU
     * "%-d" no-pad flag and stops at it, which silently truncated the whole
     * string to just the weekday. So this is built by hand.
     *
     * FOUR SEPARATE PREFERENCES, not a format string. A person has an opinion
     * about "do I want seconds" and about "do I want the date"; nobody has an
     * opinion about a strftime pattern, and offering one would be handing over
     * a way to make the bar say nothing at all. */
    char date[24] = "";
    if (g_bar_cfg.bar_date)
        snprintf(date, sizeof date, "%s %d %s  ",
                 (const char *[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[tm->tm_wday % 7],
                 tm->tm_mday,
                 (const char *[]){"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"}[tm->tm_mon % 12]);

    int hour = tm->tm_hour;
    const char *suffix = "";
    if (!g_bar_cfg.bar_24h) {
        suffix = hour < 12 ? " am" : " pm";
        hour %= 12;
        if (hour == 0) hour = 12;          /* midnight and noon are 12, not 0 */
    }

    if (g_bar_cfg.bar_seconds)
        snprintf(buf, sizeof buf, "%s%02d:%02d:%02d%s", date, hour, tm->tm_min,
                 tm->tm_sec, suffix);
    else
        snprintf(buf, sizeof buf, "%s%02d:%02d%s", date, hour, tm->tm_min, suffix);
    return buf;
}

/* Ask the desktop to open the Apps launcher. The launcher renders on the desktop
 * (same program as the dock, so apps can be dragged into it); the top bar is a
 * separate process, so it signals by CONNECTING to the desktop's IPC channel in
 * /run (the desktop listens there and opens the grid). The connect itself is the
 * signal -- no payload -- so we close immediately. */
/* LOCK THE SCREEN. Its own endpoint beside the launcher's, and the connect is
 * again the whole message. The bar cannot draw the lock itself -- it is an
 * ordinary window and an ordinary window can be lowered -- so it asks the
 * desktop, which owns the layer that goes in front of everything. */
static void open_notifications(void) {
    static int child=-1;
    if(child>=0 && embk_proc_alive(child)){embk_win_restore(child);return;}
    if(child>=0)embk_wait(child);
    struct embk_spawn_file_action action;
    embk_action_set_caps(&action,EMBK_CAP_BIT(EMBK_CAP_FILESYSTEM)|EMBK_CAP_BIT(EMBK_CAP_GPU));
    char *args[]={"/data/apps/notifications/notifications.elf",NULL};
    child=embk_spawn(args[0],args,&action,1);
}
static void request_lock(void) {
    embk_puts(1, "TopBar: lock screen requested\n");
    int ch = (int)embk_chan_connect("/run/emlink.lock");
    if (ch >= 0) { embk_chan_close(ch); return; }
    char b[96];
    snprintf(b, sizeof b, "TopBar: could not reach the desktop to lock the screen (%d)\n", ch);
    embk_puts(1, b);
}

static void request_apps(void) {
    embk_puts(1, "TopBar: launcher button pressed\n");
    int ch = (int)embk_chan_connect("/run/emlink.desktop");
    if (ch >= 0) {
        embk_chan_close(ch);
        return;
    }
    /* SAY SO. This used to fail silently, and a launcher button that does
     * nothing gives the user no way to tell a missing desktop listener from a
     * missing /run from a dead click -- aarch64 had the second for its whole
     * life and nothing on screen or in the log said which. */
    char b[96];
    snprintf(b, sizeof b, "TopBar: could not reach the desktop to open the launcher (%d)\n", ch);
    embk_puts(1, b);
}

/* THE WINDOWS THAT EXIST, sampled on the bar's own beat.
 *
 * The bar could not name the focused application before -- nothing could. A
 * process in this system is named by handle and stores no path, so the only
 * human-readable name an application has is the title of its window, and until
 * embk_win_list there was no way to ask the compositor for it. In its place
 * the bar carried three menus (File, Edit, View) that belonged to no app and
 * did nothing, because a menu bar is SUPPOSED to say whose menus these are and
 * this one had no way to find out.
 *
 * Now it says the truth: the focused application's name, and a menu of
 * everything open that switches to it. */
#define WINS_MAX 32
static struct embk_win_info g_wins[WINS_MAX];
static int      g_win_n;
static char     g_focus_name[32];
static uint64_t g_wins_next;

static void wins_sample(void) {
    uint64_t now = em_now_ms();
    if (now < g_wins_next) return;
    g_wins_next = now + 700;               /* windows do not open at frame rate */

    int n = embk_win_list(g_wins, WINS_MAX);
    g_win_n = n < 0 ? 0 : n;
    g_focus_name[0] = 0;
    for (int i = 0; i < g_win_n; i++)
        if (g_wins[i].flags & EMBK_WIN_FOCUSED) {
            snprintf(g_focus_name, sizeof g_focus_name, "%s", g_wins[i].title);
            break;
        }
}

static struct embk_power_info g_power;
static struct embk_audio_info g_audio;
static struct embk_desktop g_space;

static void system_menu(void) {
    Menu("EmbLink",.font=BodyBold,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0) {
        if(MenuItem("About EmbLink"))embk_notify("EmbLink OS","Contour desktop · a workspace of your own.");
        MenuSeparator();
        if(MenuItem("Applications"))request_apps();
        if(MenuItem("Notifications"))open_notifications();
        MenuSeparator();
        if(MenuItem("Lock Screen"))request_lock();
        if(MenuItem("Log Out"))embk_session_end(0);
        MenuSeparator();
        if(MenuItem("Restart"))embk_power_restart();
        if(MenuItem("Shut Down"))embk_power_off();
    }
}
static void workspace_menu(void) {
    static char label[24];snprintf(label,sizeof label,"Space %u",g_space.workspace+1);
    Menu(label,.font=Caption,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0,.width=224) {
        for(unsigned i=0;i<g_space.workspace_count;i++) {
            char text[32];snprintf(text,sizeof text,"%sWorkspace %u",i==g_space.workspace?"✓  ":"    ",i+1);
            if(MenuItem(text)) {struct embk_desktop d={.version=1,.workspace=i};embk_desktop_control(EMBK_DESKTOP_SWITCH,&d);}
        }
        MenuSeparator();
        for(unsigned i=0;i<g_space.workspace_count;i++)if(i!=g_space.workspace) {
            char text[40];snprintf(text,sizeof text,"Move current window to %u",i+1);
            if(MenuItem(text)){struct embk_desktop d={.version=1,.workspace=i};embk_desktop_control(EMBK_DESKTOP_MOVE_FRONT,&d);}
        }
    }
}
static void status_network(void) {
    Menu("Network",.icon=IconNetwork,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0,.width=242) {
        if(!g_net_ok)MenuItem("Network unavailable");
        else if(!g_net.up)MenuItem("Not connected");
        else {
            char line[60];snprintf(line,sizeof line,"Address   %s",ip_text(g_net.ip));MenuItem(line);
            snprintf(line,sizeof line,"Router   %s",ip_text(g_net.gateway));MenuItem(line);
            snprintf(line,sizeof line,"DNS   %s",ip_text(g_net.dns));MenuItem(line);
            MenuSeparator();MenuItem(g_net.dhcp?"Automatic address (DHCP)":"Manual address");
        }
    }
}
static void status_power(void) {
    Menu("Power",.icon=IconBattery,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0,.width=224) {
        char status[64];
        if((g_power.flags&EMBK_POWER_BATTERY) && g_power.percent<=100)snprintf(status,sizeof status,"Battery  %u%%",g_power.percent);
        else snprintf(status,sizeof status,"No battery reported");
        MenuItem(status);MenuSeparator();
        if(MenuItem("Lock Screen"))request_lock();
    }
}
static void status_sound(void) {
    Menu("Sound",.icon=IconVolume,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0) {
        if(!(g_audio.flags&EMBK_AUDIO_AVAILABLE)){Text("No audio output available").caption().secondary().padding(10);}
        else {
        char label[40];snprintf(label,sizeof label,"Output volume   %u%%",g_audio.percent);MenuItem(label);
        MenuSeparator();
        if(MenuItem((g_audio.flags&EMBK_AUDIO_MUTED)?"Unmute output":"Mute output"))embk_audio_volume(g_audio.percent,!(g_audio.flags&EMBK_AUDIO_MUTED));
        if(MenuItem("Volume up"))embk_audio_volume(g_audio.percent>90?100:g_audio.percent+10,!!(g_audio.flags&EMBK_AUDIO_MUTED));
        if(MenuItem("Volume down"))embk_audio_volume(g_audio.percent<10?0:g_audio.percent-10,!!(g_audio.flags&EMBK_AUDIO_MUTED));
        }
    }
}
static void clock_menu(void) {
    Menu(bar_clock(),.font=Caption,.width=268,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0) {
        time_t now=time(NULL);struct tm *tm=localtime(&now);
        if(tm) {
            char heading[48];strftime(heading,sizeof heading,"%B %Y",tm);MenuItem(heading);MenuSeparator();
            MenuItem(" Su  Mo  Tu  We  Th  Fr  Sa");
            char weeks[6][32];int n=month_grid(tm,weeks);CodeSurface(){for(int i=0;i<n;i++)Text(weeks[i]).caption().px(12).py(3);}
        } else MenuItem("Clock unavailable");
    }
}
static void bar(void) {
    bar_cfg_apply();cpu_sample();net_sample();wins_sample();
    g_space.version=1;embk_desktop_control(EMBK_DESKTOP_GET,&g_space);
    embk_power_get(&g_power);embk_audio_get(&g_audio);
    em_shell_band(g_bar_cfg.bar_edge,g_bar_cfg.bar_size);

    static uint64_t reveal_until;float px,py;ui_pointer_pos(&px,&py);
    uint64_t now=em_now_ms();
    float strip_y=g_bar_cfg.bar_edge?em_viewport_height()-g_bar_cfg.bar_size:0;
    if(py>=strip_y && py<strip_y+g_bar_cfg.bar_size)reveal_until=now+900;
    int hidden=g_bar_cfg.bar_autohide && !em_menu_any_open() && now>reveal_until;
    if(!em_menu_any_open()) {
        uint32_t sw,sh;embk_screen_size(&sw,&sh);
        int canvas=(int)em_viewport_height();
        em_window_move_to(0,g_bar_cfg.bar_edge?(int)sh-canvas+(hidden?g_bar_cfg.bar_size-2:0):(hidden?2-g_bar_cfg.bar_size:0));

    }
    VStack(.width=em_viewport_width(),.height=em_viewport_height(),.align=Fill,.spacing=EmZero) {
        if(g_bar_cfg.bar_edge)Spacer();
        HStack(.height=g_bar_cfg.bar_size,.align=Center,.spacing=EmZero,.px=8,
               .background=T.surface,.border=EmZero) {
            if(ImageButtonTinted("/system/images/ui-launcher.eic",26,T.accent))request_apps();
            system_menu();
            if(g_bar_cfg.bar_workspaces)workspace_menu();
            if(g_focus_name[0] && em_viewport_width()>900) {
                Menu(g_focus_name,.font=Caption,.height=g_bar_cfg.bar_size-4,.menu_above=g_bar_cfg.bar_edge?g_bar_cfg.bar_size:0,.width=260) {
                    for(int i=0;i<g_win_n;i++) {
                        char text[64];snprintf(text,sizeof text,"%s%s",g_wins[i].title,(g_wins[i].flags&EMBK_WIN_MINIMIZED)?"  · hidden":"");
                        if(MenuItem(text))embk_win_raise(g_wins[i].pid);
                    }
                }
            }
            Spacer();
            if(g_bar_cfg.bar_order)clock_menu();
            if(g_bar_cfg.bar_cpu && em_viewport_width()>1050)Text(cpu_text()).caption().secondary();
            if(g_bar_cfg.bar_network)status_network();
            if(g_bar_cfg.bar_sound)status_sound();
            if(g_bar_cfg.bar_power)status_power();
            if(!g_bar_cfg.bar_order)clock_menu();
        }
        if(!g_bar_cfg.bar_edge)Spacer();
    }
}

/* Not EM_APPLICATION: the bar has to span whatever display it finds, and the
 * macro's spec is fixed at compile time. Same runtime, one line of setup. */
static EmApp em_app_spec_ = {
    .title    = "TopBar",
    .size     = { BAR_W, BAR_H },
    .theme    = Dark,
    .chrome   = Chromeless,
    .material = Translucent,      /* thin transparent bar; grows for dropdowns */
    .refresh_ms = 500,           /* the clock is live, and the ink re-samples */
    .view     = bar,
};

int main(void) {
    uint32_t sw = 0, sh = 0;
    embk_screen_size(&sw, &sh);
    if (sw) em_app_spec_.size.w = (int)sw;    /* a menu bar spans the display */
    (void)sh;
    /* Off the render path, deliberately -- see bar_cfg_thread. */
    oscfg_defaults(&g_bar_cfg);
    g_bar_pending = g_bar_cfg;
    embk_thread_create(bar_cfg_thread, 0);
    return em_app_run(&em_app_spec_);
}
