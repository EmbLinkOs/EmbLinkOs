/* A single-document workspace. Durable saves and recoverable drafts are shared
 * with Note++; the runtime routes every ordinary close through the same guard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "embk.h"
#include "emfiles.h"
#include "document.h"
#include "workspace.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#define DOC_MAX (256*1024)
static char g_path[256],g_doc[DOC_MAX],g_status[128],g_pending_path[256];
static int g_cur,g_confirm,g_allow_close,g_loaded;
static uint64_t g_saved_hash,g_checkpoint_ms;
static struct emdoc_stamp g_original;
static struct emdoc_workspace g_workspace;
enum { NONE,NEW,OPEN_PANEL,OPEN_PATH,CLOSE };
static int g_pending;
static const char *base_name(const char *p) {
    const char *b=strrchr(p,'/');return p[0]?(b?b+1:p):"Untitled";
}
static const char *parent(const char *p) {
    static char dir[256];
    if(!p[0]) {const char *home=getenv("HOME");return home&&home[0]?home:"/home";}
    snprintf(dir,sizeof dir,"%s",p);char *slash=strrchr(dir,'/');
    if(slash==dir)slash[1]=0;else if(slash)*slash=0;return dir;
}
static int dirty(void) {return emdoc_hash(g_doc,strlen(g_doc))!=g_saved_hash;}
static void checkpoint(void) {
    struct emdoc_draft meta={.cursor=(uint32_t)g_cur,.original=g_original};
    snprintf(meta.path,sizeof meta.path,"%s",g_path);
    if(emws_checkpoint(&g_workspace,0,&meta,g_doc,dirty()))
        snprintf(g_status,sizeof g_status,"Draft recovery unavailable; save your work");
}
static void idle(void) {
    uint64_t now=embk_uptime_ms();if(now-g_checkpoint_ms<2000)return;
    g_checkpoint_ms=now;checkpoint();
}
static int restore(const struct emdoc_draft *meta,const char *text) {
    if(g_loaded)return -1;
    snprintf(g_path,sizeof g_path,"%s",meta->path);strcpy(g_doc,text);
    g_cur=(int)meta->cursor;g_original=meta->original;
    g_saved_hash=emdoc_hash(g_doc,strlen(g_doc))^1;g_loaded=1;
    snprintf(g_status,sizeof g_status,"Recovered unsaved draft");
    embk_puts(1,"Editor: recovered unsaved draft\n");return 0;
}
static int load_path(const char *path) {
    if(strlen(path)>=sizeof g_path){snprintf(g_status,sizeof g_status,"Path is too long");return 0;}
    struct emdoc_stamp stamp;
    if(emdoc_read(path,g_doc,sizeof g_doc,&stamp)) {
        snprintf(g_status,sizeof g_status,"Cannot open: %s",errno==EFBIG?"file exceeds 256 KB":
            errno==EILSEQ?"binary file":strerror(errno));return 0;
    }
    g_original=stamp;snprintf(g_path,sizeof g_path,"%s",path);g_cur=0;
    g_saved_hash=emdoc_hash(g_doc,strlen(g_doc));
    snprintf(g_status,sizeof g_status,"Opened %lu bytes",(unsigned long)strlen(g_doc));
    embk_puts(1,"Editor: document opened\n");return 1;
}
static int save_file(int save_as) {
    char path[256];struct emdoc_stamp expected=g_original;
    snprintf(path,sizeof path,"%s",g_path);
    if(save_as || !path[0]) {
        int r=embk_file_panel(EMFILE_SAVE,"Save document",parent(g_path),base_name(g_path),path,sizeof path);
        if(r<=0){if(r<0)snprintf(g_status,sizeof g_status,"Save dialog unavailable (%d)",r);return 0;}
        if(emdoc_probe(path,&expected)){snprintf(g_status,sizeof g_status,"Cannot inspect destination");return 0;}
    }
    struct emdoc_stamp saved;
    if(emdoc_save(path,g_doc,strlen(g_doc),&expected,&saved)) {
        snprintf(g_status,sizeof g_status,"%s",errno==EAGAIN?"File changed on disk. Save As a different name.":"Save failed; changes are still open");
        checkpoint();return 0;
    }
    g_original=saved;snprintf(g_path,sizeof g_path,"%s",path);
    g_saved_hash=emdoc_hash(g_doc,strlen(g_doc));emws_forget(&g_workspace,0);
    snprintf(g_status,sizeof g_status,"Saved %lu bytes",(unsigned long)strlen(g_doc));
    embk_puts(1,"Editor: document saved and synced\n");return 1;
}
static void perform(int action) {
    g_confirm=0;g_pending=NONE;
    if(action==CLOSE){g_allow_close=1;em_app_request_exit(0);return;}
    if(action==NEW){emws_forget(&g_workspace,0);g_doc[0]=g_path[0]=0;g_cur=0;
        memset(&g_original,0,sizeof g_original);g_saved_hash=emdoc_hash("",0);g_status[0]=0;return;}
    char path[256];snprintf(path,sizeof path,"%s",g_pending_path);
    if(action==OPEN_PANEL) {
        int r=embk_file_panel(EMFILE_OPEN,"Open document",parent(g_path),0,path,sizeof path);
        if(r<=0){if(r<0)snprintf(g_status,sizeof g_status,"Open dialog unavailable (%d)",r);return;}
    }
    if(load_path(path))emws_forget(&g_workspace,0);
}
static void request(int action) {
    if(dirty()){g_pending=action;g_confirm=1;checkpoint();em_request_frame();}
    else perform(action);
}
static int close_window(void) {
    if(g_allow_close || !dirty()){emws_forget(&g_workspace,0);return 1;}
    request(CLOSE);return 0;
}
static int on_key(int ch) {
    if(g_confirm){if(ch==27){g_confirm=0;g_pending=NONE;}return 1;}
    if((em_key_modifiers()&2) && ch==19){save_file(0);return 1;}
    if((em_key_modifiers()&2) && ch==15){request(OPEN_PANEL);return 1;}
    if((em_key_modifiers()&2) && ch==14){request(NEW);return 1;}
    return 0;
}
static void app(void) {
    if(!g_loaded) {
        em_set_key_hook(on_key);em_set_close_hook(close_window);em_set_idle_hook(idle);
        emws_init(&g_workspace,"editor");g_saved_hash=emdoc_hash("",0);
        const char *path=getenv("EDIT_FILE");
        if(path && path[0]) {load_path(path);g_loaded=1;}
        else {emws_restore(&g_workspace,"edit",sizeof g_doc,restore);g_loaded=1;}
    }
    struct embk_drop d;char dropped[256];
    if(embk_drop_take(&d,dropped,sizeof dropped-1)==1 && !strcmp(d.type,"path") && d.len && d.len<sizeof dropped) {
        dropped[d.len]=0;snprintf(g_pending_path,sizeof g_pending_path,"%s",dropped);request(OPEN_PATH);
    }
    Window("Editor") {
        AppBar(base_name(g_path)){}
        VStack(.spacing=10,.padding=16,.align=Fill,.grow=1) {
            HStack(.spacing=8,.align=Fill) {
                Text(dirty()?"Unsaved changes":"All changes saved").caption().secondary();
                Spacer();
                if(Button("New").ghost().clicked())request(NEW);
                if(Button("Open").ghost().clicked())request(OPEN_PANEL);
                if(Button("Save As").ghost().clicked())save_file(1);
                if(Button("Save").primary().clicked())save_file(0);
            }
            TextEditor(g_doc,sizeof g_doc,&g_cur,em_viewport_height()>240?em_viewport_height()-168:72);
            Text(g_path[0]?g_path:"Choose a name and location when you save").caption().secondary();
            Text(g_status).caption().secondary();
        }
        if(g_confirm) {
            Overlay() {
                Dialog(.width=420,.spacing=14,.padding=20) {
                    Text("Save your changes?").heading();
                    Text(base_name(g_path)).caption();
                    Text("Your document has unsaved changes.").caption().secondary();
                    HStack(.spacing=10) {
                        if(Button("Cancel").ghost().clicked()){g_confirm=0;g_pending=NONE;}
                        Spacer();
                        if(Button("Discard").destructive().clicked()) {emws_forget(&g_workspace,0);perform(g_pending);}
                        if(Button("Save changes").primary().clicked() && save_file(0))perform(g_pending);
                    }
                }
            }
        }
    }
}
EM_APPLICATION {
    .title="Editor",.size={740,540},.theme=Dark,.chrome=Chromeless,.resize=Resizable,.view=app,
    .min_size = { 400, 300 },
};
