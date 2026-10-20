#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* Breath of the Wild 1.6.0: no game-code patches yet. The game runs as the original code, so its frame pacing, output
   size and render size are the shipping ones (30 fps, 1600x900 handheld). Every patch TOTK has (60 FPS, real-time frame
   step, 2K output, aspect) needs its own sites found in this game's main and guarded like TOTK's. */
/* Breath of the Wild's game logic takes a fixed step per frame and asks for present interval 2 (30 fps): it needs the original pacing
   (interval / 60 s), not the real-time pacing TOTK's 60 FPS patch relies on. With real-time pacing the game ran at twice its speed
   (and its sound events twice as often as the audio renderer plays them). TOTK_REALTIME=1 brings the other pacing back. */
__attribute__((constructor)) static void botw_defaults(void) { setenv("TOTK_REALTIME", "0", 0); }

int runtime_frame_step_realtime(void) { return 0; }   /* fixed step per frame: paced by our own 60 Hz clock (hle_nvn.c) */
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
#define OBS_ENTRY_OFFSET 0x1296030ull   /* the allocator function: `sub sp,sp,#0x1f0` */
static uint8_t *obs_site, *obs_entry;
typedef struct { uint32_t thread, pad; uint64_t seq, x[8], st[10]; } CallRec;
#define NREC 4096
static CallRec calls[NREC];
/* TOTK_BOTW_OBS=5: counts allocations (main+0x1296030) and frees (main+0x12b6cb0, `stp x20,x19,[sp,#-0x20]!`) per frame. */
#define OBS_FREE_OFFSET 0x12b6cb0ull
static uint8_t *obs_free;
typedef struct { uint64_t frame, ns; uint32_t thread, kind; } EvRec;
uint64_t runtime_now_ns(void);
#define NEV 65536
static EvRec evs[NEV]; static _Atomic uint64_t ev_seq;
/* TOTK_BOTW_OBS=3: also every `cmp xN,xM` of the allocator's range checks (sites below) is a brk that records the compared values. */
static const uint32_t cmp_sites[] = {0x1296afc, 0x1296b14, 0x1296b24, 0x1296b34, 0x1296b3c, 0x1296c68, 0x1296c78, 0x1296c90, 0x1296ca0, 0x1296ca8};
#define NCMP (sizeof cmp_sites / sizeof *cmp_sites)
static uint32_t cmp_orig[NCMP]; static uint8_t *cmp_addr[NCMP];
typedef struct { uint64_t seq, a, b, frame; uint32_t site, thread; } CmpRec;
uint64_t dm_nvn_frame_count(void) __attribute__((weak_import));
#define NCREC 8192
static CmpRec cmprecs[NCREC]; static _Atomic uint64_t cmp_seq;
static _Atomic uint64_t call_seq;
void runtime_trace_guest(uint64_t fp, uint64_t lr);
uint32_t runtime_thread_handle_self(void);
int runtime_botw_trap(ucontext_t *u) {
    uint64_t *x = u->uc_mcontext->__ss.__x;
    if (obs_entry && u->uc_mcontext->__ss.__pc == (uintptr_t)obs_entry) {   /* record the call, emulate `sub sp,sp,#0x1f0` */
        uint64_t n = atomic_fetch_add(&call_seq, 1); CallRec *r = &calls[n % NREC];
        r->seq = n + 1; r->thread = runtime_thread_handle_self();
        for (int i = 0; i < 8; i++) r->x[i] = x[i];
        const uint64_t *sp = (const uint64_t *)(uintptr_t)u->uc_mcontext->__ss.__sp; for (int i = 0; i < 10; i++) r->st[i] = sp[i];
        { uint64_t q = atomic_fetch_add(&ev_seq, 1); EvRec *e = &evs[q % NEV]; e->frame = dm_nvn_frame_count ? dm_nvn_frame_count() : 0; e->ns = runtime_now_ns(); e->thread = r->thread; e->kind = 1; }
        u->uc_mcontext->__ss.__sp -= 0x1f0; u->uc_mcontext->__ss.__pc += 4; return 1;
    }
    if (obs_free && u->uc_mcontext->__ss.__pc == (uintptr_t)obs_free) {   /* emulate stp x20,x19,[sp,#-0x20]! and count the free */
        uint64_t q = atomic_fetch_add(&ev_seq, 1); EvRec *e = &evs[q % NEV];
        e->frame = dm_nvn_frame_count ? dm_nvn_frame_count() : 0; e->ns = runtime_now_ns(); e->thread = runtime_thread_handle_self(); e->kind = 2;
        uint64_t sp = u->uc_mcontext->__ss.__sp - 0x20; ((uint64_t *)(uintptr_t)sp)[0] = x[20]; ((uint64_t *)(uintptr_t)sp)[1] = x[19];
        u->uc_mcontext->__ss.__sp = sp; u->uc_mcontext->__ss.__pc += 4; return 1;
    }
    for (unsigned i = 0; i < NCMP; i++) if (cmp_addr[i] && u->uc_mcontext->__ss.__pc == (uintptr_t)cmp_addr[i]) {   /* cmp xN,xM = subs xzr,xN,xM */
        uint32_t w = cmp_orig[i]; unsigned n = (w >> 5) & 31, m = (w >> 16) & 31;
        uint64_t va = x[n], vb = x[m], nz;
        __asm__ volatile("subs xzr, %1, %2\n\tmrs %0, nzcv" : "=r"(nz) : "r"(va), "r"(vb) : "cc");
        uint64_t q = atomic_fetch_add(&cmp_seq, 1); CmpRec *r = &cmprecs[q % NCREC];
        r->seq = q + 1; r->a = va; r->b = vb; r->site = cmp_sites[i]; r->thread = runtime_thread_handle_self(); r->frame = dm_nvn_frame_count ? dm_nvn_frame_count() : 0;
        if (cmp_sites[i] == 0x1296c78 && r->frame >= 1500 && getenv("TOTK_BOTW_PEAK")) {   /* peak section start per frame (thread 0x1009's pool) */
            static _Atomic uint64_t cur_frame, peak; uint64_t f = r->frame;
            if (f != atomic_load(&cur_frame)) { uint64_t old = atomic_exchange(&cur_frame, f), pk = atomic_exchange(&peak, 0);
                if (old) fprintf(stderr, "[botw-peak] frame %llu peak b=%#llx (a=%#llx)\n", (unsigned long long)old, (unsigned long long)pk, (unsigned long long)va); }
            if (vb > atomic_load(&peak)) atomic_store(&peak, vb);
        }
        u->uc_mcontext->__ss.__cpsr = (u->uc_mcontext->__ss.__cpsr & ~0xf0000000u) | (uint32_t)(nz & 0xf0000000u);
        u->uc_mcontext->__ss.__pc += 4; return 1;
    }
    if (!obs_site || u->uc_mcontext->__ss.__pc != (uintptr_t)obs_site) return 0;
    fprintf(stderr, "[botw-obs] ring-full path reached on guest thread %#x\n", runtime_thread_handle_self());
    for (int i = 0; i < 29; i += 4) fprintf(stderr, "[botw-obs] x%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)x[i], (unsigned long long)x[i + 1],
                                            (unsigned long long)(i + 2 < 29 ? x[i + 2] : 0), (unsigned long long)(i + 3 < 29 ? x[i + 3] : 0));
    const uint64_t *o = (const uint64_t *)(uintptr_t)(x[21] + 0xa40);   /* the allocator object: lists, counters, sizes */
    for (int i = 0; i < 32; i += 2) fprintf(stderr, "[botw-obs] obj+%#x %016llx %016llx\n", 0xa40 + 8 * i, (unsigned long long)o[i], (unsigned long long)o[i + 1]);
    if (obs_entry) {   /* the most recent calls on the same allocator object */
        uint64_t last = atomic_load(&call_seq), shown = 0;
        for (uint64_t k = last; k > 0 && last - k < NREC && shown < 40; k--) {
            const CallRec *r = &calls[(k - 1) % NREC]; if (r->seq != k || r->x[0] != x[21]) continue; shown++;
            fprintf(stderr, "[botw-obs] call #%llu thr=%#x  x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx x6=%llx x7=%llx st=%llx %llx %llx %llx %llx\n", (unsigned long long)k, r->thread,
                    (unsigned long long)r->x[1], (unsigned long long)r->x[2], (unsigned long long)r->x[3], (unsigned long long)r->x[4], (unsigned long long)r->x[5],
                    (unsigned long long)r->x[6], (unsigned long long)r->x[7], (unsigned long long)r->st[2], (unsigned long long)r->st[3], (unsigned long long)r->st[4],
                    (unsigned long long)r->st[6], (unsigned long long)r->st[7]);
        }
        fprintf(stderr, "[botw-obs] %llu allocator calls recorded in total\n", (unsigned long long)last);
    }
    {   /* the section changes of the last range checks (site 0x1296c78 compares the section start b with the limit a), with the frame count */
        uint64_t last = atomic_load(&cmp_seq), prev_b = ~0ull;
        for (uint64_t k = last > NCREC - 64 ? last - (NCREC - 64) : 0; k < last; k++) { const CmpRec *r = &cmprecs[k % NCREC]; if (r->seq != k + 1 || r->site != 0x1296c78) continue;
            if (r->b == prev_b) continue; prev_b = r->b;
            fprintf(stderr, "[botw-obs] frames=%llu thr=%#x  section start b=%llx limit a=%llx  (%s)\n", (unsigned long long)r->frame, r->thread, (unsigned long long)r->b,
                    (unsigned long long)r->a, r->a < r->b ? "OVER the limit" : "inside"); }
    }
    if (obs_free) {   /* bursts of allocator events: consecutive events of one thread less than 3 ms apart */
        uint64_t last = atomic_load(&ev_seq), from = last > 3000 ? last - 3000 : 0, bs = 0, be = 0; uint32_t bt = 0; unsigned na = 0, nf = 0, nb = 0; uint64_t base = evs[from % NEV].ns;
        #define FLUSH() if (bt) fprintf(stderr, "[botw-obs] burst thr=%#x  %8.3f..%8.3f ms  allocs=%u frees=%u\n", bt, (bs - base) / 1e6, (be - base) / 1e6, na, nf)
        for (uint64_t k = from; k < last; k++) { const EvRec *e = &evs[k % NEV];
            if (bt == e->thread && e->ns - be < 3000000) { be = e->ns; if (e->kind == 1) na++; else nf++; continue; }
            FLUSH(); nb++; bt = e->thread; bs = be = e->ns; na = e->kind == 1; nf = e->kind == 2; }
        FLUSH(); (void)nb;
        #undef FLUSH
    }
    if (obs_free) {   /* allocations and frees per frame and thread over the last frames */
        uint64_t last = atomic_load(&ev_seq), top = 0;
        for (uint64_t k = last > NEV ? last - NEV : 0; k < last; k++) if (evs[k % NEV].frame > top) top = evs[k % NEV].frame;
        for (uint64_t fr = top > 14 ? top - 14 : 0; fr <= top; fr++) {
            unsigned na[8] = {0}, nf[8] = {0};
            for (uint64_t k = last > NEV ? last - NEV : 0; k < last; k++) { const EvRec *e = &evs[k % NEV]; if (e->frame != fr) continue;
                unsigned slot = e->thread == 0x1009 ? 0 : e->thread == 0x1001 ? 1 : e->thread == 0x1004 ? 2 : 3; if (e->kind == 1) na[slot]++; else nf[slot]++; }
            if (na[0] + na[1] + na[2] + na[3] + nf[0] + nf[1] + nf[2] + nf[3])
                fprintf(stderr, "[botw-obs] frame %llu: allocs 1009=%u 1001=%u 1004=%u other=%u | frees 1009=%u 1001=%u 1004=%u other=%u\n", (unsigned long long)fr,
                        na[0], na[1], na[2], na[3], nf[0], nf[1], nf[2], nf[3]);
        }
    }
    runtime_trace_guest(u->uc_mcontext->__ss.__fp, u->uc_mcontext->__ss.__lr);
    x[2] = 1;                                   /* mov w2, #1 */
    u->uc_mcontext->__ss.__pc += 4;
    return 1;
}

