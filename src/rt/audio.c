/* Host audio backend: a 44.1 kHz stereo s16 mixing ring drained by winmm waveOut.
 *
 * The PSP mixes up to 8 sceAudio output channels in hardware; each guest channel that calls
 * sceAudioOutput*Blocking pushes its buffer here. Channels keep their own write cursor over a
 * shared int32 accumulator ring, so concurrently-playing channels overlap (sum) instead of
 * interleaving in time. A feeder thread clamps accumulated samples to s16 and hands fixed-size
 * blocks to waveOut; if the game outruns real time the push clamps (drops), if it falls behind
 * the feeder emits silence. This favours "keeps running, sounds right enough" over hi-fi sync —
 * the scheduler's virtual-time pacing of the *Blocking calls stays the authority on game speed.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define RING_FRAMES  (1 << 16)            /* 64k frames ≈ 1.5 s of headroom */
#define RING_MASK    (RING_FRAMES - 1)
#define BLOCK_FRAMES 1024                 /* waveOut block: ~23 ms */
#define NUM_BLOCKS   4                    /* device queue ~93 ms (keeps voice near subtitles) */

static int32_t  s_accL[RING_FRAMES], s_accR[RING_FRAMES];
static uint64_t s_play = 0;               /* feeder (read) cursor, frames */
static uint64_t s_chw[8];                 /* per-channel write cursors, frames */
static CRITICAL_SECTION s_lock;
static HWAVEOUT s_wo = NULL;
static WAVEHDR  s_hdr[NUM_BLOCKS];
static int16_t  s_blk[NUM_BLOCKS][BLOCK_FRAMES * 2];
static HANDLE   s_event = NULL, s_thread = NULL;
static volatile int s_run = 0;
static int s_inited = 0, s_ok = 0;

static int16_t clamp16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

static DWORD WINAPI feeder(LPVOID arg) {
    (void)arg;
    int next = 0;
    while (s_run) {
        WaitForSingleObject(s_event, 100);
        while (s_run) {
            WAVEHDR *h = &s_hdr[next];
            if (!(h->dwFlags & WHDR_DONE)) break;          /* block still queued */
            int16_t *out = s_blk[next];
            EnterCriticalSection(&s_lock);
            for (int i = 0; i < BLOCK_FRAMES; i++) {
                uint32_t idx = (uint32_t)((s_play + (uint64_t)i) & RING_MASK);
                out[i * 2 + 0] = clamp16(s_accL[idx]);
                out[i * 2 + 1] = clamp16(s_accR[idx]);
                s_accL[idx] = s_accR[idx] = 0;
            }
            s_play += BLOCK_FRAMES;
            LeaveCriticalSection(&s_lock);
            h->dwFlags &= ~WHDR_DONE;
            waveOutWrite(s_wo, h, sizeof(*h));
            next = (next + 1) % NUM_BLOCKS;
        }
    }
    return 0;
}

int sr_audio_init(void) {
    if (s_inited) return s_ok;
    s_inited = 1;
    if (getenv("SR_NOAUDIO")) return s_ok = 0;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100 * 4;
    s_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&s_wo, WAVE_MAPPER, &wf, (DWORD_PTR)s_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: waveOutOpen failed (silent run)\n");
        return s_ok = 0;
    }
    InitializeCriticalSection(&s_lock);
    for (int i = 0; i < NUM_BLOCKS; i++) {
        s_hdr[i].lpData = (LPSTR)s_blk[i];
        s_hdr[i].dwBufferLength = BLOCK_FRAMES * 4;
        waveOutPrepareHeader(s_wo, &s_hdr[i], sizeof(WAVEHDR));
        s_hdr[i].dwFlags |= WHDR_DONE;                     /* available immediately */
    }
    s_run = 1;
    s_thread = CreateThread(NULL, 0, feeder, NULL, 0, NULL);
    SetEvent(s_event);
    fprintf(stderr, "audio: waveOut 44100 Hz stereo open\n");
    return s_ok = 1;
}

/* Mix nframes of interleaved stereo s16 into the channel's slice of the ring.
 * volL/volR are 0..0x8000 (PSP panned-output volumes). */
void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR) {
    if (!sr_audio_init() || nframes <= 0) return;
    if (ch < 0) ch = 0;
    ch &= 7;
    EnterCriticalSection(&s_lock);
    uint64_t w = s_chw[ch];
    if (w < s_play) w = s_play;                            /* channel fell behind: snap to now */
    if (w + (uint64_t)nframes > s_play + RING_FRAMES)      /* too far ahead: clamp (drop tail) */
        nframes = (int)(s_play + RING_FRAMES - w);
    for (int i = 0; i < nframes; i++) {
        uint32_t idx = (uint32_t)((w + (uint64_t)i) & RING_MASK);
        s_accL[idx] += ((int32_t)lr[i * 2 + 0] * volL) >> 15;
        s_accR[idx] += ((int32_t)lr[i * 2 + 1] * volR) >> 15;
    }
    s_chw[ch] = w + (uint64_t)(nframes > 0 ? nframes : 0);
    LeaveCriticalSection(&s_lock);
}

/* Frames this channel has queued ahead of the playhead (-1: no host audio). The blocking
 * output calls pace against this, like real hardware, so drift self-corrects instead of
 * accumulating (an open-loop sleep of the buffer duration always runs slightly slow). */
int sr_audio_queued(int ch) {
    if (!s_ok) return -1;
    ch &= 7;
    EnterCriticalSection(&s_lock);
    uint64_t w = s_chw[ch], p = s_play;
    LeaveCriticalSection(&s_lock);
    return w > p ? (int)(w - p) : 0;
}
