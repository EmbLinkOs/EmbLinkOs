/* A guest-side terminal witness. No mock descriptors or injected kernel state. */
#include "embk.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>

static int failures,checks;
#define CHECK(expr) do { checks++; if (!(expr)) { failures++; printf("ttyprobe: FAIL line %d: %s (errno %d)\n",__LINE__,#expr,errno); } } while (0)
static void raw(int fd) { struct termios t; CHECK(tcgetattr(fd,&t)==0); cfmakeraw(&t); CHECK(tcsetattr(fd,TCSANOW,&t)==0); }
static int readable(int fd) {
    for (int i=0;i<300;i++) {
        if (embk_fd_poll(fd,EMBK_POLLIN)&(EMBK_POLLIN|EMBK_POLLHUP)) return 1;
        embk_sleep_ms(10);
    }
    return 0;
}
static int child(const char *mode) {
    if (!isatty(0) || !isatty(1) || !isatty(2)) return 21;
    struct termios t;
    if (tcgetattr(0,&t)) return 22;
    if (!strcmp(mode,"--keys")) {
        cfmakeraw(&t); if (tcsetattr(0,TCSANOW,&t)) return 30;
        printf("Raw key check: Ctrl+B, Ctrl+S, Ctrl+H, Return, Left.\r\n"); fflush(stdout);
        unsigned char bytes[7],expected[]={2,19,8,13,27,'[','D'};
        size_t off=0;
        while (off<sizeof bytes) {
            ssize_t n=read(0,bytes+off,sizeof bytes-off); if (n<=0) return 31;
            off+=(size_t)n;
        }
        if (memcmp(bytes,expected,sizeof bytes)) return 32;
        printf("Raw key sequence verified.\r\n"); return 0;
    }
    if (!strcmp(mode,"--echo")) {
        cfmakeraw(&t); if (tcsetattr(0,TCSANOW,&t)) return 23;
        if (write(1,"ready",5)!=5) return 24;
        unsigned char c; if (read(0,&c,1)!=1) return 25;
        return write(1,&c,1)==1?0:26;
    }
    if (!strcmp(mode,"--blockedwrite")) {
        if (write(1,"ready",5)!=5) return 33;
        char bytes[4096]; memset(bytes,'w',sizeof bytes);
        for (;;) {
            ssize_t n=write(1,bytes,sizeof bytes); if (n<0) return 34;
        }
    }
    if (!strcmp(mode,"--hangup")) {
        if (write(1,"ready",5)!=5) return 27;
        unsigned char c; return read(0,&c,1)==0?0:28;
    }
    return 29;
}
static long spawn_child(int slave,const char *mode) {
    struct embk_spawn_file_action actions[3]; memset(actions,0,sizeof actions);
    for (int i=0;i<3;i++) { actions[i].kind=EMBK_SPAWN_ACTION_INHERIT_FD; actions[i].target_fd=i; actions[i].src_obj_handle=slave; }
    char *argv[]={"ttyprobe",(char *)mode,NULL};
    return embk_spawn("/data/apps/ttyprobe/ttyprobe.elf",argv,actions,3);
}
static volatile int read_started,read_finished,read_result;
static void blocked_reader(long fd) {
    read_started=1; char c; read_result=(int)read((int)fd,&c,1); read_finished=1; embk_thread_exit(0);
}
static int selftest(void) {
    int a[2],b[2]; char buf[128]; struct termios t; struct winsize size={37,123,900,650},got;
    CHECK(embk_openpty(a)==0); CHECK(embk_openpty(b)==0);
    CHECK(isatty(a[0]) && isatty(a[1]));
    CHECK(tcgetattr(a[1],&t)==0 && (t.c_lflag&ICANON) && (t.c_lflag&ECHO));
    CHECK(ioctl(a[0],TIOCSWINSZ,&size)==0 && ioctl(a[1],TIOCGWINSZ,&got)==0 && !memcmp(&size,&got,sizeof size));
    CHECK(ioctl(b[0],TIOCGWINSZ,&got)==0 && !got.ws_col && !got.ws_row);
    CHECK(write(a[0],"ab",2)==2 && embk_fd_avail(a[1])==0);
    CHECK(write(a[0],"\177c\r",3)==3);
    CHECK(read(a[1],buf,sizeof buf)==3 && !memcmp(buf,"ac\n",3));
    CHECK(read(a[0],buf,sizeof buf)==8 && !memcmp(buf,"ab\b \bc\r\n",8));
    CHECK(embk_fd_avail(b[0])==0 && embk_fd_avail(b[1])==0);
    t.c_lflag&=~ECHO; CHECK(tcsetattr(a[1],TCSANOW,&t)==0);
    CHECK(write(a[0],"ab\004x\n\004y\n",8)==8);
    CHECK(read(a[1],buf,sizeof buf)==2 && !memcmp(buf,"ab",2));
    CHECK(read(a[1],buf,sizeof buf)==2 && !memcmp(buf,"x\n",2));
    CHECK(read(a[1],buf,sizeof buf)==0); /* EOF is one record, not a permanent shutdown */
    CHECK(read(a[1],buf,sizeof buf)==2 && !memcmp(buf,"y\n",2));
    CHECK(write(a[0],"pending",7)==7); raw(a[1]);
    CHECK(read(a[1],buf,sizeof buf)==7 && !memcmp(buf,"pending",7));
    CHECK(write(a[0],"\033[A",3)==3 && read(a[1],buf,sizeof buf)==3 && !memcmp(buf,"\033[A",3));
    CHECK(embk_fcntl_set_nonblock(a[1],1)==0);
    CHECK(read(a[1],buf,1)==-1 && errno==EAGAIN);
    CHECK(read(a[1],buf,0)==0 && write(a[0],buf,0)==0);
    CHECK(tcgetattr(a[1],&t)==0); t.c_lflag|=0x80000000u;
    CHECK(tcsetattr(a[1],TCSANOW,&t)==-1 && errno==ENOTSUP);
    CHECK(tcsetattr(a[1],TCSADRAIN,&t)==-1 && errno==ENOTSUP);
    CHECK(tcgetattr(a[1],&t)==0); t.c_cc[VMIN]=0; CHECK(tcsetattr(a[1],TCSANOW,&t)==0);
    CHECK(read(a[1],buf,1)==0);
    CHECK(tcgetattr(-1,&t)==-1 && errno==EBADF);
    CHECK(tcgetattr(63,&t)==-1 && errno==EBADF);
    int f=embk_open("/dev/null",EMBK_O_RDWR,0); CHECK(f>=0);
    CHECK(tcgetattr(f,&t)==-1 && errno==ENOTTY); close(f);
    int dupslave=dup(a[1]); CHECK(dupslave>=0); close(a[1]);
    CHECK(write(a[0],"q",1)==1 && read(dupslave,buf,1)==1 && buf[0]=='q');
    close(dupslave); CHECK(readable(a[0]) && read(a[0],buf,1)==0);
    CHECK(write(a[0],"x",1)==-1 && errno==EPIPE); close(a[0]);
    /* An in-flight operation must not keep its peer descriptor alive. */
    long thread=embk_thread_create(blocked_reader,b[0]); CHECK(thread>=0);
    while (!read_started) embk_yield();
    embk_sleep_ms(25); close(b[1]);
    for (int i=0;i<100 && !read_finished;i++) embk_sleep_ms(10);
    CHECK(read_finished && read_result==0); CHECK(embk_thread_join((int)thread)==0); close(b[0]);
    /* Real spawn inheritance: child raw read receives a byte without Enter. */
    CHECK(embk_openpty(a)==0); long h=spawn_child(a[1],"--echo"); CHECK(h>=0); close(a[1]);
    CHECK(readable(a[0]) && read(a[0],buf,sizeof buf)==5 && !memcmp(buf,"ready",5));
    CHECK(write(a[0],"Z",1)==1); CHECK(readable(a[0]) && read(a[0],buf,sizeof buf)==1 && buf[0]=='Z');
    CHECK(embk_wait((int)h)==0); close(a[0]);
    CHECK(embk_openpty(a)==0); h=spawn_child(a[1],"--hangup"); CHECK(h>=0); close(a[1]);
    CHECK(readable(a[0]) && read(a[0],buf,sizeof buf)==5); close(a[0]); CHECK(embk_wait((int)h)==0);
    /* Hard kill does not unwind a blocked syscall's kernel stack. Both read
     * and write pins must be released by thread reaping, not leaked forever. */
    for (int i=0;i<2;i++) {
        CHECK(embk_openpty(a)==0);
        h=spawn_child(a[1],i?"--blockedwrite":"--hangup"); CHECK(h>=0); close(a[1]);
        CHECK(readable(a[0]) && read(a[0],buf,5)==5 && !memcmp(buf,"ready",5));
        embk_sleep_ms(25); CHECK(embk_kill((int)h)==0); CHECK(embk_wait((int)h)==-1);
        close(a[0]);
    }
    /* Faulting copyout must release both newly allocated descriptors. */
    for (int i=0;i<20;i++) CHECK(embk_terminal_control(-1,EMBK_TERMINAL_OPEN,(void *)1,8)==-14);
    CHECK(embk_openpty(a)==0); CHECK(tcgetattr(a[1],&t)==0);
    CHECK(embk_terminal_foreground(a[1],EMBK_TERMINAL_SELF)==0); CHECK(embk_intr_catch(1)>=0);
    CHECK(write(a[0],"\003",1)==1 && embk_intr_take()==1);
    CHECK(embk_intr_catch(0)>=0);
    CHECK(embk_terminal_foreground(a[1],EMBK_TERMINAL_NONE)==0); close(a[0]); close(a[1]);
    /* A rejected spawn after one transfer must not strand the terminal. */
    CHECK(embk_openpty(a)==0);
    struct embk_spawn_file_action act[2]; memset(act,0,sizeof act);
    for (int i=0;i<2;i++) { act[i].kind=EMBK_SPAWN_ACTION_INHERIT_FD; act[i].target_fd=i; act[i].src_obj_handle=i?64:a[1]; }
    char *argv[]={"ttyprobe","--hangup",NULL};
    CHECK(embk_spawn("/data/apps/ttyprobe/ttyprobe.elf",argv,act,2)<0);
    close(a[1]); CHECK(readable(a[0]) && read(a[0],buf,1)==0); close(a[0]);
    /* Bounded queues apply backpressure and retain byte-exact data. */
    CHECK(embk_openpty(a)==0); raw(a[1]);
    CHECK(embk_fcntl_set_nonblock(a[0],1)==0 && embk_fcntl_set_nonblock(a[1],1)==0);
    char large[4096],copy[4096]; memset(large,'k',sizeof large);
    CHECK(write(a[0],large,sizeof large)==sizeof large);
    CHECK(write(a[0],"x",1)==-1 && errno==EAGAIN);
    CHECK(read(a[1],copy,sizeof copy)==sizeof copy && !memcmp(copy,large,sizeof copy));
    CHECK(write(a[1],large,sizeof large)==sizeof large);
    CHECK(write(a[1],"x",1)==-1 && errno==EAGAIN);
    CHECK(read(a[0],copy,sizeof copy)==sizeof copy && !memcmp(copy,large,sizeof copy));
    CHECK(tcgetattr(a[1],&t)==0); t.c_lflag=ICANON;
    CHECK(tcsetattr(a[1],TCSANOW,&t)==0 && write(a[0],"discard",7)==7);
    cfmakeraw(&t); CHECK(tcsetattr(a[1],TCSAFLUSH,&t)==0);
    CHECK(read(a[1],buf,1)==-1 && errno==EAGAIN);
    close(a[0]); close(a[1]);
    int pairs[16][2],count=0;
    while (count<16 && embk_openpty(pairs[count])==0) count++;
    CHECK(count==16 && embk_openpty(a)==-23);
    for (int i=0;i<count;i++) { close(pairs[i][0]); close(pairs[i][1]); }
    CHECK(embk_openpty(a)==0); close(a[0]); close(a[1]);
    printf("ttyprobe: %d/%d checks passed\n",checks-failures,checks); return failures?1:0;
}
static int interactive(void) {
    struct termios saved,rawmode; struct winsize size;
    if (tcgetattr(0,&saved) || ioctl(0,TIOCGWINSZ,&size)) { puts("A terminal session is required."); return 1; }
    printf("Terminal session: %u columns, %u rows\n",size.ws_col,size.ws_row);
    puts("Canonical input: type a line; Backspace edits it, then Enter."); fflush(stdout);
    char line[128]; ssize_t n=read(0,line,sizeof line-1); if (n<=0) return 1;
    line[n]=0; printf("Received: %s",line);
    rawmode=saved; cfmakeraw(&rawmode); if (tcsetattr(0,TCSANOW,&rawmode)) return 2;
    printf("Raw input: press one key without Enter.\r\n"); fflush(stdout);
    unsigned char c; n=read(0,&c,1);
    if (tcsetattr(0,TCSANOW,&saved)) return 3;
    if (n!=1) return 4;
    printf("\nReceived immediately: 0x%02x\n",c);
    puts("Canonical input restored. Ctrl+D on an empty line finishes; Ctrl+C interrupts."); fflush(stdout);
    n=read(0,line,sizeof line); if (n<0) return 5;
    puts("Terminal probe complete."); return 0;
}

