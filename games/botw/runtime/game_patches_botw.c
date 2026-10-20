#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* Breath of the Wild 1.6.0: no game-code patches yet. The game runs as the original code, so its frame pacing, output
   size and render size are the shipping ones (30 fps, 1600x900 handheld). Every patch TOTK has (60 FPS, real-time frame
   step, 2K output, aspect) needs its own sites found in this game's main and guarded like TOTK's. */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }

/* Bring-up aid: TOTK_BOTW_POLL=<hex main offset>[,...] logs every change of the 8-byte word at each offset of main. */
static uint8_t *poll_base;
static uint64_t poll_offsets[8];
static unsigned poll_count;
static void *poll_thread(void *unused) {
    (void)unused;
    uint64_t last[8];
    for (unsigned i = 0; i < poll_count; i++) memcpy(&last[i], poll_base + poll_offsets[i], 8);
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        for (unsigned i = 0; i < poll_count; i++) {
            uint64_t now; memcpy(&now, poll_base + poll_offsets[i], 8);
            if (now != last[i]) {
                struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                fprintf(stderr, "[botw-poll] %.3fs main+%#llx: %#llx -> %#llx\n", (t.tv_sec - start.tv_sec) + (t.tv_nsec - start.tv_nsec) / 1e9,
                        (unsigned long long)poll_offsets[i], (unsigned long long)last[i], (unsigned long long)now);
                last[i] = now;
            }
        }
        usleep(200);
    }
    return NULL;
}

void runtime_game_patches(Module *m) {
    const char *poll = getenv("TOTK_BOTW_POLL");
    if (poll && *poll) {
        poll_base = m->base;
        for (char *copy = strdup(poll), *tok = strtok(copy, ","); tok && poll_count < 8; tok = strtok(NULL, ","))
            poll_offsets[poll_count++] = strtoull(tok, NULL, 16);
        pthread_t t; pthread_create(&t, NULL, poll_thread, NULL); pthread_detach(t);
    }
    fprintf(stderr, "[totk] %s: no game-code patches\n", TOTK_GAME_VERSION);
}
/* NVN API 53.311, the version its SDK was built against and the game checks at start (main+0xe55300, 0xe55310). */
int runtime_nvn_api_version(int minor) { return minor ? 311 : 53; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
