#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* Breath of the Wild 1.6.0: no game-code patches yet. The game runs as the original code, so its frame pacing, output
   size and render size are the shipping ones (30 fps, 1600x900 handheld). Every patch TOTK has (60 FPS, real-time frame
   step, 2K output, aspect) needs its own sites found in this game's main and guarded like TOTK's. */
/* Breath of the Wild's game logic takes a fixed step per frame and asks for present interval 2 (30 fps): it needs the original pacing
   (interval / 60 s), not the real-time pacing TOTK's 60 FPS patch relies on. With real-time pacing the game ran at twice its speed
   (and its sound events twice as often as the audio renderer plays them). TOTK_REALTIME=1 brings the other pacing back. */
__attribute__((constructor)) static void botw_defaults(void) { setenv("TOTK_REALTIME", "0", 0); }

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

/* Bring-up aid (TOTK_BOTW_OBS=1): observe the agl allocator's "ring full" path (main+0x1296cb0, whose first instruction is `mov w2,#1`):
   the instruction becomes a brk; the SIGTRAP handler prints what the game decided on, emulates the instruction and goes on. */
#define OBS_OFFSET 0x1296cb0ull
static uint8_t *obs_site;
void runtime_trace_guest(uint64_t fp, uint64_t lr);
uint32_t runtime_thread_handle_self(void);
int runtime_botw_trap(ucontext_t *u) {
    if (!obs_site || u->uc_mcontext->__ss.__pc != (uintptr_t)obs_site) return 0;
    uint64_t *x = u->uc_mcontext->__ss.__x;
    fprintf(stderr, "[botw-obs] ring-full path reached on guest thread %#x\n", runtime_thread_handle_self());
    for (int i = 0; i < 29; i += 4) fprintf(stderr, "[botw-obs] x%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)x[i], (unsigned long long)x[i + 1],
                                            (unsigned long long)(i + 2 < 29 ? x[i + 2] : 0), (unsigned long long)(i + 3 < 29 ? x[i + 3] : 0));
    const uint64_t *o = (const uint64_t *)(uintptr_t)(x[21] + 0xa40);   /* the allocator object: lists, counters, sizes */
    for (int i = 0; i < 32; i += 2) fprintf(stderr, "[botw-obs] obj+%#x %016llx %016llx\n", 0xa40 + 8 * i, (unsigned long long)o[i], (unsigned long long)o[i + 1]);
    runtime_trace_guest(u->uc_mcontext->__ss.__fp, u->uc_mcontext->__ss.__lr);
    x[2] = 1;                                   /* mov w2, #1 */
    u->uc_mcontext->__ss.__pc += 4;
    return 1;
}

void runtime_game_patches(Module *m) {
    const char *poll = getenv("TOTK_BOTW_POLL");
    if (poll && *poll) {
        poll_base = m->base;
        for (char *copy = strdup(poll), *tok = strtok(copy, ","); tok && poll_count < 8; tok = strtok(NULL, ","))
            poll_offsets[poll_count++] = strtoull(tok, NULL, 16);
        pthread_t t; pthread_create(&t, NULL, poll_thread, NULL); pthread_detach(t);
    }
    if (getenv("TOTK_BOTW_OBS")) {
        uint32_t *site = (uint32_t *)(void *)(m->base + OBS_OFFSET);
        if (*site != 0x320003e2u) fprintf(stderr, "[botw-obs] site %#llx has %#x, not mov w2,#1: observer off\n", (unsigned long long)OBS_OFFSET, *site);
        else { *site = 0xd4200020u; obs_site = (uint8_t *)site; fprintf(stderr, "[botw-obs] observing main+%#llx\n", (unsigned long long)OBS_OFFSET); }
    }
    fprintf(stderr, "[totk] %s: no game-code patches\n", TOTK_GAME_VERSION);
}
/* NVN API 53.311, the version its SDK was built against and the game checks at start (main+0xe55300, 0xe55310). */
int runtime_nvn_api_version(int minor) { return minor ? 311 : 53; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