/* The size of the agl render-target pool (transient render targets of one frame plus a persistent part) is a constant of the game's render
   budget table: main+0x1804c44 is `mov w8,#0x7300000`. The heaviest frames of the opening need a little more than the pool holds
   (the pool's allocator then takes its "out of memory" path and the game crashes), so the pool is made larger. */
#define DTA_POOL_OFFSET 0x1804c44ull
#define DTA_POOL_ORIGINAL 0x52a0e608u            /* movz w8,#0x730,lsl #16 */
static void patch_dta_pool(Module *m) {
    const char *e = getenv("TOTK_BOTW_DTA_POOL");
    uint64_t size = e ? strtoull(e, NULL, 16) : 0;   /* opt-in: a bigger pool did not stop the ring-full crash */
    if (!size || size == 0x7300000ull) return;
    if (size & 0xffffull || (size >> 16) > 0xffff) { fprintf(stderr, "[totk] TOTK_BOTW_DTA_POOL must be a multiple of 64 KiB below 4 GiB\n"); return; }
    uint32_t *site = (uint32_t *)(void *)(m->base + DTA_POOL_OFFSET);
    if (*site != DTA_POOL_ORIGINAL) { fprintf(stderr, "[totk] render-target pool site has %#x: pool size left as it is\n", *site); return; }
    *site = 0x52a00000u | (uint32_t)((size >> 16) << 5) | 8u;       /* movz w8,#(size>>16),lsl #16 */
    fprintf(stderr, "[totk] render-target pool: %#llx bytes instead of 0x7300000\n", (unsigned long long)size);
}

