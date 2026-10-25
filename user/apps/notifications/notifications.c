#include <stdio.h>
#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
static struct embk_notice_info history;
static float scroll;
static void app(void) {
    int rc=embk_notify_get(&history,0);
    Window("Notifications") {
        AppBar("Notifications") {
            if(Button("Clear all").ghost().clicked())embk_notify_get(&history,1);
        }
        ContentPane() {
            ScrollView(&scroll,em_viewport_height()-80) {
                VStack(.spacing=12,.grow=1,.align=Fill) {
                    if(rc)Text("Notifications are unavailable for this session.").secondary();
                    else if(!history.count)EmptyState(IconBell,"All caught up","New notifications will appear here.");
                    else for(unsigned i=0;i<history.count && i<EMBK_NOTIFY_MAX;i++) {
                        const struct embk_notice *n=&history.entries[i];
                        char key[32];snprintf(key,sizeof key,"notice%llu",(unsigned long long)n->sequence);
                        Divider();
                        Section(n->title,.key=key) {
                            Text(n->body).body();
                            char age[48];
                            snprintf(age,sizeof age,"%llu min ago",(unsigned long long)((embk_uptime_ms()-n->created_ms)/60000));
                            Text(age).caption().tertiary();
                        }
                    }
                }
            }
        }
    }
}
EM_APPLICATION {.title="Notifications",.size={520,520},.theme=Dark,
    .min_size = { 400, 300 },
    .chrome=Chromeless,.resize=Resizable,.refresh_ms=1000,.view=app};
