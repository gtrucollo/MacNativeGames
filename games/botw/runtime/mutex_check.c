/* Bring-up aid (TOTK_MUTEX_CHECK=1): the game's nn::os::LockMutex / UnlockMutex / TryLockMutex run the SDK's own code, wrapped so the
   owner of every mutex is known; a lock that returns while another thread still owns the mutex (the SDK mutex failed to exclude) is
   reported with the guest call chain. */
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void runtime_trace_guest(uint64_t fp, uint64_t lr);

typedef struct { _Atomic uint64_t addr, owner; _Atomic int count, reports; } MutexRec;
#define NREC 8192
static MutexRec recs[NREC];
static _Atomic int violations;

static MutexRec *rec_of(uint64_t m) {
    unsigned h = (unsigned)((m >> 4) * 2654435761u) % NREC;
    for (unsigned i = 0; i < NREC; i++) {
        MutexRec *r = &recs[(h + i) % NREC];
        uint64_t a = atomic_load(&r->addr);
        if (a == m) return r;
        if (!a) { uint64_t z = 0; if (atomic_compare_exchange_strong(&r->addr, &z, m) || z == m) return r; }
    }
    return NULL;
}
uint32_t runtime_thread_handle_self(void);
static uint64_t me(void) { uint32_t h = runtime_thread_handle_self(); return h ? h : (uint64_t)(uintptr_t)pthread_self(); }

static void *orig(const char *name) { void *p = runtime_symbol(name); if (!p) runtime_fail("mutex check: SDK function missing"); return p; }

static void report(const char *what, uint64_t m, uint64_t owner, void *lr, void *fp) {
    { MutexRec *r = rec_of(m); if (r && atomic_fetch_add(&r->reports, 1) >= 3) return; }   /* a mutex used with a condition variable keeps tripping this: three reports each */
    if (atomic_fetch_add(&violations, 1) >= 400) return;
    fprintf(stderr, "[mutex-check] t=%.3f VIOLATION %s mutex=%#llx owned by thread %#llx, this thread %#llx\n", runtime_now_ns() / 1e9, what, (unsigned long long)m,
            (unsigned long long)owner, (unsigned long long)me());
    { static _Atomic int chains; if (atomic_fetch_add(&chains, 1) < 6) runtime_trace_guest((uint64_t)(uintptr_t)fp, (uint64_t)(uintptr_t)lr); }
}
static void lock_mutex(void *m) {
    static void (*f)(void *); if (!f) f = orig("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE");
    void *lr = __builtin_return_address(0), *fp = *(void **)__builtin_frame_address(0);
    MutexRec *r = rec_of((uint64_t)(uintptr_t)m);
    uint64_t o = r ? atomic_load(&r->owner) : 0;
    f(m);
    if (!r) return;
    uint64_t cur = atomic_load(&r->owner);
    if (cur == me()) { atomic_fetch_add(&r->count, 1); return; }   // recursive lock
    if (cur) report("lock", (uint64_t)(uintptr_t)m, cur, lr, fp);
    (void)o;
    atomic_store(&r->owner, me()); atomic_store(&r->count, 1);
}
static int try_lock_mutex(void *m) {
    static int (*f)(void *); if (!f) f = orig("_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE");
    void *lr = __builtin_return_address(0), *fp = *(void **)__builtin_frame_address(0);
    MutexRec *r = rec_of((uint64_t)(uintptr_t)m);
    int ok = f(m) & 1;
    if (!ok || !r) return ok;
    uint64_t cur = atomic_load(&r->owner);
    if (cur == me()) { atomic_fetch_add(&r->count, 1); return ok; }
    if (cur) report("trylock", (uint64_t)(uintptr_t)m, cur, lr, fp);
    atomic_store(&r->owner, me()); atomic_store(&r->count, 1);
    return ok;
}
static void unlock_mutex(void *m) {
    static void (*f)(void *); if (!f) f = orig("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
    void *lr = __builtin_return_address(0), *fp = *(void **)__builtin_frame_address(0);
    MutexRec *r = rec_of((uint64_t)(uintptr_t)m);
    if (r) {
        uint64_t cur = atomic_load(&r->owner);
        if (cur != me()) report("unlock by a non-owner of", (uint64_t)(uintptr_t)m, cur, lr, fp);
        else if (atomic_fetch_sub(&r->count, 1) == 1) atomic_store(&r->owner, 0);   // cleared before the release: the next owner sets it after its lock returns
    }
    f(m);
}

void *runtime_mutex_check_override(const char *name) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_MUTEX_CHECK"); on = e && *e && strcmp(e, "0"); if (on) fprintf(stderr, "[mutex-check] on\n"); }
    if (!on) return NULL;
    if (!strcmp(name, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE")) return lock_mutex;
    if (!strcmp(name, "_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE")) return try_lock_mutex;
    if (!strcmp(name, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE")) return unlock_mutex;
    return NULL;
}
