/* user/bin/mp3play.c -- the music player.
 *
 * Ours, all the way down: our decoder (user/audio/mp3/), our resampler, our
 * audio stack, our toolkit. The MP3 arrives as bytes and leaves as sound
 * without passing through anybody else's code.
 *
 * THE SHAPE OF THE PROBLEM IS TIMING, not decoding. The speaker consumes
 * exactly 48000 frames a second and will not wait; if the decoder is late by
 * one buffer the ring empties and the listener hears a gap. So the player
 * never decodes "when it needs samples" -- it keeps a queue AHEAD of the
 * speaker and tops it up on every idle tick, whether or not the UI did
 * anything. Under emulation the decode is slower than on the host, and the
 * queue is what absorbs the difference.
 *
 * The audio device runs at a fixed 48000; most music is 44100. Everything is
 * resampled on the way out (user/audio/resample.c), because playing 44100
 * samples into a 48000 device makes the song 9% fast and a semitone sharp,
 * which sounds like a different performance rather than a bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "mp3.h"
#include "resample.h"

#define BAR_H      52.0f
#define MUSIC_SUB  "/Music/"
#define LIST_MAX   128
#define NAME_MAX_  96

/* How far ahead of the speaker to stay, in frames at the device rate. About
 * a third of a second: long enough to ride out a slow decode or a busy
 * compositor frame, short enough that pause and track changes feel immediate. */
#define QUEUE_AHEAD 16000

static char  g_dir[256];
static char  g_names[LIST_MAX][NAME_MAX_];
static int   g_count, g_index = -1;

static uint8_t   *g_file;          /* the whole track, in memory */
static size_t     g_size, g_pos;   /* read cursor into it        */
static Mp3Decoder g_dec;
static Resampler  g_rs;
static bool       g_open;          /* the audio device is ours   */
static bool       g_playing;
static bool       g_eof;

static uint32_t g_dev_rate = 48000;
static int      g_src_rate, g_src_ch;
static uint64_t g_frames_out;      /* device frames handed over  */
static uint64_t g_frames_dec;      /* source frames decoded      */
static char     g_status[192], g_title[160], g_time[64];
static float    g_scroll;          /* playlist scroll position */

/* --- the playlist --------------------------------------------------------- */

static bool is_mp3(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot || strlen(n) >= NAME_MAX_) return false;
    return (dot[1] == 'm' || dot[1] == 'M') && (dot[2] == 'p' || dot[2] == 'P')
        && dot[3] == '3' && dot[4] == 0;
}

static void scan(const char *dir)
{
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    g_count = 0;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_count < LIST_MAX) {
        if (e->d_name[0] == '.' || !is_mp3(e->d_name)) continue;
        snprintf(g_names[g_count++], NAME_MAX_, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 1; i < g_count; i++) {          /* by name, like the folder */
        char key[NAME_MAX_];
        snprintf(key, sizeof key, "%s", g_names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(g_names[j], key) > 0) {
            memcpy(g_names[j + 1], g_names[j], NAME_MAX_);
            j--;
        }
        snprintf(g_names[j + 1], NAME_MAX_, "%s", key);
    }
}

/* --- playback ------------------------------------------------------------- */

static void stop_audio(void)
{
    if (g_open) { embk_audio_close(); g_open = false; }
    g_playing = false;
}

static bool load(int i)
{
    if (i < 0 || i >= g_count) return false;
    stop_audio();
    free(g_file);
    g_file = NULL;
    g_size = g_pos = 0;
    g_eof = false;
    g_frames_out = g_frames_dec = 0;
    g_index = i;

    char path[400];
    snprintf(path, sizeof path, "%s%s", g_dir, g_names[i]);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(g_status, sizeof g_status, "cannot open %s", g_names[i]); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return false; }

    g_file = malloc((size_t)sz);
    if (!g_file) { fclose(f); snprintf(g_status, sizeof g_status, "out of memory"); return false; }
    size_t got = 0;
    while (got < (size_t)sz) {                   /* short reads are normal */
        size_t n = fread(g_file + got, 1, (size_t)sz - got, f);
        if (n == 0) break;
        got += n;
    }
    fclose(f);
    g_size = got;

    mp3_init(&g_dec);
    /* Start at the first frame, past any ID3 tag -- which on a tagged file
     * with cover art can be tens of kilobytes. */
    size_t start = mp3_skip_id3(g_file, g_size);
    long first = mp3_find_sync(g_file, g_size, start);
    g_pos = first >= 0 ? (size_t)first : start;
    g_src_rate = 0;
    snprintf(g_status, sizeof g_status, "%s", g_names[i]);
    return true;
}