void runtime_game_patches(Module *m) {
    const char *poll = getenv("TOTK_BOTW_POLL");
    if (poll && *poll) {
        poll_base = m->base;
        for (char *copy = strdup(poll), *tok = strtok(copy, ","); tok && poll_count < 8; tok = strtok(NULL, ","))
            poll_offsets[poll_count++] = strtoull(tok, NULL, 16);
        pthread_t t; pthread_create(&t, NULL, poll_thread, NULL); pthread_detach(t);
    }
    patch_dta_pool(m);
    if (getenv("TOTK_BOTW_OBS")) {
        uint32_t *site = (uint32_t *)(void *)(m->base + OBS_OFFSET);
        if (*site != 0x320003e2u) fprintf(stderr, "[botw-obs] site %#llx has %#x, not mov w2,#1: observer off\n", (unsigned long long)OBS_OFFSET, *site);
        else { *site = 0xd4200020u; obs_site = (uint8_t *)site; fprintf(stderr, "[botw-obs] observing main+%#llx\n", (unsigned long long)OBS_OFFSET); }
        uint32_t *entry = (uint32_t *)(void *)(m->base + OBS_ENTRY_OFFSET);
        const char *obs = getenv("TOTK_BOTW_OBS");
        if (!strcmp(obs, "5")) {   /* allocations (entry) + frees */
            uint32_t *entry5 = (uint32_t *)(void *)(m->base + OBS_ENTRY_OFFSET), *fr = (uint32_t *)(void *)(m->base + OBS_FREE_OFFSET);
            if (*entry5 == 0xd107c3ffu && *fr == 0xa9be4ff4u) { *entry5 = 0xd4200040u; obs_entry = (uint8_t *)entry5; *fr = 0xd4200080u; obs_free = (uint8_t *)fr;
                fprintf(stderr, "[botw-obs] counting allocations and frees\n"); }
            else fprintf(stderr, "[botw-obs] sites differ (%#x, %#x): counters off\n", *entry5, *fr);
        }
        if (!strcmp(obs, "3") || !strcmp(obs, "4")) for (unsigned i = 0; i < NCMP; i++) {
            if (!strcmp(obs, "4") && cmp_sites[i] != 0x1296c78) continue;   /* 4: only the section check, light enough not to slow the game down */
            uint32_t *w = (uint32_t *)(void *)(m->base + cmp_sites[i]); cmp_orig[i] = *w;
            if ((*w & 0xffe0fc1fu) == 0xeb00001fu) { *w = 0xd4200060u; cmp_addr[i] = (uint8_t *)w; }
            else fprintf(stderr, "[botw-obs] site %#x is %#x, not cmp xN,xM\n", cmp_sites[i], *w);
        }
        if (!strcmp(getenv("TOTK_BOTW_OBS"), "2") && *entry == 0xd107c3ffu) { *entry = 0xd4200040u; obs_entry = (uint8_t *)entry; fprintf(stderr, "[botw-obs] recording calls of main+%#llx\n", (unsigned long long)OBS_ENTRY_OFFSET); }
    }
    fprintf(stderr, "[totk] %s: no game-code patches\n", TOTK_GAME_VERSION);
}
/* NVN API 53.311, the version its SDK was built against and the game checks at start (main+0xe55300, 0xe55310). */
int runtime_nvn_api_version(int minor) { return minor ? 311 : 53; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
