#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "drivers/audio/audio.h"
#include "drivers/audio/pcm.h"
#include "include/spinlock.h"
static int16_t written[32][2048];static unsigned present=0,stops,plays;
void kprintf(const char *f,...) {(void)f;}
void spinlock_init(spinlock_t *l) {l->locked=0;}
void spin_lock(spinlock_t *l) {assert(!l->locked);l->locked=1;}
void spin_unlock(spinlock_t *l) {assert(l->locked);l->locked=0;}
static bool mock_init(void) {return present;}
bool ac97_present(void) {return present;}
uint32_t ac97_sample_rate(void) {return present?48000:0;}
uint32_t ac97_frames_per_buffer(void) {return 1024;}
uint32_t ac97_fill(int i,const int16_t *p,uint32_t n) {assert(i>=0 && i<32 && n<=1024);memcpy(written[i],p,n*4);return n;}
uint8_t ac97_civ(void) {return 0;}
void ac97_set_last(int i) {(void)i;}
void ac97_play(int i) {assert(i>=0);plays++;}
bool ac97_done(int i) {(void)i;return true;}
void ac97_stop(void) {stops++;}
void hda_init(void) {}
bool hda_present(void) {return false;}
uint32_t hda_fill(int i,const int16_t *p,uint32_t n) {(void)i;(void)p;(void)n;assert(0);return 0;}
uint8_t hda_civ(void) {assert(0);return 0;}
void hda_play(int i) {(void)i;assert(0);}
void hda_set_last(int i) {(void)i;assert(0);}
bool hda_done(int i) {(void)i;assert(0);return true;}
void hda_stop(void) {assert(0);}
const char *hda_output_name(void) {return "mock";}
static uint64_t underruns(void) {return 0;}
static bool no_init(void) {return false;}
const struct pcm_driver hda_driver={.name="HDA mock",.init=no_init};
const struct pcm_driver ac97_driver={.name="AC97 mock",.init=mock_init,
    .sample_rate=ac97_sample_rate,.frames_per_buffer=ac97_frames_per_buffer,
    .fill=ac97_fill,.civ=ac97_civ,.set_last=ac97_set_last,.play=ac97_play,
    .done=ac97_done,.stop=ac97_stop,.underruns=underruns};
int main(void) {
    assert(!audio_init() && audio_open(9)==-19 && audio_control(20,0)==-19);
    present=1;assert(audio_init());struct embk_audio_info info;audio_snapshot(&info);
    assert(info.flags==EMBK_AUDIO_AVAILABLE && info.sample_rate==48000 && info.percent==100);
    assert(!audio_open(9) && audio_open(10)==-16);unsigned stopped=stops;
    assert(!audio_open(9) && stops==stopped); /* a repeated open cannot reset DMA */
    int16_t frames[]={32767,-32768,8000,-8000};uint32_t n;
    assert(audio_write(10,frames,2,&n)==-1 && !n);
    assert(audio_control(101,0)==-22 && audio_control(50,2)==-22);
    assert(!audio_control(50,0) && !audio_write(9,frames,2,&n) && n==2);
    assert(written[0][0]==16383 && written[0][1]==-16384 && written[0][2]==4000 && written[0][3]==-4000);
    assert(!audio_control(100,1) && !audio_write(9,frames,2,&n));
    assert(!written[1][0] && !written[1][1]);
    audio_reap_pid(10);assert(audio_open(10)==-16);
    audio_reap_pid(9);assert(!audio_open(10));audio_close(10);
    puts("audio sink: linear gain, mute, owner isolation, idempotent open and process cleanup OK");
}
