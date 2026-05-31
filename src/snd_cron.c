/* snd_cron.c — Cronopio sound seam for Quake (replaces sound/src/snd_sdl.c).
 *
 * We keep Quake's own software mixer (snd_dma.c + snd_mix.c + snd_mem.c): it
 * loads the WAVs from the pak, spatialises channels, and paints a 16-bit stereo
 * stream into the shm ring buffer. This file is only the platform DMA layer
 * snd_sdl.c used to be — but instead of an SDL callback pulling from shm, we
 * PUSH the freshly-painted frames into the host audio ring via cron_stream
 * (22050 Hz, interleaved stereo S16 — exactly Quake's mix format here).
 *
 * The contract snd_dma.c expects (see GetSoundtime / S_Update_):
 *   - SNDDMA_GetDMAPos() returns the PLAY cursor in mono samples within the
 *     ring; snd_dma derives a monotonic soundtime from it (tracking wraps) and
 *     mixes _snd_mixahead seconds ahead of it.
 *   - S_PaintChannels() fills shm->buffer for frames [paintedtime, endtime).
 *   - SNDDMA_Submit() is called right after; we forward the new frames to
 *     cron_stream.
 *
 * We never read the host ring's capacity. Instead we recover how many frames
 * the host CONSUMED from the change in cron_stream_free() between calls,
 * compensating for the frames we pushed ourselves:
 *     drain = (free_now - free_prev) + pushed_since_last_check
 * (free falls by our pushes, rises by the host's drains). consumed accumulates
 * drain; the play cursor is (consumed * channels) wrapped to the ring. */

#include "quakedef.h"
#include "sound.h"
#include "console.h"

#include <cronopio.h>

/* shm->samples (mono) — a power of two so snd_mix's `& (fullsamples-1)` wrap is
 * valid. 16384 mono = 8192 stereo frames (~0.37 s at 22050 Hz), comfortably
 * more than _snd_mixahead (0.1 s) so the ring never overruns the play cursor. */
#define CRON_DMA_SAMPLES 16384

static byte      dma_buffer[CRON_DMA_SAMPLES * 2];   /* 16-bit stereo */
static int64_t   s_consumed;        /* frames the host has played (monotonic)  */
static int64_t   s_pushed_painted;  /* abs frame index pushed to cron_stream    */
static int32_t   s_free_prev;       /* cron_stream_free() at the last GetDMAPos  */
static int32_t   s_pushed_since;     /* frames pushed since the last GetDMAPos    */

qboolean SNDDMA_Init(dma_t* dma) {
    Q_memset((void*)dma, 0, sizeof(dma_t));
    shm = dma;

    shm->splitbuffer = false;
    shm->channels = 2;
    shm->samplebits = 16;
    shm->signed8 = 0;
    shm->speed = CRON_AUDIO_HZ;       /* host plays the stream at 22050 Hz */
    shm->samples = CRON_DMA_SAMPLES;  /* mono samples in the ring */
    shm->samplepos = 0;
    shm->submission_chunk = 1;
    shm->buffer = dma_buffer;

    s_consumed = 0;
    s_pushed_painted = 0;
    s_pushed_since = 0;
    s_free_prev = cron_stream_free();

    Con_Printf("Cronopio audio: %d Hz, %d-bit, %d ch, %d-frame ring\n",
               shm->speed, shm->samplebits, shm->channels,
               shm->samples / shm->channels);
    return true;
}

i32 SNDDMA_GetDMAPos(void) {
    if (!shm) {
        return 0;
    }
    int32_t free_now = cron_stream_free();
    /* Frames the host drained since last time. free drops by our pushes and
     * rises by host playback, so add back what we pushed. */
    int32_t drain = (free_now - s_free_prev) + s_pushed_since;
    if (drain < 0) {
        drain = 0;   /* defensive: never run the cursor backwards */
    }
    s_consumed += drain;
    s_free_prev = free_now;
    s_pushed_since = 0;

    return (i32)((s_consumed * shm->channels) % shm->samples);
}

void SNDDMA_Submit(void) {
    if (!shm || !shm->buffer) {
        return;
    }
    int fullframes = shm->samples / shm->channels;

    /* Frames painted but not yet handed to the host. */
    int want = paintedtime - (int)s_pushed_painted;
    if (want <= 0) {
        return;
    }
    /* Safety: never queue more than the ring holds (e.g. after a reset jump). */
    if (want > fullframes) {
        s_pushed_painted = paintedtime - fullframes;
        want = fullframes;
    }
    int freeframes = cron_stream_free();
    if (want > freeframes) {
        want = freeframes;   /* back-pressure — normally free is plentiful */
    }
    if (want <= 0) {
        return;
    }

    const int16_t* buf = (const int16_t*)shm->buffer;
    int start = (int)(s_pushed_painted % fullframes);
    int first = fullframes - start;
    if (first > want) {
        first = want;
    }
    int pushed = cron_stream(buf + (size_t)start * 2, first);
    if (pushed == first && want > first) {
        pushed += cron_stream(buf, want - first);   /* ring wrap */
    }

    s_pushed_painted += pushed;
    s_pushed_since   += pushed;
}

void SNDDMA_Shutdown(void) {
    shm = NULL;
}

/* The engine wraps mixing in lock/unlock and pause hooks; with a push model
 * there is no shared callback buffer to guard, so these are no-ops. */
void SNDDMA_LockBuffer(void)   { }
void SNDDMA_BlockSound(void)   { }
void SNDDMA_UnblockSound(void) { }

/* ---- dropped subsystems (kept as stubs) -------------------------------- */

/* snd_dma.c's S_Init/S_Shutdown call these; the codec layer (flac/mp3/vorbis +
 * the WAV streamer) is only needed for background music, which we don't ship. */
void S_CodecInit(void)     { }
void S_CodecShutdown(void) { }

/* Background music. Quake's soundtrack is recorded audio (CD tracks); modern
 * data ships it as music/track%02d.ogg. We read the requested track straight
 * from the cart's pak/ROM and hand the ogg bytes to the host, which decodes and
 * streams it (cron_music). No music in the pak -> silent, no error. */

static int   s_track;        /* current track number, 0 = none */

static void bgm_load(int track, int looping) {
    char name[64];
    if (track < 2) {            /* track 1 is the data track — never music */
        cron_ogg_stop();
        s_track = 0;
        return;
    }
    snprintf(name, sizeof(name), "music/track%02d.ogg", track);
    byte* data = COM_LoadTempFile(name);   /* NULL if absent; host copies bytes */
    if (!data) {
        cron_ogg_stop();
        s_track = 0;
        return;
    }
    cron_ogg_play(data, com_filesize, looping ? 1 : 0);
    cron_ogg_volume((int)(bgmvolume.value * 256.0f));
    s_track = track;
}

i32  BGMusic_Init(void) { return 1; }

void BGMusic_Play(byte track, qboolean looping) { bgm_load((int)track, looping); }

void BGMusic_Stop(void) { cron_ogg_stop(); s_track = 0; }

/* Pause/Resume fire on menu/console toggles; keep the music playing under them
 * (no host-side pause yet — tracking position would need a new syscall). */
void BGMusic_Pause(void)  { }
void BGMusic_Resume(void) { }

void BGMusic_Shutdown(void) { cron_ogg_stop(); }

/* Push the menu's bgmvolume slider to the host each tick (cheap). */
void BGMusic_Update(void) {
    if (s_track) {
        cron_ogg_volume((int)(bgmvolume.value * 256.0f));
    }
}