/* Real ring-3 full-screen acceptance witness, using only terminal stdio. */
static int screen_witness(void) {
    struct termios saved,t;struct winsize size={0},previous={0};
    if(tcgetattr(0,&saved) || ioctl(0,TIOCGWINSZ,&size))return 40;
    t=saved;cfmakeraw(&t);if(tcsetattr(0,TCSANOW,&t))return 41;
    printf("\033[?1049h\033[?25l\033[?1h\033[?2004h");fflush(stdout);
    unsigned selected=0,logs=0,pasted=0,paste_lines=0;bool paste=false,redraw=true;
    char sequence[80];unsigned sequence_n=0,state=0;char status[160]="Ready for raw keys; no Enter required.";
    for(;;) {
        if(embk_cancelled())break;
        if(ioctl(0,TIOCGWINSZ,&size)==0 && memcmp(&size,&previous,sizeof size)){previous=size;redraw=true;}
        if(redraw) {
            printf("\033[r\033[0m\033[2J\033[H");
            if(size.ws_col<35 || size.ws_row<13)printf("Resize to at least 35 x 13. q: return");
            else {
                printf("\033[1;38;2;115;210;188mEmbLink terminal screen check\033[0m");
                printf("\033[2;1HLive PTY dimensions: %u columns, %u rows",size.ws_col,size.ws_row);
                printf("\033[3;1HArrows: select | Enter: log | r: reply | q: return");
                const char *names[]={"Primary viewport","Scrolling region","Unicode cells"};
                for(unsigned i=0;i<3;i++)printf("\033[%u;1H%s %c %-25s\033[0m",5+i,i==selected?"\033[48;2;56;95;89;38;2;238;247;242m":"",i==selected?'>':' ',names[i]);
                printf("\033[8;1HUTF-8: caf\xc3\xa9   e\xcc\x81   \xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90");
                printf("\033[9;1H\033[38;5;109mScroll-region log\033[0m");
                printf("\033[10;%ur\033[%u;1H",size.ws_row-2,size.ws_row-2);
                printf("Log %03u: only this region scrolls",logs);
                printf("\033[%u;1H\033[2K%s",size.ws_row,status);
            }
            fflush(stdout);redraw=false;
        }
        if(!(embk_fd_poll(0,EMBK_POLLIN)&EMBK_POLLIN)){embk_sleep_ms(10);continue;}
        unsigned char c;if(read(0,&c,1)!=1)break;
        if(state==1){if(c=='[' || c=='O'){state=2;sequence_n=0;}else state=0;continue;}
        if(state==2) {
            if(sequence_n+1<sizeof sequence)sequence[sequence_n++]=(char)c;
            if(c>=0x40 && c<=0x7e) {
                sequence[sequence_n]=0;state=0;
                if(!strcmp(sequence,"A")){selected=(selected+2)%3;redraw=true;}
                else if(!strcmp(sequence,"B")){selected=(selected+1)%3;redraw=true;}
                else if(!strcmp(sequence,"4;10R")){snprintf(status,sizeof status,"Cursor reply verified: row 4, column 10");redraw=true;}
                else if(!strcmp(sequence,"200~")){paste=true;pasted=paste_lines=0;}
                else if(!strcmp(sequence,"201~")){paste=false;snprintf(status,sizeof status,"Bracketed paste verified: %u bytes, %u newline(s)",pasted,paste_lines);redraw=true;}
            }
            continue;
        }
        if(c==27){state=1;continue;}
        if(paste){pasted++;if(c=='\n')paste_lines++;continue;}
        if(c=='q' || c==3)break;
        if(c=='r'){printf("\033[4;10H\033[6n");fflush(stdout);continue;}
        if(c=='\r' && size.ws_row>=13 && size.ws_col>=35) {
            logs++;printf("\033[%u;1H\r\n\033[2KLog %03u: only this region scrolls",size.ws_row-2,logs);fflush(stdout);
        }
    }
    printf("\033[r\033[?2004l\033[?1l\033[?25h\033[0m\033[?1049l");fflush(stdout);
    if(tcsetattr(0,TCSANOW,&saved))return 42;
    puts("Alternate screen restored.");return 0;
}
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--screen")) return screen_witness();
    if (argc>1 && !strcmp(argv[1],"--selftest")) return selftest();
    if (argc>1) return child(argv[1]);
    return interactive();
}
