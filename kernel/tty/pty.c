#include "pty.h"
#include "process/process.h"
#include "fs/fd.h"
#include "include/errno.h"
#include "include/kstring.h"

#define PTY_MAX 16
#define PTY_QUEUE 4096
#define PTY_LINE 1024
#define RECORD_END 0x200u
#define RECORD_EOF 0x100u

struct pty {
    bool used;
    unsigned refs[2], busy;           /* descriptor references separate from I/O pins */
    bool hungup[2];                   /* permanent, even if an operation owns a ref */
    uint16_t input[PTY_QUEUE];
    uint8_t output[PTY_QUEUE], line[PTY_LINE];
    unsigned in_head, in_tail, in_count, out_head, out_tail, out_count, fill;
    struct wait_queue changed;
    struct embk_terminal_attr attr;
    struct embk_terminal_size size;
    uint32_t foreground;
};
static struct pty terminals[PTY_MAX];

int pty_create(struct pty **out) {
    sched_lock();
    for (unsigned i=0;i<PTY_MAX;i++) if (!terminals[i].used) {
        struct pty *p=&terminals[i]; memset(p,0,sizeof *p);
        p->used=true; p->refs[0]=p->refs[1]=1;
        p->attr.iflag=EMBK_TIF_ICRNL;
        p->attr.oflag=EMBK_TOF_OPOST|EMBK_TOF_ONLCR;
        p->attr.cflag=EMBK_TCF_CS8;
        p->attr.lflag=EMBK_TLF_ICANON|EMBK_TLF_ECHO|EMBK_TLF_ECHOE|EMBK_TLF_ECHOK|EMBK_TLF_ISIG;
        p->attr.cc[EMBK_TCC_INTR]=3; p->attr.cc[EMBK_TCC_ERASE]=127;
        p->attr.cc[EMBK_TCC_KILL]=21; p->attr.cc[EMBK_TCC_EOF]=4;
        p->attr.cc[EMBK_TCC_MIN]=1;
        /* Unknown until the emulator measures its actual viewport. */
        *out=p; sched_unlock(); return 0;
    }
    sched_unlock(); return -EMBK_ENFILE;
}
void pty_ref_locked(struct pty *p,int side) { p->refs[side]++; }
void pty_unref_locked(struct pty *p,int side) {
    if (--p->refs[side]==0) {
        p->hungup[side]=true;
        if (!side && p->foreground) (void)process_cancel_locked(p->foreground);
        wait_queue_wake_all(&p->changed);
    }
    if (!p->refs[0] && !p->refs[1] && !p->busy) p->used=false;
}
void pty_pin_locked(struct pty *p) { p->busy++; }
void pty_unpin_locked(struct pty *p) {
    if (--p->busy==0 && !p->refs[0] && !p->refs[1]) p->used=false;
}
unsigned pty_live_count(void) {
    unsigned n=0; sched_lock();
    for (unsigned i=0;i<PTY_MAX;i++) n+=terminals[i].used;
    sched_unlock(); return n;
}
static void input(struct pty *p,uint16_t c) {
    p->input[p->in_head]=c; p->in_head=(p->in_head+1)%PTY_QUEUE; p->in_count++;
}
static void output(struct pty *p,uint8_t c) {
    p->output[p->out_head]=c; p->out_head=(p->out_head+1)%PTY_QUEUE; p->out_count++;
}
static void echo(struct pty *p,uint8_t c) {
    if (c=='\n' && (p->attr.oflag&(EMBK_TOF_OPOST|EMBK_TOF_ONLCR))==(EMBK_TOF_OPOST|EMBK_TOF_ONLCR)) output(p,'\r');
    output(p,c);
}
static void commit_line(struct pty *p,bool eof) {
    if (!p->fill && eof) input(p,RECORD_EOF);
    for (unsigned i=0;i<p->fill;i++) input(p,p->line[i]|(i+1==p->fill?RECORD_END:0));
    p->fill=0;
}
static int wait_changed(struct pty *p,bool nonblock) {
    if (nonblock) { sched_unlock(); return -EMBK_EAGAIN; }
    if (current_thread && current_process->cancelled) { sched_unlock(); return -EMBK_ECANCELED; }
    sched_block_current_locked(&p->changed); sched_lock(); return 0;
}
int pty_read(struct pty *p,int side,void *buf,size_t len,size_t *out,bool nonblock) {
    *out=0; if (!len) return 0;
    sched_lock();
    while (!(side?p->in_count:p->out_count)) {
        if (p->hungup[!side]) {
            /* Master hangup makes an unterminated canonical line readable. */
            if (side && p->fill) { commit_line(p,false); break; }
            sched_unlock(); return 0;
        }
        if (side && !(p->attr.lflag&EMBK_TLF_ICANON) && !p->attr.cc[EMBK_TCC_MIN]) { sched_unlock(); return 0; }
        int rc=wait_changed(p,nonblock); if (rc) return rc;
    }
    uint8_t *dst=buf;
    while (*out<len && (side?p->in_count:p->out_count)) {
        if (side) {
            uint16_t c=p->input[p->in_tail];
            if (c==RECORD_EOF && *out) break;
            p->in_tail=(p->in_tail+1)%PTY_QUEUE; p->in_count--;
            if (c==RECORD_EOF) break;
            dst[(*out)++]=(uint8_t)c;
            if (c&RECORD_END) break;
        } else {
            dst[(*out)++]=p->output[p->out_tail];
            p->out_tail=(p->out_tail+1)%PTY_QUEUE; p->out_count--;
        }
    }
    wait_queue_wake_all(&p->changed); sched_unlock(); return 0;
}
int pty_write(struct pty *p,int side,const void *buf,size_t len,size_t *out,bool nonblock) {
    const uint8_t *src=buf; *out=0; sched_lock();
    while (*out<len) {
        if (p->hungup[!side]) { sched_unlock(); return *out?0:-EMBK_EPIPE; }
        uint8_t c=src[*out];
        bool canon=(p->attr.lflag&EMBK_TLF_ICANON)!=0;
        bool do_echo=(p->attr.lflag&EMBK_TLF_ECHO)!=0;
        if (!side && c=='\r' && (p->attr.iflag&EMBK_TIF_ICRNL)) c='\n';
        bool intr=!side && (p->attr.lflag&EMBK_TLF_ISIG) && p->attr.cc[EMBK_TCC_INTR] && c==p->attr.cc[EMBK_TCC_INTR];
        unsigned need_in=0,need_out=0;
        if (side) need_out=(c=='\n' && (p->attr.oflag&5u)==5u)?2:1;
        else if (intr) need_out=do_echo?4:0;
        else if (!canon) { need_in=1; need_out=do_echo?2:0; }
        else {
            bool eof=p->attr.cc[EMBK_TCC_EOF] && c==p->attr.cc[EMBK_TCC_EOF];
            bool erase=p->attr.cc[EMBK_TCC_ERASE] && c==p->attr.cc[EMBK_TCC_ERASE];
            bool kill=p->attr.cc[EMBK_TCC_KILL] && c==p->attr.cc[EMBK_TCC_KILL];
            if (c=='\n') need_in=p->fill+1;
            else if (eof) need_in=p->fill?p->fill:1;
            if (do_echo && !eof) need_out=kill?2:erase?3:2;
        }
        if (PTY_QUEUE-p->in_count<need_in || PTY_QUEUE-p->out_count<need_out) {
            if (*out) break; /* partial write; never block after making progress */
            int rc=wait_changed(p,nonblock); if (rc) return rc;
            continue;
        }
        if (side) echo(p,c); /* output processing, independent of ECHO */
        else if (intr) {
            p->in_head=p->in_tail=p->in_count=p->fill=0;
            if (do_echo) {
                if (c<32 || c==127) { output(p,'^'); output(p,c==127?'?':c+64); }
                else output(p,c);
                echo(p,'\n');
            }
            uint32_t target=p->foreground;
            (*out)++; wait_queue_wake_all(&p->changed);
            sched_unlock();
            if (target && process_raise_interrupt(target)==0) process_cancel(target);
            sched_lock(); continue;
        } else if (!canon) {
            input(p,c); if (do_echo) echo(p,c);
        } else if (p->attr.cc[EMBK_TCC_EOF] && c==p->attr.cc[EMBK_TCC_EOF]) commit_line(p,true);
        else if (p->attr.cc[EMBK_TCC_ERASE] && c==p->attr.cc[EMBK_TCC_ERASE]) {
            if (p->fill) {
                p->fill--;
                if (do_echo) {
                    if (p->attr.lflag&EMBK_TLF_ECHOE) { output(p,'\b'); output(p,' '); output(p,'\b'); }
                    else echo(p,c);
                }
            }
        } else if (p->attr.cc[EMBK_TCC_KILL] && c==p->attr.cc[EMBK_TCC_KILL]) {
            p->fill=0; if (do_echo && (p->attr.lflag&EMBK_TLF_ECHOK)) echo(p,'\n');
        } else if (c=='\n') {
            p->line[p->fill++]=c; commit_line(p,false); if (do_echo) echo(p,c);
        } else if (p->fill<PTY_LINE-1) {
            p->line[p->fill++]=c; if (do_echo) echo(p,c);
        }
        (*out)++; wait_queue_wake_all(&p->changed);
    }
    sched_unlock(); return 0;
}
int64_t pty_avail(struct pty *p,int side) {
    sched_lock(); int64_t n=side?p->in_count:p->out_count; sched_unlock(); return n;
}
int pty_poll(struct pty *p,int side,int events) {
    sched_lock(); int ready=0;
    if ((side?p->in_count:p->out_count) || p->hungup[!side]) ready|=events&POLLIN;
    if (p->hungup[!side]) ready|=POLLHUP;
    else if (side?p->out_count<PTY_QUEUE-1:(PTY_QUEUE-p->in_count>=PTY_LINE && PTY_QUEUE-p->out_count>=4)) ready|=events&POLLOUT;
    sched_unlock(); return ready;
}
int pty_control(struct pty *p,int side,unsigned op,void *data) {
    sched_lock(); int rc=0;
    if (op==EMBK_TERMINAL_GET_ATTR) *(struct embk_terminal_attr *)data=p->attr;
    else if (op==EMBK_TERMINAL_GET_SIZE) *(struct embk_terminal_size *)data=p->size;
    else if (op==EMBK_TERMINAL_SET_SIZE) {
        struct embk_terminal_size *s=data;
        if (!s->rows || !s->cols) rc=-EMBK_EINVAL; else p->size=*s;
    } else if (op==EMBK_TERMINAL_FOREGROUND) p->foreground=*(uint32_t *)data;
    else if (op==EMBK_TERMINAL_SET_ATTR || op==EMBK_TERMINAL_SET_ATTR_FLUSH) {
        struct embk_terminal_attr *a=data;
        if ((a->iflag&~EMBK_TIF_ICRNL) || (a->oflag&~5u) || (a->cflag!=0 && a->cflag!=EMBK_TCF_CS8) ||
            (a->lflag&~(EMBK_TLF_ISIG|EMBK_TLF_ICANON|EMBK_TLF_ECHO|EMBK_TLF_ECHOE|EMBK_TLF_ECHOK)) ||
            a->cc[1] || a->cc[7] || a->cc[EMBK_TCC_TIME] || a->cc[EMBK_TCC_MIN]>1 || a->reserved[0] || a->reserved[1]) rc=-EMBK_ENOTSUP;
        else if (op!=EMBK_TERMINAL_SET_ATTR_FLUSH && !(a->lflag&EMBK_TLF_ICANON) && PTY_QUEUE-p->in_count<p->fill) rc=-EMBK_EAGAIN;
        else {
            if (op==EMBK_TERMINAL_SET_ATTR_FLUSH) p->in_head=p->in_tail=p->in_count=p->fill=0;
            else if (!(a->lflag&EMBK_TLF_ICANON)) {
                for (unsigned i=0;i<p->fill;i++) input(p,p->line[i]);
                p->fill=0;
            }
            p->attr=*a; wait_queue_wake_all(&p->changed);
        }
    } else rc=-EMBK_EINVAL;
    (void)side; sched_unlock(); return rc;
}
