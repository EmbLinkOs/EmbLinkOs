/* vt_keys.c -- what a key, a mouse event or a focus change sends to the
 * program, in xterm's encodings. See vt.h. */
#include "vt.h"
#include <stdio.h>
#include <string.h>

size_t vt_key_sequence(const struct vt *v,unsigned key,unsigned mods,char out[16]) {
    unsigned modifier=1+(mods&7);size_t n=0;
    char final=0;
    switch (key) {
    case VT_KEY_UP: final='A';break;
    case VT_KEY_DOWN: final='B';break;
    case VT_KEY_RIGHT: final='C';break;
    case VT_KEY_LEFT: final='D';break;
    case VT_KEY_HOME: final='H';break;
    case VT_KEY_END: final='F';break;
    }
    if (key>=VT_KEY_F1 && key<VT_KEY_F1+4) final=(char)('P'+(key-VT_KEY_F1));
    if (final) {
        if (mods) return (size_t)snprintf(out,16,"\033[1;%u%c",modifier,final);
        out[0]=27;out[1]=key>=VT_KEY_F1 || v->application_cursor?'O':'[';out[2]=final;return 3;
    }
    static const unsigned char tilde[]={15,17,18,19,20,21,23,24};
    unsigned code=key==VT_KEY_DELETE?3:key==VT_KEY_INSERT?2:key==VT_KEY_PAGEUP?5:key==VT_KEY_PAGEDOWN?6:
                  key==VT_KEY_MENU?29:key>=VT_KEY_F1+4 && key<=VT_KEY_F12?tilde[key-VT_KEY_F1-4]:0;
    if (code) return (size_t)snprintf(out,16,mods?"\033[%u;%u~":"\033[%u~",code,modifier);
    if (key>=VT_KEY_KP_0 && key<=VT_KEY_KP_SLASH) {
        static const char app[]="pqrstuvwxynMkmjo",num[]="0123456789.\r+-*/";
        unsigned i=key-VT_KEY_KP_0;
        if (v->keypad_application) {out[0]=27;out[1]='O';out[2]=app[i];return 3;}
        key=(unsigned char)num[i];
    }
    if (key==VT_KEY_TAB) {if (mods&1) {memcpy(out,"\033[Z",3);return 3;} key='\t';}
    else if (key==VT_KEY_BACKSPACE) key=((mods&4)!=0)!=v->backspace_sends_bs?8:127;
    else if (key==VT_KEY_ENTER) key='\r';
    else if (key==VT_KEY_ESCAPE) key=27;
    if (key<256) {
        if ((mods&2) && v->alt_sends_escape) out[n++]=27;
        out[n++]=(char)key;
        if (key=='\r' && v->newline_mode) out[n++]='\n';     /* LNM: Return sends CR LF */
        return n;
    }
    return 0;
}

/* One coordinate in the legacy encodings: 32+value as a byte (X10, up to
 * 223) or as UTF-8 (1005, up to 2015). */
static bool coordinate(char *out,size_t *n,int value,bool utf8) {
    unsigned c=32u+(unsigned)value;
    if (!utf8) {if (c>255) return false;out[(*n)++]=(char)c;return true;}
    if (c>2047) return false;
    if (c<128) out[(*n)++]=(char)c;
    else {out[(*n)++]=(char)(0xc0|(c>>6));out[(*n)++]=(char)(0x80|(c&63));}
    return true;
}
size_t vt_mouse_report(const struct vt *v,enum vt_mouse_event event,int button,int col,int row,
                       int px,int py,unsigned mods,char out[48]) {
    if (v->mouse==VT_MOUSE_OFF || button<0 || button>7) return 0;
    bool wheel=button>=4;
    switch (event) {
    case VT_MOUSE_PRESS: break;
    case VT_MOUSE_RELEASE: if (v->mouse==VT_MOUSE_X10 || wheel) return 0; break;
    case VT_MOUSE_MOTION:
        if (v->mouse==VT_MOUSE_X10 || v->mouse==VT_MOUSE_NORMAL || wheel) return 0;
        if (button==3 && v->mouse!=VT_MOUSE_ANY) return 0;
        break;
    }
    int code=wheel?64+(button-4):button;
    if (event==VT_MOUSE_MOTION) code+=32;
    if (v->mouse!=VT_MOUSE_X10) code|=(int)(((mods&1)?4:0)|((mods&2)?8:0)|((mods&4)?16:0));
    bool sgr=v->mouse_encoding==VT_MOUSE_ENC_SGR || v->mouse_encoding==VT_MOUSE_ENC_SGR_PIXELS;
    if (sgr) {
        bool pixels=v->mouse_encoding==VT_MOUSE_ENC_SGR_PIXELS;
        int x=pixels?px+1:col+1,y=pixels?py+1:row+1;
        int n=snprintf(out,48,"\033[<%d;%d;%d%c",code,x,y,event==VT_MOUSE_RELEASE?'m':'M');
        return n>0?(size_t)n:0;
    }
    /* the legacy encodings cannot say which button went up */
    if (event==VT_MOUSE_RELEASE) code=(code&~3)|3;
    if (v->mouse_encoding==VT_MOUSE_ENC_URXVT) {
        int n=snprintf(out,48,"\033[%d;%d;%dM",code+32,col+1,row+1);
        return n>0?(size_t)n:0;
    }
    size_t n=0;
    memcpy(out,"\033[M",3);n=3;
    bool utf8=v->mouse_encoding==VT_MOUSE_ENC_UTF8;
    if (!coordinate(out,&n,code,false) || !coordinate(out,&n,col+1,utf8) || !coordinate(out,&n,row+1,utf8)) return 0;
    return n;
}
size_t vt_focus_report(const struct vt *v,bool focused,char out[4]) {
    if (!v->focus_events) return 0;
    out[0]=27;out[1]='[';out[2]=focused?'I':'O';
    return 3;
}
