/*
 * ntsync_test.c - userspace correctness tests for FreeBSD ntsync.ko
 *
 * Build: cc ntsync_test.c -o ntsync_test -lpthread
 * Run:   doas ./ntsync_test        (needs /dev/ntsync)
 *
 * Tests verify that the kmod behaves identically to Linux ntsync,
 * which is required for Wine/Proton to work correctly.
 */

#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <err.h>
#include <errno.h>

#include "ntsync_uapi.h"

/* ------------------------------------------------------------------ */
/* Test framework                                                       */
/* ------------------------------------------------------------------ */

static int dev_fd;
static int passed = 0;
static int failed = 0;
static const char *current_test = "";

#define FAIL(msg) do { \
    printf("  [FAIL] %s: %s (errno=%d: %s)\n", \
           current_test, msg, errno, strerror(errno)); \
    failed++; \
    return; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) FAIL(msg); \
} while(0)

#define ASSERT_ERRNO(cond, expected_errno, msg) do { \
    if (!(cond)) FAIL(msg); \
    if (errno != (expected_errno)) { \
        printf("  [FAIL] %s: %s: expected errno=%d (%s), got errno=%d (%s)\n", \
               current_test, msg, \
               expected_errno, strerror(expected_errno), \
               errno, strerror(errno)); \
        failed++; \
        return; \
    } \
} while(0)

static void pass(void) {
    printf("  [PASS] %s\n", current_test);
    passed++;
}

/* ------------------------------------------------------------------ */
/* Thread helpers                                                       */
/* ------------------------------------------------------------------ */

struct wait_ctx {
    int      dev_fd;
    uint32_t fds[64];   /* array of object fds to wait on */
    uint32_t count;
    uint32_t owner;
    uint64_t timeout;
    int      result;    /* ioctl return value */
    int      err;       /* errno after ioctl */
    uint32_t index;     /* which object woke us */
    volatile int done;
};

static void *thread_wait_any(void *arg)
{
    struct wait_ctx *ctx = arg;
    struct ntsync_wait_args wa = {0};

    wa.objs    = (uintptr_t)ctx->fds;   /* pointer to fd array in userspace */
    wa.count   = ctx->count;
    wa.owner   = ctx->owner;
    wa.timeout = ctx->timeout;

    ctx->result = ioctl(ctx->dev_fd, NTSYNC_IOC_WAIT_ANY, &wa);
    ctx->err    = errno;
    ctx->index  = wa.index;
    __sync_synchronize();
    ctx->done   = 1;
    return NULL;
}

static void *thread_wait_all(void *arg)
{
    struct wait_ctx *ctx = arg;
    struct ntsync_wait_args wa = {0};

    /* WAIT_ALL uses the exact same struct as WAIT_ANY, just different ioctl */
    wa.objs    = (uintptr_t)ctx->fds;
    wa.count   = ctx->count;
    wa.owner   = ctx->owner;
    wa.timeout = ctx->timeout;

    ctx->result = ioctl(ctx->dev_fd, NTSYNC_IOC_WAIT_ALL, &wa);
    ctx->err    = errno;
    ctx->index  = wa.index;
    __sync_synchronize();
    ctx->done   = 1;
    return NULL;
}

/* Spin-wait until ctx->done or timeout_ms elapsed. Returns 1 if done. */
static int wait_for(volatile int *flag, int timeout_ms)
{
    int i;
    for (i = 0; i < timeout_ms * 10; i++) {
        if (*flag) return 1;
        usleep(100);
    }
    return *flag;
}

