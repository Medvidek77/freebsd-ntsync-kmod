/*
 * FreeBSD ntsync.ko - NT synchronization primitive emulation
 * Ported from Linux drivers/misc/ntsync.c
 *
 * Emulates Windows NT synchronization primitives in kernel-space
 * to provide accurate semantics and high performance for Wine/Proton.
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sx.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/capsicum.h>
#include <sys/syscallsubr.h>
#include <sys/refcount.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/counter.h>
#include <machine/atomic.h>

#include "ntsync_uapi.h"

MALLOC_DEFINE(M_NTSYNC, "ntsync", "NT sync objects");

#define NTSYNC_SIGNALED_NONE (-1)
#define NTSYNC_STACK_COUNT 16

static volatile u_int ntsync_refs;

#ifndef NTSYNC_NO_STATS
static SYSCTL_NODE(_kern, OID_AUTO, ntsync, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "ntsync stats");

#define NTSYNC_STAT(name) \
    static counter_u64_t ntsync_stat_##name; \
    SYSCTL_COUNTER_U64(_kern_ntsync, OID_AUTO, name, CTLFLAG_RD, &ntsync_stat_##name, "")

NTSYNC_STAT(dev_opens);
NTSYNC_STAT(sem_creates);
NTSYNC_STAT(mutex_creates);
NTSYNC_STAT(event_creates);
NTSYNC_STAT(wait_any_calls);
NTSYNC_STAT(wait_all_calls);
NTSYNC_STAT(woke_any);
NTSYNC_STAT(woke_all);
NTSYNC_STAT(timeouts);
NTSYNC_STAT(signals);
NTSYNC_STAT(sem_releases);
NTSYNC_STAT(mutex_unlocks);
NTSYNC_STAT(mutex_kills);
NTSYNC_STAT(event_sets);
NTSYNC_STAT(event_resets);
NTSYNC_STAT(event_pulses);
NTSYNC_STAT(enotty);

#define STAT(name) counter_u64_add(ntsync_stat_##name, 1)

static void
ntsync_stats_init(void)
{
    ntsync_stat_dev_opens = counter_u64_alloc(M_WAITOK);
    ntsync_stat_sem_creates = counter_u64_alloc(M_WAITOK);
    ntsync_stat_mutex_creates = counter_u64_alloc(M_WAITOK);
    ntsync_stat_event_creates = counter_u64_alloc(M_WAITOK);
    ntsync_stat_wait_any_calls = counter_u64_alloc(M_WAITOK);
    ntsync_stat_wait_all_calls = counter_u64_alloc(M_WAITOK);
    ntsync_stat_woke_any = counter_u64_alloc(M_WAITOK);
    ntsync_stat_woke_all = counter_u64_alloc(M_WAITOK);
    ntsync_stat_timeouts = counter_u64_alloc(M_WAITOK);
    ntsync_stat_signals = counter_u64_alloc(M_WAITOK);
    ntsync_stat_sem_releases = counter_u64_alloc(M_WAITOK);
    ntsync_stat_mutex_unlocks = counter_u64_alloc(M_WAITOK);
    ntsync_stat_mutex_kills = counter_u64_alloc(M_WAITOK);
    ntsync_stat_event_sets = counter_u64_alloc(M_WAITOK);
    ntsync_stat_event_resets = counter_u64_alloc(M_WAITOK);
    ntsync_stat_event_pulses = counter_u64_alloc(M_WAITOK);
    ntsync_stat_enotty = counter_u64_alloc(M_WAITOK);
}

static void
ntsync_stats_fini(void)
{
    counter_u64_free(ntsync_stat_dev_opens);
    counter_u64_free(ntsync_stat_sem_creates);
    counter_u64_free(ntsync_stat_mutex_creates);
    counter_u64_free(ntsync_stat_event_creates);
    counter_u64_free(ntsync_stat_wait_any_calls);
    counter_u64_free(ntsync_stat_wait_all_calls);
    counter_u64_free(ntsync_stat_woke_any);
    counter_u64_free(ntsync_stat_woke_all);
    counter_u64_free(ntsync_stat_timeouts);
    counter_u64_free(ntsync_stat_signals);
    counter_u64_free(ntsync_stat_sem_releases);
    counter_u64_free(ntsync_stat_mutex_unlocks);
    counter_u64_free(ntsync_stat_mutex_kills);
    counter_u64_free(ntsync_stat_event_sets);
    counter_u64_free(ntsync_stat_event_resets);
    counter_u64_free(ntsync_stat_event_pulses);
    counter_u64_free(ntsync_stat_enotty);
}
#else
#define STAT(name) ((void)0)
#define ntsync_stats_init() ((void)0)
#define ntsync_stats_fini() ((void)0)
#endif

enum ntsync_type { NTSYNC_TYPE_SEM, NTSYNC_TYPE_MUTEX, NTSYNC_TYPE_EVENT };

struct ntsync_device {
    u_int     refcount;
    struct sx wait_all_lock;
};

struct ntsync_q;
struct ntsync_q_entry {
    TAILQ_ENTRY(ntsync_q_entry) node;
    struct ntsync_q   *q;
    struct ntsync_obj *obj;
    uint32_t           index;
};

struct ntsync_q {
    volatile int   signaled;
    bool           all;
    bool           ownerdead;
    uint32_t       owner;
    uint32_t       count;
    struct mtx     lock;
    struct cv      cv;
    struct ntsync_q_entry entries[];
};

struct ntsync_obj {
    enum ntsync_type      type;
    struct mtx            lock;
    int                   dev_locked;
    struct ntsync_device *dev;
    volatile u_int        all_hint;
    TAILQ_HEAD(, ntsync_q_entry) any_waiters;
    TAILQ_HEAD(, ntsync_q_entry) all_waiters;
    union {
        struct { uint32_t count; uint32_t max; } sem;
        struct { uint32_t owner; uint32_t count; int abandoned; } mutex;
        struct { uint32_t manual; uint32_t signaled; } event;
    } u;
};

static struct cdev *ntsync_dev;

static int ntsync_obj_stat(struct file *fp, struct stat *sb, struct ucred *cr);
static int ntsync_obj_fill_kinfo(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp);
static int ntsync_obj_ioctl(struct file *fp, u_long cmd, void *data, struct ucred *cr, struct thread *td);
static int ntsync_obj_close(struct file *fp, struct thread *td);

static struct fileops ntsync_obj_ops = {
    .fo_read     = invfo_rdwr,
    .fo_write    = invfo_rdwr,
    .fo_truncate = invfo_truncate,
    .fo_ioctl    = ntsync_obj_ioctl,
    .fo_poll     = invfo_poll,
    .fo_kqfilter = invfo_kqfilter,
    .fo_stat     = ntsync_obj_stat,
    .fo_close    = ntsync_obj_close,
    .fo_chmod    = invfo_chmod,
    .fo_chown    = invfo_chown,
    .fo_sendfile = invfo_sendfile,
    .fo_fill_kinfo = ntsync_obj_fill_kinfo,
    .fo_flags    = DFLAG_PASSABLE,
};

static int
ntsync_obj_stat(struct file *fp, struct stat *sb, struct ucred *cr)
{
    bzero(sb, sizeof(*sb));
    sb->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
    sb->st_blksize = PAGE_SIZE;
    sb->st_uid = cr->cr_uid;
    sb->st_gid = cr->cr_gid;
    sb->st_atim.tv_sec = sb->st_mtim.tv_sec = sb->st_ctim.tv_sec = sb->st_birthtim.tv_sec = time_second;
    return (0);
}

static int
ntsync_obj_fill_kinfo(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp)
{
    kif->kf_type = KF_TYPE_SHM;
    strlcpy(kif->kf_path, "ntsync_obj", sizeof(kif->kf_path));
    return (0);
}

static void
ntsync_dev_release(struct ntsync_device *dev)
{
    if (refcount_release(&dev->refcount)) {
        sx_destroy(&dev->wait_all_lock);
        free(dev, M_NTSYNC);
    }
}

static bool
ntsync_lock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    bool all = atomic_load_acq_int(&obj->all_hint) != 0;
    if (all) sx_slock(&dev->wait_all_lock);
    mtx_lock(&obj->lock);
    return all;
}

static void
ntsync_unlock_obj(struct ntsync_device *dev, struct ntsync_obj *obj, bool all)
{
    mtx_unlock(&obj->lock);
    if (all) sx_sunlock(&dev->wait_all_lock);
}

static void
dev_lock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    mtx_lock(&obj->lock);
    obj->dev_locked = 1;
}

static void
dev_unlock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    obj->dev_locked = 0;
    mtx_unlock(&obj->lock);
}

static bool
is_signaled(struct ntsync_obj *obj, uint32_t owner)
{
    switch (obj->type) {
    case NTSYNC_TYPE_SEM:   return obj->u.sem.count > 0;
    case NTSYNC_TYPE_MUTEX:
        return obj->u.mutex.owner == 0 || obj->u.mutex.owner == owner;
    case NTSYNC_TYPE_EVENT: return obj->u.event.signaled != 0;
    }
    return false;
}

static bool
ntsync_try_acquire_obj(struct ntsync_obj *obj, uint32_t owner, bool *ownerdead)
{
    mtx_assert(&obj->lock, MA_OWNED);
    switch (obj->type) {
    case NTSYNC_TYPE_SEM:
        if (obj->u.sem.count > 0) { obj->u.sem.count--; return true; }
        break;
    case NTSYNC_TYPE_MUTEX:
        if (obj->u.mutex.owner == 0 || obj->u.mutex.owner == owner) {
            if (obj->u.mutex.count < UINT32_MAX) {
                if (obj->u.mutex.abandoned) *ownerdead = true;
                obj->u.mutex.abandoned = 0;
                obj->u.mutex.count++;
                obj->u.mutex.owner = owner;
                return true;
            }
        }
        break;
    case NTSYNC_TYPE_EVENT:
        if (obj->u.event.signaled) {
            if (!obj->u.event.manual) obj->u.event.signaled = 0;
            return true;
        }
        break;
    }
    return false;
}

static void
try_wake_any_sem(struct ntsync_obj *sem)
{
    struct ntsync_q_entry *e;
    TAILQ_FOREACH(e, &sem->any_waiters, node) {
        if (!sem->u.sem.count) break;
        int old = NTSYNC_SIGNALED_NONE;
        if (atomic_cmpset_int(&e->q->signaled, (u_int)old, e->index)) {
            sem->u.sem.count--;
            mtx_lock(&e->q->lock);
            cv_signal(&e->q->cv);
            mtx_unlock(&e->q->lock);
        }
    }
}

static void
try_wake_any_mutex(struct ntsync_obj *mutex)
{
    struct ntsync_q_entry *e;
    TAILQ_FOREACH(e, &mutex->any_waiters, node) {
        if (mutex->u.mutex.count == UINT32_MAX) break;
        struct ntsync_q *q = e->q;
        if (mutex->u.mutex.owner && mutex->u.mutex.owner != q->owner) continue;
        int old = NTSYNC_SIGNALED_NONE;
        if (atomic_cmpset_int(&q->signaled, (u_int)old, e->index)) {
            if (mutex->u.mutex.abandoned) q->ownerdead = true;
            mutex->u.mutex.abandoned = 0;
            mutex->u.mutex.count++;
            mutex->u.mutex.owner = q->owner;
            mtx_lock(&q->lock);
            cv_signal(&q->cv);
            mtx_unlock(&q->lock);
        }
    }
}

static void
try_wake_any_event(struct ntsync_obj *event)
{
    struct ntsync_q_entry *e;
    TAILQ_FOREACH(e, &event->any_waiters, node) {
        if (!event->u.event.signaled) break;
        int old = NTSYNC_SIGNALED_NONE;
        if (atomic_cmpset_int(&e->q->signaled, (u_int)old, e->index)) {
            if (!event->u.event.manual) event->u.event.signaled = 0;
            mtx_lock(&e->q->lock);
            cv_signal(&e->q->cv);
            mtx_unlock(&e->q->lock);
        }
    }
}

static void
try_wake_any_obj(struct ntsync_obj *obj)
{
    switch (obj->type) {
    case NTSYNC_TYPE_SEM:   try_wake_any_sem(obj);   break;
    case NTSYNC_TYPE_MUTEX: try_wake_any_mutex(obj); break;
    case NTSYNC_TYPE_EVENT: try_wake_any_event(obj); break;
    }
}

static void
try_wake_all(struct ntsync_device *dev, struct ntsync_q *q, struct ntsync_obj *locked_obj)
{
    uint32_t count = q->count, i;
    bool can_wake = true;
    int old = NTSYNC_SIGNALED_NONE;
    sx_assert(&dev->wait_all_lock, SA_XLOCKED);
    for (i = 0; i < count; i++)
        if (q->entries[i].obj != locked_obj) dev_lock_obj(dev, q->entries[i].obj);
    for (i = 0; i < count; i++)
        if (!is_signaled(q->entries[i].obj, q->owner)) { can_wake = false; break; }
    if (can_wake && atomic_cmpset_int(&q->signaled, (u_int)old, 0)) {
        for (i = 0; i < count; i++) {
            struct ntsync_obj *obj = q->entries[i].obj;
            switch (obj->type) {
            case NTSYNC_TYPE_SEM:   obj->u.sem.count--; break;
            case NTSYNC_TYPE_MUTEX:
                if (obj->u.mutex.abandoned) q->ownerdead = true;
                obj->u.mutex.abandoned = 0;
                obj->u.mutex.count++;
                obj->u.mutex.owner = q->owner;
                break;
            case NTSYNC_TYPE_EVENT:
                if (!obj->u.event.manual) obj->u.event.signaled = 0;
                break;
            }
        }
        mtx_lock(&q->lock);
        cv_signal(&q->cv);
        mtx_unlock(&q->lock);
    }
    for (i = 0; i < count; i++)
        if (q->entries[i].obj != locked_obj) dev_unlock_obj(dev, q->entries[i].obj);
}

static void
try_wake_all_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    struct ntsync_q_entry *e;
    TAILQ_FOREACH(e, &obj->all_waiters, node) {
        if (sx_try_upgrade(&dev->wait_all_lock)) {
            try_wake_all(dev, e->q, obj);
            sx_downgrade(&dev->wait_all_lock);
        }
    }
}

static int
ntsync_obj_get_fd(struct thread *td, struct ntsync_obj *obj, int *fdp)
{
    struct file *fp;
    int fd, error;
    error = falloc(td, &fp, &fd, O_CLOEXEC);
    if (error) return (error);
    finit(fp, FREAD | FWRITE, DTYPE_DEV, obj, &ntsync_obj_ops);
    *fdp = fd;
    fdrop(fp, td);
    atomic_add_int(&ntsync_refs, 1);
    return (0);
}

static int
ntsync_get_obj(struct thread *td, int fd, struct ntsync_obj **objp, struct file **fpp)
{
    struct file *fp;
    int error = fget(td, fd, &cap_no_rights, &fp);
    if (error) return (error);
    if (fp->f_ops != &ntsync_obj_ops) { fdrop(fp, td); return (EINVAL); }
    *objp = fp->f_data;
    if (fpp) *fpp = fp; else fdrop(fp, td);
    return (0);
}

static int
ntsync_obj_close(struct file *fp, struct thread *td)
{
    struct ntsync_obj *obj = fp->f_data;
    if (!obj) return (0);
    mtx_destroy(&obj->lock);
    ntsync_dev_release(obj->dev);
    free(obj, M_NTSYNC);
    fp->f_data = NULL;
    atomic_subtract_int(&ntsync_refs, 1);
    return (0);
}

static int
ntsync_obj_ioctl(struct file *fp, u_long cmd, void *data, struct ucred *cr, struct thread *td)
{
    struct ntsync_obj *obj = fp->f_data;
    struct ntsync_device *dev = obj->dev;
    bool all;
    int error = 0;
    switch (cmd) {
    case NTSYNC_IOC_SEM_RELEASE: {
        uint32_t *args = data, count = *args, prev;
        if (obj->type != NTSYNC_TYPE_SEM) return (EINVAL);
        all = ntsync_lock_obj(dev, obj);
        prev = obj->u.sem.count;
        if (count > obj->u.sem.max - prev) { ntsync_unlock_obj(dev, obj, all); return (EOVERFLOW); }
        obj->u.sem.count += count;
        if (all) try_wake_all_obj(dev, obj);
        try_wake_any_sem(obj);
        ntsync_unlock_obj(dev, obj, all);
        *args = prev;
        STAT(sem_releases);
        return (0);
    }
    case NTSYNC_IOC_SEM_READ:
    case NTSYNC_IOC_SEM_READ_LINUX: {
        struct ntsync_sem_args *a = data;
        if (obj->type != NTSYNC_TYPE_SEM) return (EINVAL);
        mtx_lock(&obj->lock);
        a->count = obj->u.sem.count; a->max = obj->u.sem.max;
        mtx_unlock(&obj->lock);
        return (0);
    }
    case NTSYNC_IOC_MUTEX_UNLOCK: {
        struct ntsync_mutex_args *a = data;
        if (obj->type != NTSYNC_TYPE_MUTEX || !a->owner) return (EINVAL);
        all = ntsync_lock_obj(dev, obj);
        if (obj->u.mutex.owner != a->owner) error = EPERM;
        else if (!obj->u.mutex.count)       error = EINVAL;
        else {
            a->count = obj->u.mutex.count--;
            if (!obj->u.mutex.count) {
                obj->u.mutex.owner = 0;
                if (all) try_wake_all_obj(dev, obj);
                try_wake_any_mutex(obj);
            }
        }
        ntsync_unlock_obj(dev, obj, all);
        if (!error) STAT(mutex_unlocks);
        return (error);
    }
    case NTSYNC_IOC_MUTEX_KILL:
    case NTSYNC_IOC_MUTEX_KILL_LINUX: {
        uint32_t owner = *(uint32_t *)data;
        if (obj->type != NTSYNC_TYPE_MUTEX || !owner) return (EINVAL);
        all = ntsync_lock_obj(dev, obj);
        if (obj->u.mutex.owner != owner) error = EPERM;
        else {
            obj->u.mutex.owner = obj->u.mutex.count = 0;
            obj->u.mutex.abandoned = 1;
            if (all) try_wake_all_obj(dev, obj);
            try_wake_any_mutex(obj);
        }
        ntsync_unlock_obj(dev, obj, all);
        if (!error) STAT(mutex_kills);
        return (error);
    }
    case NTSYNC_IOC_MUTEX_READ:
    case NTSYNC_IOC_MUTEX_READ_LINUX: {
        struct ntsync_mutex_args *a = data;
        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);
        mtx_lock(&obj->lock);
        if (obj->u.mutex.abandoned) { a->count = a->owner = 0; error = EOWNERDEAD; }
        else { a->count = obj->u.mutex.count; a->owner = obj->u.mutex.owner; }
        mtx_unlock(&obj->lock);
        return (error);
    }
    case NTSYNC_IOC_EVENT_SET:
    case NTSYNC_IOC_EVENT_SET_LINUX: {
        uint32_t *args = data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);
        all = ntsync_lock_obj(dev, obj);
        *args = obj->u.event.signaled;
        obj->u.event.signaled = 1;
        if (all) try_wake_all_obj(dev, obj);
        try_wake_any_event(obj);
        ntsync_unlock_obj(dev, obj, all);
        STAT(event_sets);
        return (0);
    }
    case NTSYNC_IOC_EVENT_RESET:
    case NTSYNC_IOC_EVENT_RESET_LINUX: {
        uint32_t *args = data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);
        all = ntsync_lock_obj(dev, obj);
        *args = obj->u.event.signaled;
        obj->u.event.signaled = 0;
        ntsync_unlock_obj(dev, obj, all);
        STAT(event_resets);
        return (0);
    }
    case NTSYNC_IOC_EVENT_PULSE:
    case NTSYNC_IOC_EVENT_PULSE_LINUX: {
        uint32_t *args = data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);
        all = ntsync_lock_obj(dev, obj);
        *args = obj->u.event.signaled;
        obj->u.event.signaled = 1;
        if (all) try_wake_all_obj(dev, obj);
        try_wake_any_event(obj);
        obj->u.event.signaled = 0;
        ntsync_unlock_obj(dev, obj, all);
        STAT(event_pulses);
        return (0);
    }
    case NTSYNC_IOC_EVENT_READ:
    case NTSYNC_IOC_EVENT_READ_LINUX: {
        struct ntsync_event_args *a = data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);
        mtx_lock(&obj->lock);
        a->manual = obj->u.event.manual; a->signaled = obj->u.event.signaled;
        mtx_unlock(&obj->lock);
        return (0);
    }
    default: STAT(enotty); return (ENOTTY);
    }
}

static int
ntsync_schedule(struct ntsync_q *q, uint64_t timeout_ns, uint32_t flags)
{
    int error = 0;
    mtx_lock(&q->lock);
    while (atomic_load_acq_int((volatile u_int *)&q->signaled) == (u_int)NTSYNC_SIGNALED_NONE) {
        if (timeout_ns == UINT64_MAX) {
            error = cv_wait_sig(&q->cv, &q->lock);
        } else {
            sbintime_t sbt = nstosbt(timeout_ns);
            if (flags & NTSYNC_WAIT_REALTIME) {
                struct timespec rts, mts;
                nanotime(&rts); nanouptime(&mts);
                sbt = tstosbt(mts) + (sbt - tstosbt(rts));
            }
            error = cv_timedwait_sig_sbt(&q->cv, &q->lock, sbt, 0, C_ABSOLUTE);
        }
        if (error == EWOULDBLOCK) { error = ETIMEDOUT; break; }
        if (error == ERESTART)    { error = EINTR;     break; }
        if (error) break;
    }
    mtx_unlock(&q->lock);
    return error;
}

static struct ntsync_q *
ntsync_alloc_q(const struct ntsync_wait_args *args, struct ntsync_obj **objs, uint32_t total)
{
    struct ntsync_q *q = malloc(sizeof(*q) + total * sizeof(q->entries[0]), M_NTSYNC, M_WAITOK | M_ZERO);
    atomic_store_rel_int((volatile u_int *)&q->signaled, (u_int)NTSYNC_SIGNALED_NONE);
    q->owner = args->owner;
    q->count = args->count;
    mtx_init(&q->lock, "ntsync_q", NULL, MTX_DEF);
    cv_init(&q->cv, "ntsync_wait");
    for (uint32_t i = 0; i < total; i++) {
        q->entries[i].q = q; q->entries[i].obj = objs[i]; q->entries[i].index = i;
    }
    return q;
}

static void ntsync_free_q(struct ntsync_q *q) { cv_destroy(&q->cv); mtx_destroy(&q->lock); free(q, M_NTSYNC); }

static int
ntsync_wait_any(struct ntsync_device *dev, struct thread *td, struct ntsync_wait_args *args)
{
    if (args->count > NTSYNC_MAX_WAIT_COUNT) return (EINVAL);
    STAT(wait_any_calls);
    uint32_t total = args->count + (args->alert ? 1 : 0);
    if (total == 0) return (ETIMEDOUT);
    uint32_t fds_st[NTSYNC_STACK_COUNT];
    struct ntsync_obj *objs_st[NTSYNC_STACK_COUNT];
    struct file *fps_st[NTSYNC_STACK_COUNT];
    uint32_t *fds = (total <= NTSYNC_STACK_COUNT) ? fds_st : malloc(total * sizeof(*fds), M_NTSYNC, M_WAITOK);
    struct ntsync_obj **objs = (total <= NTSYNC_STACK_COUNT) ? objs_st : malloc(total * sizeof(*objs), M_NTSYNC, M_WAITOK | M_ZERO);
    struct file **fps = (total <= NTSYNC_STACK_COUNT) ? fps_st : malloc(total * sizeof(*fps), M_NTSYNC, M_WAITOK | M_ZERO);
    int error = 0; uint32_t i;
    if (args->count) error = copyin((void *)(uintptr_t)args->objs, fds, args->count * sizeof(uint32_t));
    if (!error && args->alert) fds[args->count] = args->alert;
    if (error) goto out_free;
    for (i = 0; i < total; i++) {
        error = ntsync_get_obj(td, fds[i], &objs[i], &fps[i]);
        if (error || objs[i]->dev != dev) {
            error = error ? error : EINVAL;
            while (i--) fdrop(fps[i], td);
            goto out_free;
        }
        bool all_h = ntsync_lock_obj(dev, objs[i]);
        bool dead = false;
        if (ntsync_try_acquire_obj(objs[i], args->owner, &dead)) {
            ntsync_unlock_obj(dev, objs[i], all_h);
            args->index = i; error = dead ? EOWNERDEAD : 0;
            STAT(woke_any);
            for (uint32_t k = 0; k <= i; k++) fdrop(fps[k], td);
            goto out_free;
        }
        ntsync_unlock_obj(dev, objs[i], all_h);
    }
    struct ntsync_q *q = ntsync_alloc_q(args, objs, total);
    for (i = 0; i < total; i++) {
        bool all_h = ntsync_lock_obj(dev, objs[i]);
        TAILQ_INSERT_TAIL(&objs[i]->any_waiters, &q->entries[i], node);
        try_wake_any_obj(objs[i]);
        ntsync_unlock_obj(dev, objs[i], all_h);
    }
    error = ntsync_schedule(q, args->timeout, args->flags);
    for (i = 0; i < total; i++) {
        bool all_h = ntsync_lock_obj(dev, objs[i]);
        TAILQ_REMOVE(&objs[i]->any_waiters, &q->entries[i], node);
        ntsync_unlock_obj(dev, objs[i], all_h);
    }
    int sig = (int)atomic_load_acq_int((volatile u_int *)&q->signaled);
    if (sig != NTSYNC_SIGNALED_NONE) {
        error = q->ownerdead ? EOWNERDEAD : 0;
        args->index = (uint32_t)sig;
        STAT(woke_any);
    } else if (error == 0) { error = ETIMEDOUT; STAT(timeouts); }
    else if (error == EINTR) STAT(signals);
    ntsync_free_q(q);
    for (i = 0; i < total; i++) fdrop(fps[i], td);
out_free:
    if (total > NTSYNC_STACK_COUNT) { free(fds, M_NTSYNC); free(objs, M_NTSYNC); free(fps, M_NTSYNC); }
    return (error);
}

static int
ntsync_wait_all(struct ntsync_device *dev, struct thread *td, struct ntsync_wait_args *args)
{
    if (args->count > NTSYNC_MAX_WAIT_COUNT) return (EINVAL);
    STAT(wait_all_calls);
    uint32_t total = args->count + (args->alert ? 1 : 0);
    if (total == 0) return (ETIMEDOUT);
    uint32_t fds_st[NTSYNC_STACK_COUNT];
    struct ntsync_obj *objs_st[NTSYNC_STACK_COUNT];
    struct file *fps_st[NTSYNC_STACK_COUNT];
    uint32_t *fds = (total <= NTSYNC_STACK_COUNT) ? fds_st : malloc(total * sizeof(*fds), M_NTSYNC, M_WAITOK);
    struct ntsync_obj **objs = (total <= NTSYNC_STACK_COUNT) ? objs_st : malloc(total * sizeof(*objs), M_NTSYNC, M_WAITOK | M_ZERO);
    struct file **fps = (total <= NTSYNC_STACK_COUNT) ? fps_st : malloc(total * sizeof(*fps), M_NTSYNC, M_WAITOK | M_ZERO);
    int error = 0; uint32_t i;
    if (args->count) error = copyin((void *)(uintptr_t)args->objs, fds, args->count * sizeof(uint32_t));
    if (!error && args->alert) fds[args->count] = args->alert;
    if (error) goto out_free;
    for (i = 0; i < total; i++) {
        error = ntsync_get_obj(td, fds[i], &objs[i], &fps[i]);
        if (error || objs[i]->dev != dev) {
            error = error ? error : EINVAL;
            while (i--) fdrop(fps[i], td);
            goto out_free;
        }
    }
    struct ntsync_q *q = ntsync_alloc_q(args, objs, total);
    q->all = true;
    sx_xlock(&dev->wait_all_lock);
    for (i = 0; i < args->count; i++) {
        atomic_add_int(&objs[i]->all_hint, 1);
        TAILQ_INSERT_TAIL(&objs[i]->all_waiters, &q->entries[i], node);
    }
    if (args->alert) {
        dev_lock_obj(dev, objs[args->count]);
        TAILQ_INSERT_TAIL(&objs[args->count]->any_waiters, &q->entries[args->count], node);
        dev_unlock_obj(dev, objs[args->count]);
    }
    try_wake_all(dev, q, NULL);
    sx_xunlock(&dev->wait_all_lock);
    error = ntsync_schedule(q, args->timeout, args->flags);
    sx_xlock(&dev->wait_all_lock);
    for (i = 0; i < args->count; i++) {
        TAILQ_REMOVE(&objs[i]->all_waiters, &q->entries[i], node);
        atomic_subtract_int(&objs[i]->all_hint, 1);
    }
    sx_xunlock(&dev->wait_all_lock);
    if (args->alert) {
        bool all_h = ntsync_lock_obj(dev, objs[args->count]);
        TAILQ_REMOVE(&objs[args->count]->any_waiters, &q->entries[args->count], node);
        ntsync_unlock_obj(dev, objs[args->count], all_h);
    }
    int sig = (int)atomic_load_acq_int((volatile u_int *)&q->signaled);
    if (sig != NTSYNC_SIGNALED_NONE) {
        error = q->ownerdead ? EOWNERDEAD : 0;
        args->index = (uint32_t)sig;
        STAT(woke_all);
    } else if (error == 0) { error = ETIMEDOUT; STAT(timeouts); }
    else if (error == EINTR) STAT(signals);
    ntsync_free_q(q);
    for (i = 0; i < total; i++) fdrop(fps[i], td);
out_free:
    if (total > NTSYNC_STACK_COUNT) { free(fds, M_NTSYNC); free(objs, M_NTSYNC); free(fps, M_NTSYNC); }
    return (error);
}

static void
ntsync_dev_dtor(void *data)
{
    ntsync_dev_release((struct ntsync_device *)data);
    atomic_subtract_int(&ntsync_refs, 1);
}

static int ntsync_open(struct cdev *cdev, int oflags, int devtype, struct thread *td);
static int ntsync_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag, struct thread *td);
static int ntsync_close(struct cdev *dev, int fflag, int devtype, struct thread *td);

static struct cdevsw ntsync_cdevsw = {
    .d_version = D_VERSION,
    .d_name    = "ntsync",
    .d_open    = ntsync_open,
    .d_ioctl   = ntsync_ioctl,
    .d_close   = ntsync_close,
    .d_flags   = D_TRACKCLOSE,
};

static int
ntsync_open(struct cdev *cdev, int oflags, int devtype, struct thread *td)
{
    struct ntsync_device *dev = malloc(sizeof(*dev), M_NTSYNC, M_WAITOK | M_ZERO);
    refcount_init(&dev->refcount, 1);
    sx_init(&dev->wait_all_lock, "ntsync_wait_all");
    atomic_add_int(&ntsync_refs, 1);
    devfs_set_cdevpriv(dev, ntsync_dev_dtor);
    STAT(dev_opens);
    return (0);
}

static int
ntsync_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
    struct ntsync_device *dev;
    if (devfs_get_cdevpriv((void **)&dev)) return (ENXIO);
    struct ntsync_obj *obj;
    int fd, error;
    switch (cmd) {
    case NTSYNC_IOC_CREATE_SEM:
    case NTSYNC_IOC_CREATE_SEM_LINUX:
    case NTSYNC_IOC_CREATE_MUTEX:
    case NTSYNC_IOC_CREATE_MUTEX_LINUX:
    case NTSYNC_IOC_CREATE_EVENT:
    case NTSYNC_IOC_CREATE_EVENT_LINUX:
        obj = malloc(sizeof(*obj), M_NTSYNC, M_WAITOK | M_ZERO);
        mtx_init(&obj->lock, "ntsync_obj", NULL, MTX_DEF);
        TAILQ_INIT(&obj->any_waiters); TAILQ_INIT(&obj->all_waiters);
        obj->dev = dev; refcount_acquire(&dev->refcount);
        if (cmd == NTSYNC_IOC_CREATE_SEM || cmd == NTSYNC_IOC_CREATE_SEM_LINUX) {
            struct ntsync_sem_args *a = (void *)data;
            if (a->count > a->max) { error = EINVAL; goto fail; }
            obj->type = NTSYNC_TYPE_SEM; obj->u.sem.count = a->count; obj->u.sem.max = a->max;
            STAT(sem_creates);
        } else if (cmd == NTSYNC_IOC_CREATE_MUTEX || cmd == NTSYNC_IOC_CREATE_MUTEX_LINUX) {
            struct ntsync_mutex_args *a = (void *)data;
            if ((a->owner == 0) != (a->count == 0)) { error = EINVAL; goto fail; }
            obj->type = NTSYNC_TYPE_MUTEX; obj->u.mutex.owner = a->owner; obj->u.mutex.count = a->count;
            STAT(mutex_creates);
        } else {
            struct ntsync_event_args *a = (void *)data;
            obj->type = NTSYNC_TYPE_EVENT; obj->u.event.manual = a->manual; obj->u.event.signaled = a->signaled;
            STAT(event_creates);
        }
        error = ntsync_obj_get_fd(td, obj, &fd);
        if (error) goto fail;
        td->td_retval[0] = fd;
        return (0);
    fail:
        mtx_destroy(&obj->lock); ntsync_dev_release(dev); free(obj, M_NTSYNC);
        return (error);
    case NTSYNC_IOC_WAIT_ANY: return ntsync_wait_any(dev, td, (void *)data);
    case NTSYNC_IOC_WAIT_ALL: return ntsync_wait_all(dev, td, (void *)data);
    default: STAT(enotty); return (ENOTTY);
    }
}

static int ntsync_close(struct cdev *dev, int fflag, int devtype, struct thread *td) { return (0); }

static int
ntsync_modevent(module_t mod, int type, void *arg)
{
    switch (type) {
    case MOD_LOAD:
        ntsync_stats_init();
        ntsync_dev = make_dev(&ntsync_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "ntsync");
        break;
    case MOD_UNLOAD:
        if (atomic_load_acq_int(&ntsync_refs) > 0) return (EBUSY);
        destroy_dev(ntsync_dev); ntsync_stats_fini();
        break;
    default: return (EOPNOTSUPP);
    }
    return (0);
}

DEV_MODULE(ntsync, ntsync_modevent, NULL);
MODULE_VERSION(ntsync, 1);
