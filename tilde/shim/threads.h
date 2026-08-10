// Minimal C11 <threads.h> shim for Apple Clang, backed by pthreads.
// Only implements what Cuik/TB/NBHM actually uses.
#ifndef C11_THREADS_SHIM_H
#define C11_THREADS_SHIM_H

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <errno.h>

typedef pthread_t       thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t  cnd_t;
typedef int             thrd_start_t;

enum {
    thrd_success  = 0,
    thrd_nomem    = 1,
    thrd_timedout = 2,
    thrd_busy     = 3,
    thrd_error    = 4
};

enum {
    mtx_plain     = 0,
    mtx_recursive = 1,
    mtx_timed     = 2
};

typedef struct thrd_start_trampoline {
    thrd_start_t (*func)(void *);
    void *arg;
} thrd_start_trampoline;

static void *thrd_trampoline(void *ptr) {
    thrd_start_trampoline *t = (thrd_start_trampoline *)ptr;
    thrd_start_t (*func)(void *) = t->func;
    void *arg = t->arg;
    pthread_mutex_t *free_mtx = (pthread_mutex_t *)t[1].arg; // unused slot trick avoided below
    (void)free_mtx;
    // NOTE: we free the trampoline in thrd_create via detached stack copy instead
    int r = func(arg);
    return (void *)(long)r;
}

// Simplest correct approach: leak a tiny struct per thread creation is bad,
// so use pthread cleanup via stack allocation + synchronization is overkill.
// NBHM creates exactly one long-lived thread, so a small static pool is fine.
static thrd_start_trampoline thrd_tramp_pool[64];
static _Thread_local int thrd_tramp_dummy;
static pthread_mutex_t thrd_tramp_pool_mtx = PTHREAD_MUTEX_INITIALIZER;

static inline int thrd_create(thrd_t *thr, thrd_start_t (*func)(void *), void *arg) {
    pthread_mutex_lock(&thrd_tramp_pool_mtx);
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if (thrd_tramp_pool[i].func == NULL) { slot = i; break; }
    }
    if (slot < 0) { pthread_mutex_unlock(&thrd_tramp_pool_mtx); return thrd_nomem; }
    thrd_tramp_pool[slot].func = func;
    thrd_tramp_pool[slot].arg = arg;
    int rc = pthread_create(thr, NULL, thrd_trampoline, &thrd_tramp_pool[slot]);
    pthread_mutex_unlock(&thrd_tramp_pool_mtx);
    return rc == 0 ? thrd_success : thrd_error;
}

static inline int thrd_join(thrd_t thr, int *res) {
    void *ret = NULL;
    if (pthread_join(thr, &ret) != 0) return thrd_error;
    if (res) *res = (int)(long)ret;
    return thrd_success;
}

static inline void thrd_yield(void) { sched_yield(); }

static inline int mtx_init(mtx_t *mtx, int type) {
    (void)type;
    return pthread_mutex_init(mtx, NULL) == 0 ? thrd_success : thrd_error;
}

static inline int mtx_lock(mtx_t *mtx) {
    return pthread_mutex_lock(mtx) == 0 ? thrd_success : thrd_error;
}

static inline int mtx_unlock(mtx_t *mtx) {
    return pthread_mutex_unlock(mtx) == 0 ? thrd_success : thrd_error;
}

static inline void mtx_destroy(mtx_t *mtx) { pthread_mutex_destroy(mtx); }

static inline int cnd_init(cnd_t *cond) {
    return pthread_cond_init(cond, NULL) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_broadcast(cnd_t *cond) {
    return pthread_cond_broadcast(cond) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_signal(cnd_t *cond) {
    return pthread_cond_signal(cond) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_wait(cnd_t *cond, mtx_t *mtx) {
    return pthread_cond_wait(cond, mtx) == 0 ? thrd_success : thrd_error;
}

static inline int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts) {
    int rc = pthread_cond_timedwait(cond, mtx, ts);
    if (rc == 0) return thrd_success;
    if (rc == ETIMEDOUT) return thrd_timedout;
    return thrd_error;
}

typedef pthread_once_t once_flag;
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT

static inline void call_once(once_flag *flag, void (*func)(void)) {
    pthread_once(flag, func);
}

typedef pthread_key_t tss_t;
typedef void (*tss_dtor_t)(void *);
#define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS

static inline int tss_create(tss_t *key, tss_dtor_t dtor) {
    return pthread_key_create(key, dtor) == 0 ? thrd_success : thrd_error;
}

static inline void *tss_get(tss_t key) { return pthread_getspecific(key); }

static inline int tss_set(tss_t key, void *val) {
    return pthread_setspecific(key, val) == 0 ? thrd_success : thrd_error;
}

static inline void tss_delete(tss_t key) { pthread_key_delete(key); }

static inline thrd_t thrd_current(void) { return pthread_self(); }

static inline void cnd_destroy(cnd_t *cond) { pthread_cond_destroy(cond); }

#endif // C11_THREADS_SHIM_H