/* One decode step. The caller has to distinguish three outcomes and they need
 * different responses, so this reports which rather than a bare bool:
 *   PUMP_OK    progress made, call again
 *   PUMP_FULL  the speaker's ring is full -- STOP for this tick, we are ahead
 *   PUMP_END   the track is finished
 * Collapsing FULL and END into "false" is how a player either spins on a full
 * ring or treats being ahead as the end of the song. */
enum { PUMP_END = 0, PUMP_OK = 1, PUMP_FULL = 2 };

static int pump_once(void)
{
    static int16_t pcm[MP3_MAX_SAMPLES * 2];
    static int16_t conv[MP3_MAX_SAMPLES * 4];

    if (g_pos + 4 > g_size) return PUMP_END;

    int n = 0;
    int used = mp3_decode_frame(&g_dec, g_file + g_pos, g_size - g_pos, pcm, &n);
    if (used <= 0) {
        long nxt = mp3_find_sync(g_file, g_size, g_pos + 1);
        if (nxt < 0) return PUMP_END;
        g_pos = (size_t)nxt;
        return PUMP_OK;
    }
    g_pos += (size_t)used;
    g_frames_dec += (uint64_t)n;
    if (n == 0) return PUMP_OK;                  /* cold reservoir: normal */

    if (!g_src_rate) {
        g_src_rate = g_dec.samplerate;
        g_src_ch   = g_dec.channels;
        resample_init(&g_rs, g_src_rate, (int)g_dev_rate, g_src_ch);
    }

    const int16_t *src = pcm;
    int frames = n;
    if (g_src_rate != (int)g_dev_rate) {
        frames = resample_run(&g_rs, pcm, n, conv, MP3_MAX_SAMPLES * 2);
        src = conv;
    }

    /* The device is stereo; a mono track is doubled here rather than in the
     * decoder, which should not have to know what the speaker wants. */
    static int16_t st[MP3_MAX_SAMPLES * 4];
    if (g_src_ch == 1) {
        for (int i = 0; i < frames; i++) { st[i * 2] = st[i * 2 + 1] = src[i]; }
        src = st;
    }

    int off = 0;
    while (off < frames) {
        int took = embk_audio_write(src + off * 2, (uint32_t)(frames - off));
        if (took < 0) return PUMP_END;
        if (took == 0) {
            /* The ring is full, which is SUCCESS -- it means the decoder is
             * ahead of the speaker, which is the whole objective. The frames
             * we could not hand over are dropped rather than held, because
             * holding them would need a second queue in front of the device's
             * own; being one frame ahead is close enough that the ring never
             * empties, and the ring IS the buffer. */
            break;
        }
        off += took;
        g_frames_out += (uint64_t)took;
    }
    return off < frames ? PUMP_FULL : PUMP_OK;
}

static void pump(void)
{
    if (!g_playing || !g_file) return;

    /* Bounded work per tick, and stop the moment the speaker is ahead of us.
     * Decoding until the ring fills would hold the UI for however long that
     * takes; decoding exactly one frame per tick would fall behind whenever a
     * frame is expensive. The ring reporting FULL is the natural stopping
     * point -- it means the queue is deep enough for now. */
    for (int i = 0; i < 32; i++) {
        int st = pump_once();
        if (st == PUMP_END)  { g_eof = true; break; }
        if (st == PUMP_FULL) break;
    }

    /* REDRAW ONLY WHEN THE DISPLAY WOULD CHANGE.
     *
     * This asked for a frame on every idle tick, which meant the compositor
     * repainted the window continuously while the decoder was trying to stay
     * ahead of the speaker -- on one emulated core, competing for exactly the
     * thing playback needs. The result was a one-second dropout early in the
     * track, visible in the capture as two consecutive half-seconds at
     * silence.
     *
     * The only thing on screen that changes while playing is the elapsed
     * time, once a second. So that is how often it is asked for. */
    unsigned secs = g_src_rate ? (unsigned)(g_frames_dec / (uint64_t)g_src_rate) : 0;
    static unsigned last_secs = (unsigned)-1;
    if (secs != last_secs) { last_secs = secs; em_request_frame(); }

    if (g_eof) {
        for (int i = 0; i < 200 && !embk_audio_drained(); i++) embk_sleep_ms(2);
        stop_audio();
        /* Move to the next track, which is what a player does at the end of
         * one -- stopping dead would make an album a series of clicks. */
        if (g_index + 1 < g_count && load(g_index + 1)) {
            if (embk_audio_open() >= 0) { g_open = true; g_playing = true; g_eof = false; }
        }
        em_request_frame();            /* the track name changed */
    }
}

