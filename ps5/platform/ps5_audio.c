#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apu/apu_xaudio2.h"

extern int sceAudioOutInit(void);
extern int sceAudioOutOpen(int user, int type, int index, uint32_t frames,
                           uint32_t rate, uint32_t format);
extern int sceAudioOutOutput(int handle, const void *samples);
extern int sceAudioOutClose(int handle);
extern int sceKernelDebugOutText(int channel, const char *text);

enum {
    SYSTEM_USER = 0xff, MAIN_PORT = 0, S16_STEREO = 1,
    RATE = 48000, FRAMES = 256, BLOCKS = 16, CAPACITY = FRAMES * BLOCKS,
    PREROLL = 4 * FRAMES
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int handle = -1, active, started, draining;
static unsigned queued, head, in_flight, underruns;
static uint64_t played;
static int16_t ring[CAPACITY][2];

static void audio_status(const char *event, int code)
{
    char message[112];
    snprintf(message, sizeof message, "[PS5AUDIO] %s (%08x)\n", event, (unsigned)code);
    sceKernelDebugOutText(0, message);
}

static void *output_thread(void *unused)
{
    (void)unused;
    uint64_t next_report = RATE;
    unsigned mixed_blocks = 0;
    pthread_mutex_lock(&lock);
    while (active) {
        unsigned minimum = draining ? 1 : started ? FRAMES : PREROLL;
        if (queued < minimum) {
            if (started && !draining)
                ++underruns;
            started = 0;
            if (queued == 0)
                draining = 0;
            pthread_cond_wait(&ready, &lock);
            continue;
        }

        int16_t block[FRAMES][2] = {{0}};
        unsigned count = queued < FRAMES ? queued : FRAMES;
        unsigned first = CAPACITY - head;
        if (first > count)
            first = count;
        memcpy(block, ring + head, first * sizeof ring[0]);
        memcpy(block + first, ring, (count - first) * sizeof ring[0]);
        head = (head + count) % CAPACITY;
        queued -= count;
        in_flight = count;
        started = 1;
        pthread_mutex_unlock(&lock);

        int result = sceAudioOutOutput(handle, block);
        for (unsigned i = 0; i < count; ++i) {
            if (block[i][0] || block[i][1]) {
                ++mixed_blocks;
                break;
            }
        }

        pthread_mutex_lock(&lock);
        in_flight = 0;
        if (result < 0) {
            active = 0;
            audio_status("output failed", result);
            break;
        }
        played += count;
        if (played >= next_report) {
            char message[144];
            snprintf(message, sizeof message,
                     "[PS5AUDIO] frames=%llu audible_blocks=%u queued=%u underruns=%u\n",
                     (unsigned long long)played, mixed_blocks, queued, underruns);
            sceKernelDebugOutText(0, message);
            next_report = played + 5 * RATE;
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int xa2_init(void)
{
    if (handle >= 0)
        return xa2_is_active();
    int init = sceAudioOutInit();
    handle = sceAudioOutOpen(SYSTEM_USER, MAIN_PORT, 0, FRAMES, RATE, S16_STEREO);
    if (handle < 0) {
        audio_status("init result", init);
        audio_status("port open failed", handle);
        return 0;
    }
    pthread_mutex_lock(&lock);
    queued = head = in_flight = underruns = 0;
    played = 0;
    started = draining = 0;
    active = 1;
    int error = pthread_create(&thread, NULL, output_thread, NULL);
    if (error) {
        active = 0;
        sceAudioOutClose(handle);
        handle = -1;
        audio_status("worker start failed", error);
    } else {
        audio_status("48 kHz stereo port opened", handle);
    }
    pthread_mutex_unlock(&lock);
    return error == 0;
}

void xa2_shutdown(void)
{
    if (handle < 0)
        return;
    pthread_mutex_lock(&lock);
    active = 0;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    sceAudioOutClose(handle);
    handle = -1;
}

int xa2_is_active(void)
{
    pthread_mutex_lock(&lock);
    int value = active;
    pthread_mutex_unlock(&lock);
    return value;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!samples || num_samples <= 0)
        return 0;
    pthread_mutex_lock(&lock);
    int accepted = active && (unsigned)num_samples <= CAPACITY - queued;
    if (accepted) {
        unsigned tail = (head + queued) % CAPACITY;
        unsigned first = CAPACITY - tail;
        if (first > (unsigned)num_samples)
            first = (unsigned)num_samples;
        memcpy(ring + tail, samples, first * sizeof ring[0]);
        memcpy(ring, samples + first * 2, ((unsigned)num_samples - first) * sizeof ring[0]);
        queued += (unsigned)num_samples;
        pthread_cond_signal(&ready);
    }
    pthread_mutex_unlock(&lock);
    return accepted;
}

int xa2_get_buffer_size(void) { return FRAMES; }
int xa2_get_buffer_count(void) { return BLOCKS; }

int xa2_queued_samples(void)
{
    pthread_mutex_lock(&lock);
    int value = (int)(queued + in_flight);
    pthread_mutex_unlock(&lock);
    return value;
}

void xa2_expect_drain(void)
{
    pthread_mutex_lock(&lock);
    draining = 1;
    pthread_cond_signal(&ready);
    pthread_mutex_unlock(&lock);
}

unsigned xa2_underruns(void)
{
    pthread_mutex_lock(&lock);
    unsigned value = underruns;
    pthread_mutex_unlock(&lock);
    return value;
}

uint64_t xa2_samples_played(void)
{
    pthread_mutex_lock(&lock);
    uint64_t value = played;
    pthread_mutex_unlock(&lock);
    return value;
}
