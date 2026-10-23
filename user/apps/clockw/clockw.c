/* user/apps/clockw/clockw.c -- the EmbLink CLOCK desktop widget (EmUI V5).
 *
 * A tiny always-on-desktop tile: big uptime clock + a live activity spark.
 * Sits in the widget z-band (above the wallpaper, below every app window);
 * home spawns it at boot. The whole program is a view + one EM_WIDGET. */

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

static void ClockView(void) {
    time_t now=time(NULL);struct tm local;
    char clock[16]="--:--",date[48]="Clock unavailable",uptime[48];
    if(localtime_r(&now,&local)) {strftime(clock,sizeof clock,"%H:%M",&local);strftime(date,sizeof date,"%a %d %B",&local);}
    unsigned long minutes=(unsigned long)(embk_uptime_ms()/60000);
    snprintf(uptime,sizeof uptime,"Up %lu h %02lu min",minutes/60,minutes%60);
    Window("Clock",.glass=1,.corner=ui_theme()->radius_lg) {
        VStack(.spacing=3,.padding=12,.align=Leading) {
            Text(clock).heading();Text(date).caption();Text(uptime).caption().secondary();
        }
    }
}

EM_WIDGET {
    .title      = "Clock",
    .size       = { 190, 108 },
    .pos        = { 24, 52 },
    .theme      = Dark,
    .refresh_ms = 1000,
    .view       = ClockView,
};
