// ui_audio.c — Subtle programmatic UI sound effects (no audio files)
#include "vtree.h"
#include <stdlib.h>
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define UI_AUDIO_SR 22050

static SDL_AudioDeviceID ui_audio_dev = 0;

void ui_audio_open(void) {
    if (ui_audio_dev) return;
    SDL_AudioSpec want = {0};
    want.freq     = UI_AUDIO_SR;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    ui_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (ui_audio_dev) SDL_PauseAudioDevice(ui_audio_dev, 0);
}

void ui_audio_close(void) {
    if (ui_audio_dev) { SDL_CloseAudioDevice(ui_audio_dev); ui_audio_dev = 0; }
}

// Square wave — retro/crunchy, used for navigate/mark/tab ticks
static void ui_play_tone(int f0, int f1, int ms, float vol) {
    if (!ui_audio_dev) return;
    int n = UI_AUDIO_SR * ms / 1000;
    if (n <= 0) return;
    Sint16 *buf = (Sint16 *)malloc((size_t)n * sizeof(Sint16));
    if (!buf) return;
    double phase = 0.0;
    for (int i = 0; i < n; i++) {
        double t    = (double)i / n;
        double freq = f0 + t * (f1 - f0);
        if (freq < 1.0) freq = 1.0;
        phase += freq / UI_AUDIO_SR;
        double frac = phase - (int)phase;
        double env  = 1.0 - t * 0.85;
        Sint16 s    = (Sint16)(32767.0 * vol * env * (frac < 0.5 ? 1.0 : -1.0));
        buf[i] = s;
    }
    SDL_QueueAudio(ui_audio_dev, buf, (Uint32)n * sizeof(Sint16));
    free(buf);
}

// Sine wave — clean/smooth, used for confirm/back tones
static void ui_play_sine(int f0, int f1, int ms, float vol) {
    if (!ui_audio_dev) return;
    int n = UI_AUDIO_SR * ms / 1000;
    if (n <= 0) return;
    Sint16 *buf = (Sint16 *)malloc((size_t)n * sizeof(Sint16));
    if (!buf) return;
    double phase = 0.0;
    for (int i = 0; i < n; i++) {
        double t    = (double)i / n;
        double freq = f0 + t * (f1 - f0);
        if (freq < 1.0) freq = 1.0;
        phase += freq / UI_AUDIO_SR;
        double env  = 1.0 - t * 0.85;
        Sint16 s    = (Sint16)(32767.0 * vol * env * sin(2.0 * M_PI * phase));
        buf[i] = s;
    }
    SDL_QueueAudio(ui_audio_dev, buf, (Uint32)n * sizeof(Sint16));
    free(buf);
}

void ui_sound_navigate(void) {
    if (!cfg.ui_sounds) return;
    ui_play_tone(600, 600, 12, 0.05f);
}

void ui_sound_confirm(void) {
    if (!cfg.ui_sounds) return;
    ui_play_sine(440, 880, 50, 0.12f);
}

void ui_sound_back(void) {
    if (!cfg.ui_sounds) return;
    ui_play_sine(600, 300, 45, 0.10f);
}

void ui_sound_mark(void) {
    if (!cfg.ui_sounds) return;
    ui_play_tone(550, 770, 30, 0.11f);
}

void ui_sound_tab(void) {
    if (!cfg.ui_sounds) return;
    ui_play_tone(440, 550, 22, 0.09f);
}

void ui_sound_osk_type(void) {
    if (!cfg.ui_sounds) return;
    ui_play_tone(900, 900, 9, 0.05f);
}

void ui_sound_osk_bksp(void) {
    if (!cfg.ui_sounds) return;
    ui_play_tone(600, 600, 9, 0.04f);
}