static void toggle(void)
{
    if (!g_file) return;
    if (g_playing) { stop_audio(); return; }
    if (!g_open) {
        if (embk_audio_open() < 0) {
            snprintf(g_status, sizeof g_status,
                     "the speaker is busy (another program has it)");
            return;
        }
        g_open = true;
    }
    g_playing = true;
}

static void skip(int delta)
{
    int n = g_count ? (g_index + delta + g_count) % g_count : -1;
    bool was = g_playing;
    if (n >= 0 && load(n) && was) {
        if (embk_audio_open() >= 0) { g_open = true; g_playing = true; }
    }
}

/* --- the view ------------------------------------------------------------- */

static void labels(void)
{
    snprintf(g_title, sizeof g_title, "%s",
             g_index >= 0 ? g_names[g_index] : "Music");
    unsigned secs = g_src_rate ? (unsigned)(g_frames_dec / (uint64_t)g_src_rate) : 0;
    snprintf(g_time, sizeof g_time, "%u:%02u", secs / 60, secs % 60);
    if (g_src_rate)
        snprintf(g_status, sizeof g_status, "%d Hz %s  ·  %s  ·  %llu frames",
                 g_src_rate, g_src_ch == 1 ? "mono" : "stereo",
                 g_src_rate == (int)g_dev_rate ? "direct" : "resampled to 48 kHz",
                 (unsigned long long)g_dec.frames);
}

static void MusicView(void)
{
    labels();

    Window("Music", .corner = 14, .clip = 1) {
        AppBar(g_title) {
            if (Button(g_playing ? "Pause" : "Play").primary().clicked()) toggle();
        }

        VStack(.grow = 1, .padding = 14, .spacing = 10, .background = T.bg) {
            HStack(.spacing = 8) {
                if (Button("<<").ghost().clicked()) skip(-1);
                Text(g_time).title();
                if (Button(">>").ghost().clicked()) skip(+1);
                Spacer();
                Text(g_status).caption().secondary();
            }

            /* An EXPLICIT height. ScrollView takes the viewport size it
             * should scroll within, and passing 0 gave it nothing to show --
             * the list was there and measured zero pixels tall, which looks
             * exactly like an empty folder. */
            ScrollView(&g_scroll, em_viewport_height() - BAR_H - 90.0f) {
                VStack(.spacing = 2) {
                    for (int i = 0; i < g_count; i++) {
                        /* Clicking a track PLAYS it -- a list where selecting
                         * an item only selects it needs a second gesture
                         * nobody wants to make. */
                        if (Button(g_names[i]).ghost().clicked()) {
                            if (load(i) && embk_audio_open() >= 0) {
                                g_open = true;
                                g_playing = true;
                            }
                        }
                    }
                    if (g_count == 0)
                        Text("No .mp3 files in this folder").caption().secondary();
                }
            }
        }
    }
}

static EmApp g_spec = {
    .title  = "Music",
    .size   = { 720, 520 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = MusicView,
};

int main(int argc, char **argv)
{
    uint32_t r = embk_audio_rate();
    if ((int)r > 0) g_dev_rate = r;

    if (argc > 1) {
        /* A single file: play it, and make its folder the playlist. */
        const char *slash = strrchr(argv[1], '/');
        char dir[256];
        snprintf(dir, sizeof dir, "%.*s",
                 slash ? (int)(slash - argv[1] + 1) : 2, slash ? argv[1] : "./");
        scan(dir);
        for (int i = 0; i < g_count; i++)
            if (slash && strcmp(g_names[i], slash + 1) == 0) { load(i); break; }
        if (g_index < 0 && g_count) load(0);
    } else {
        const char *home = getenv("HOME");
        char dir[256];
        snprintf(dir, sizeof dir, "%s" MUSIC_SUB, (home && home[0]) ? home : "/home");
        scan(dir);
        if (!g_count) {
            /* Same discovery the picture viewer does, and for the same
             * reason: launched from the dock there is no argument and no
             * environment worth trusting. */
            DIR *d = opendir("/home");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL && !g_count) {
                    if (e->d_name[0] == '.') continue;
                    snprintf(dir, sizeof dir, "/home/%.200s" MUSIC_SUB, e->d_name);
                    scan(dir);
                }
                closedir(d);
            }
        }
        if (g_count) load(0);
    }

    /* START PLAYING. A music player that opens showing a track and waits to
     * be told to play it is asking a question nobody wanted asked -- you
     * opened the music player. It also makes the app testable: the harness can
     * launch it and measure what came out of the speaker, where a paused
     * player looks identical to a broken one. */
    if (g_index >= 0 && embk_audio_open() >= 0) {
        g_open = true;
        g_playing = true;
    }

    em_set_idle_hook(pump);
    return em_app_run(&g_spec);
}