/* ------------------------------------------------------------------ */
/* TEST 1: Semaphore basic operations                                   */
/* ------------------------------------------------------------------ */
static void test_semaphore(void)
{
    current_test = "Semaphore basic";

    struct ntsync_sem_args sa = {.count = 0, .max = 3};
    int sem_fd = ioctl(dev_fd, NTSYNC_IOC_CREATE_SEM, &sa);
    ASSERT(sem_fd >= 0, "CREATE_SEM failed");

    /* Release 1 token at a time */
    uint32_t arg = 1;
    int r = ioctl(sem_fd, NTSYNC_IOC_SEM_RELEASE, &arg);
    ASSERT(r == 0, "SEM_RELEASE(1) failed");
    ASSERT(arg == 0, "prev count should be 0 before first release");

    arg = 1;
    r = ioctl(sem_fd, NTSYNC_IOC_SEM_RELEASE, &arg);
    ASSERT(r == 0, "SEM_RELEASE(2) failed");
    ASSERT(arg == 1, "prev count should be 1 after second release");

    /* Read back state */
    struct ntsync_sem_args rs = {0};
    r = ioctl(sem_fd, NTSYNC_IOC_SEM_READ, &rs);
    ASSERT(r == 0, "SEM_READ failed");
    ASSERT(rs.count == 2, "sem count should be 2");
    ASSERT(rs.max   == 3, "sem max should be 3");

    /* Overflow check: adding 2 more would exceed max=3 */
    uint32_t big = 2;
    r = ioctl(sem_fd, NTSYNC_IOC_SEM_RELEASE, &big);
    ASSERT(r == -1, "SEM_RELEASE overflow should fail");
    ASSERT(errno == EOVERFLOW, "overflow should set EOVERFLOW");

    close(sem_fd);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 2: Auto-reset event — only one waiter wakes per signal         */
/* ------------------------------------------------------------------ */
static void test_auto_reset_event(void)
{
    current_test = "Auto-Reset Event";

    struct ntsync_event_args ea = {.manual = 0, .signaled = 0};
    int ev = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    ASSERT(ev >= 0, "CREATE_EVENT failed");

    /* Two threads both waiting on the same auto-reset event */
    struct wait_ctx t1 = {.dev_fd=dev_fd, .fds={ev}, .count=1, .timeout=UINT64_MAX};
    struct wait_ctx t2 = {.dev_fd=dev_fd, .fds={ev}, .count=1, .timeout=UINT64_MAX};
    pthread_t th1, th2;
    pthread_create(&th1, NULL, thread_wait_any, &t1);
    pthread_create(&th2, NULL, thread_wait_any, &t2);

    usleep(50000); /* let both threads sleep in kernel */

    /* One SET — only one waiter should wake, event auto-resets */
    uint32_t prev = 0;
    ioctl(ev, NTSYNC_IOC_EVENT_SET, &prev);
    usleep(100000);

    int woken = t1.done + t2.done;
    ASSERT(woken == 1, "auto-reset event must wake exactly 1 waiter, not more");

    /* Wake the other thread so we can join cleanly */
    ioctl(ev, NTSYNC_IOC_EVENT_SET, &prev);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    close(ev);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 3: Manual-reset event — all waiters wake                       */
/* ------------------------------------------------------------------ */
static void test_manual_reset_event(void)
{
    current_test = "Manual-Reset Event";

    struct ntsync_event_args ea = {.manual = 1, .signaled = 0};
    int ev = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    ASSERT(ev >= 0, "CREATE_EVENT manual failed");

    struct wait_ctx t1 = {.dev_fd=dev_fd, .fds={ev}, .count=1, .timeout=UINT64_MAX};
    struct wait_ctx t2 = {.dev_fd=dev_fd, .fds={ev}, .count=1, .timeout=UINT64_MAX};
    pthread_t th1, th2;
    pthread_create(&th1, NULL, thread_wait_any, &t1);
    pthread_create(&th2, NULL, thread_wait_any, &t2);

    usleep(50000);

    uint32_t prev = 0;
    ioctl(ev, NTSYNC_IOC_EVENT_SET, &prev);
    usleep(100000);

    /* Manual-reset: event stays signaled, both waiters must wake */
    ASSERT(t1.done && t2.done, "manual-reset event must wake all waiters");

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    close(ev);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 4: Event RESET and PULSE                                        */
/* ------------------------------------------------------------------ */
static void test_event_reset_pulse(void)
{
    current_test = "Event RESET and PULSE";

    /* Start signaled */
    struct ntsync_event_args ea = {.manual = 1, .signaled = 1};
    int ev = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    ASSERT(ev >= 0, "CREATE_EVENT failed");

    uint32_t prev = 0;
    int r = ioctl(ev, NTSYNC_IOC_EVENT_RESET, &prev);
    ASSERT(r == 0, "EVENT_RESET failed");
    ASSERT(prev == 1, "prev state should be 1 (was signaled)");

    struct ntsync_event_args rs = {0};
    ioctl(ev, NTSYNC_IOC_EVENT_READ, &rs);
    ASSERT(rs.signaled == 0, "event should be unsignaled after RESET");

    /* PULSE: set then immediately reset, should wake one waiter */
    struct wait_ctx t1 = {.dev_fd=dev_fd, .fds={ev}, .count=1, .timeout=UINT64_MAX};
    pthread_t th1;
    pthread_create(&th1, NULL, thread_wait_any, &t1);
    usleep(50000);

    r = ioctl(ev, NTSYNC_IOC_EVENT_PULSE, &prev);
    ASSERT(r == 0, "EVENT_PULSE failed");
    ASSERT(wait_for(&t1.done, 200), "PULSE should wake the waiter");

    /* After pulse, manual event must be unsignaled */
    ioctl(ev, NTSYNC_IOC_EVENT_READ, &rs);
    ASSERT(rs.signaled == 0, "event must be unsignaled after PULSE");

    pthread_join(th1, NULL);
    close(ev);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 5: Mutex — acquire via WAIT_ANY, release via MUTEX_UNLOCK      */
/* ------------------------------------------------------------------ */
static void test_mutex(void)
{
    current_test = "Mutex acquire/release";

    /* owner=0 means unowned (signaled = available to acquire) */
    struct ntsync_mutex_args ma = {.owner = 0, .count = 0};
    int mx = ioctl(dev_fd, NTSYNC_IOC_CREATE_MUTEX, &ma);
    ASSERT(mx >= 0, "CREATE_MUTEX failed");

    /*
     * Mutex is acquired via WAIT_ANY with owner set — NOT a separate
     * MUTEX_ACQUIRE ioctl (that doesn't exist). The wait mechanism IS
     * the acquire mechanism, same as on Linux.
     */
    struct wait_ctx main_ctx = {
        .dev_fd = dev_fd, .fds = {mx}, .count = 1,
        .owner = 42,      /* thread ID / owner token */
        .timeout = 0,     /* non-blocking: fail if not immediately available */
    };
    int r = ioctl(dev_fd, NTSYNC_IOC_WAIT_ANY,
                  &(struct ntsync_wait_args){
                      .objs = (uintptr_t)main_ctx.fds,
                      .count = 1, .owner = 42, .timeout = 0});
    ASSERT(r == 0, "WAIT_ANY acquire of unowned mutex should succeed immediately");

    /* Now another thread tries to acquire — must block */
    struct wait_ctx t1 = {.dev_fd=dev_fd, .fds={mx}, .count=1,
                          .owner=99, .timeout=UINT64_MAX};
    pthread_t th1;
    pthread_create(&th1, NULL, thread_wait_any, &t1);
    usleep(50000);
    ASSERT(!t1.done, "thread must not acquire mutex already owned by main");

    /* Unlock from owner=42 */
    struct ntsync_mutex_args unlock_args = {.owner = 42, .count = 1};
    r = ioctl(mx, NTSYNC_IOC_MUTEX_UNLOCK, &unlock_args);
    ASSERT(r == 0, "MUTEX_UNLOCK failed");

    ASSERT(wait_for(&t1.done, 300), "thread must acquire mutex after unlock");
    ASSERT(t1.result == 0, "thread WAIT_ANY should succeed");

    /* Unlock from owner=99 */
    unlock_args.owner = 99; unlock_args.count = 1;
    ioctl(mx, NTSYNC_IOC_MUTEX_UNLOCK, &unlock_args);

    pthread_join(th1, NULL);
    close(mx);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 6: Mutex KILL (abandoned mutex)                                */
/* ------------------------------------------------------------------ */
static void test_mutex_kill(void)
{
    current_test = "Mutex KILL (abandoned)";

    struct ntsync_mutex_args ma = {.owner = 77, .count = 1};
    int mx = ioctl(dev_fd, NTSYNC_IOC_CREATE_MUTEX, &ma);
    ASSERT(mx >= 0, "CREATE_MUTEX owned failed");

    /* Kill (abandon) mutex — owner died without releasing */
    uint32_t owner = 77;
    int r = ioctl(mx, NTSYNC_IOC_MUTEX_KILL, &owner);
    ASSERT(r == 0, "MUTEX_KILL failed");

    /* Next waiter should get EOWNERDEAD */
    struct ntsync_wait_args wa = {
        .objs = (uintptr_t)&(uint32_t){mx},
        .count = 1, .owner = 55, .timeout = 0
    };
    r = ioctl(dev_fd, NTSYNC_IOC_WAIT_ANY, &wa);
    ASSERT(r == -1, "wait on abandoned mutex should return -1");
    ASSERT(errno == EOWNERDEAD, "abandoned mutex must return EOWNERDEAD");

    close(mx);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 7: WAIT_ALL — must wait until ALL objects are signaled         */
/* ------------------------------------------------------------------ */
static void test_wait_all(void)
{
    current_test = "WAIT_ALL atomicity";

    struct ntsync_event_args ea = {.manual = 1, .signaled = 0};
    int ev1 = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    int ev2 = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    ASSERT(ev1 >= 0 && ev2 >= 0, "CREATE_EVENT x2 failed");

    struct wait_ctx ctx = {
        .dev_fd  = dev_fd,
        .fds     = {ev1, ev2},   /* array of fds — passed as objs pointer */
        .count   = 2,
        .owner   = 1,
        .timeout = UINT64_MAX,
    };
    pthread_t th;
    pthread_create(&th, NULL, thread_wait_all, &ctx);
    usleep(50000);

    ASSERT(!ctx.done, "WAIT_ALL must not complete before any event is set");

    uint32_t prev = 0;
    ioctl(ev1, NTSYNC_IOC_EVENT_SET, &prev);
    usleep(100000);
    ASSERT(!ctx.done, "WAIT_ALL must not complete after only first event is set");

    ioctl(ev2, NTSYNC_IOC_EVENT_SET, &prev);
    ASSERT(wait_for(&ctx.done, 300), "WAIT_ALL must complete when both events are set");
    ASSERT(ctx.result == 0, "WAIT_ALL should return 0 on success");

    pthread_join(th, NULL);
    close(ev1);
    close(ev2);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 8: Timeout — kmod returns ETIMEDOUT, not ETIME                 */
/* ------------------------------------------------------------------ */
static void test_timeout(void)
{
    current_test = "Wait timeout (ETIMEDOUT)";

    struct ntsync_event_args ea = {.manual = 0, .signaled = 0};
    int ev = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    ASSERT(ev >= 0, "CREATE_EVENT failed");

    /*
     * ntsync timeout is absolute ns from CLOCK_MONOTONIC (same as Linux).
     * FreeBSD kmod converts to ticks internally.
     */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t deadline = (uint64_t)ts.tv_sec * 1000000000ULL
                      + (uint64_t)ts.tv_nsec
                      + 200000000ULL; /* 200ms from now */

    struct wait_ctx ctx = {
        .dev_fd = dev_fd, .fds = {ev}, .count = 1,
        .owner = 1, .timeout = deadline,
    };

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th;
    pthread_create(&th, NULL, thread_wait_any, &ctx);
    pthread_join(th, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                      + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    ASSERT(ctx.done, "thread must complete after timeout");
    ASSERT(ctx.result == -1, "timed-out wait must return -1");

    /*
     * FreeBSD kmod returns ETIMEDOUT (not Linux's ETIME=62).
     * ETIMEDOUT is the correct POSIX errno for a timed operation that expired.
     */
    errno = ctx.err;
    ASSERT(ctx.err == ETIMEDOUT, "timeout must set errno=ETIMEDOUT");

    ASSERT(elapsed_ms > 100 && elapsed_ms < 600,
           "timeout duration out of range (expected ~200ms)");

    close(ev);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 9: WAIT_ANY with alert fd                                       */
/* ------------------------------------------------------------------ */
static void test_wait_any_index(void)
{
    current_test = "WAIT_ANY index reporting";

    struct ntsync_event_args ea = {.manual = 1, .signaled = 0};
    int ev0 = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    int ev1 = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    int ev2 = ioctl(dev_fd, NTSYNC_IOC_CREATE_EVENT, &ea);
    ASSERT(ev0 >= 0 && ev1 >= 0 && ev2 >= 0, "CREATE_EVENT x3 failed");

    /* Signal the middle one (index 1) */
    uint32_t prev = 0;
    ioctl(ev1, NTSYNC_IOC_EVENT_SET, &prev);

    struct ntsync_wait_args wa = {
        .objs    = (uintptr_t)(uint32_t[]){ev0, ev1, ev2},
        .count   = 3,
        .owner   = 1,
        .timeout = 0,   /* non-blocking */
    };
    int r = ioctl(dev_fd, NTSYNC_IOC_WAIT_ANY, &wa);
    ASSERT(r == 0, "WAIT_ANY should succeed when one obj is signaled");
    ASSERT(wa.index == 1, "WAIT_ANY index must be 1 (ev1 was signaled)");

    close(ev0); close(ev1); close(ev2);
    pass();
}

/* ------------------------------------------------------------------ */
/* TEST 10: Verify ioctl numbers match Linux (ABI sanity check)        */
/* ------------------------------------------------------------------ */
static void test_ioctl_numbers(void)
{
    current_test = "IOCTL number ABI sanity";

    /*
     * These are the hardcoded values from Linux ntsync.h.
     * If any differ, Wine will call wrong ioctls and silently fall back
     * to wineserver RPC — no error, just silent performance regression.
     *
     * Calculated as: (direction<<30)|(size<<16)|(type<<8)|nr
     * type='N'=0x4e, uint32=4, ntsync_wait_args=32
     */
    struct { unsigned long actual; unsigned long expected; const char *name; } checks[] = {
        { NTSYNC_IOC_CREATE_SEM,   0x40084e80UL,                    "CREATE_SEM"   },
        { NTSYNC_IOC_WAIT_ANY,     0xc0284e82UL,                    "WAIT_ANY"     },
        { NTSYNC_IOC_WAIT_ALL,     0xc0284e83UL,                    "WAIT_ALL"     },
        { NTSYNC_IOC_CREATE_MUTEX, 0x40084e84UL,                    "CREATE_MUTEX" },
        { NTSYNC_IOC_CREATE_EVENT, 0x40084e87UL,                    "CREATE_EVENT" },
        { NTSYNC_IOC_SEM_RELEASE,  0xc0044e81UL,                    "SEM_RELEASE"  },
        { NTSYNC_IOC_MUTEX_UNLOCK, 0xc0084e85UL,                    "MUTEX_UNLOCK" },
        { NTSYNC_IOC_EVENT_SET,    0x80044e88UL,                    "EVENT_SET"    },  /* _IOR! */
        { NTSYNC_IOC_EVENT_RESET,  0x80044e89UL,                    "EVENT_RESET"  },  /* _IOR! */
        { NTSYNC_IOC_EVENT_PULSE,  0x80044e8aUL,                    "EVENT_PULSE"  },  /* _IOR! */
    };

    int ok = 1;
    for (size_t i = 0; i < sizeof(checks)/sizeof(checks[0]); i++) {
        unsigned long a = checks[i].actual & 0xffffffffUL;
        unsigned long e = checks[i].expected & 0xffffffffUL;
        if (a != e) {
            printf("  [FAIL] %s: ioctl number 0x%08lx != expected 0x%08lx\n",
                   checks[i].name, a, e);
            ok = 0;
        } else {
            printf("  [ok]   %-15s 0x%08lx\n", checks[i].name, a);
        }
    }
    ASSERT(ok, "ioctl ABI mismatch — Wine will not use ntsync");
    pass();
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("====================================================\n");
    printf("  FreeBSD ntsync.ko — Wine/Proton Correctness Tests\n");
    printf("====================================================\n\n");

    dev_fd = open("/dev/ntsync", O_RDWR);
    if (dev_fd < 0)
        err(1, "Cannot open /dev/ntsync — is ntsync.ko loaded?");

    printf("[INFO] /dev/ntsync opened (fd=%d)\n\n", dev_fd);

    /* Run in order from simplest to most complex */
    test_ioctl_numbers();        /* sanity first — if this fails, nothing else matters */
    test_semaphore();
    test_auto_reset_event();
    test_manual_reset_event();
    test_event_reset_pulse();
    test_mutex();
    test_mutex_kill();
    test_wait_all();             /* hardest — deadlock detection */
    test_timeout();
    test_wait_any_index();

    close(dev_fd);

    printf("\n====================================================\n");
    printf("  RESULTS: %d passed, %d FAILED\n", passed, failed);
    printf("====================================================\n");

    if (failed > 0) {
        printf("\n[!!] Kmod has bugs — Wine will fall back to wineserver RPC.\n");
        printf("     Stuttering will NOT be fixed until all tests pass.\n");
        return 1;
    }

    printf("\n[OK] All tests passed — kmod is Wine/Proton compatible.\n");
    printf("     Next step: launch Steam from terminal and check:\n");
    printf("       steam 2>&1 | grep -i ntsync\n");
    printf("     Expected: 'wineserver: NTSync up and running!'\n");
    return 0;
}