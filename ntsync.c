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
#include <sys/stdatomic.h>
#include <machine/atomic.h>

#include "ntsync_uapi.h"

MALLOC_DEFINE(M_NTSYNC, "ntsync", "NT sync objects");

#define NTSYNC_SIGNALED_NONE  (-1)

/* Rate-limited debug macro to prevent log spam */
static struct timeval ntsync_log_last;
static int ntsync_log_count;
#define NTSYNC_DEBUG(fmt, ...) do { \
    if (ppsratecheck(&ntsync_log_last, &ntsync_log_count, 1)) \
        printf("ntsync: " fmt "\n", ##__VA_ARGS__); \
} while (0)

enum ntsync_type {
    NTSYNC_TYPE_SEM,
    NTSYNC_TYPE_MUTEX,
    NTSYNC_TYPE_EVENT,
};

struct ntsync_device;
struct ntsync_q;

struct ntsync_q_entry {
    TAILQ_ENTRY(ntsync_q_entry) node;
    struct ntsync_q  *q;
    struct ntsync_obj *obj;
    uint32_t          index;
};

struct ntsync_q {
    volatile int     signaled;
    bool             all;
    bool             ownerdead;
    uint32_t         owner;
    uint32_t         count;

    struct mtx       lock;
    struct cv        cv;

    struct ntsync_q_entry entries[];
};

struct ntsync_obj {
    enum ntsync_type type;
    struct mtx       lock;
    int              dev_locked;
    struct ntsync_device *dev;

    volatile u_int   all_hint;

    TAILQ_HEAD(, ntsync_q_entry) any_waiters;
    TAILQ_HEAD(, ntsync_q_entry) all_waiters;

    union {
        struct { uint32_t count; uint32_t max; } sem;
        struct { uint32_t owner; uint32_t count; int abandoned; } mutex;
        struct { uint32_t manual; uint32_t signaled; } event;
    } u;
};

struct ntsync_device {
    u_int refcount;
    struct sx wait_all_lock;
};

static struct cdev *ntsync_dev;

static fo_ioctl_t  ntsync_obj_ioctl;
static fo_close_t  ntsync_obj_close;

static struct fileops ntsync_obj_ops = {
    .fo_read      = invfo_rdwr,
    .fo_write     = invfo_rdwr,
    .fo_truncate  = invfo_truncate,
    .fo_ioctl     = ntsync_obj_ioctl,
    .fo_poll      = invfo_poll,
    .fo_kqfilter  = invfo_kqfilter,
    .fo_stat      = invfo_stat,
    .fo_close     = ntsync_obj_close,
    .fo_chmod     = invfo_chmod,
    .fo_chown     = invfo_chown,
    .fo_sendfile  = invfo_sendfile,
};

static d_open_t  ntsync_open;
static d_ioctl_t ntsync_ioctl;
static d_close_t ntsync_close;

static struct cdevsw ntsync_cdevsw = {
    .d_version = D_VERSION,
    .d_name    = "ntsync",
    .d_open    = ntsync_open,
    .d_ioctl   = ntsync_ioctl,
    .d_close   = ntsync_close,
};

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
    if (all) {
        sx_xlock(&dev->wait_all_lock);
        obj->dev_locked = 1;
    }
    mtx_lock(&obj->lock);
    return all;
}

static void
ntsync_unlock_obj(struct ntsync_device *dev, struct ntsync_obj *obj, bool all)
{
    mtx_unlock(&obj->lock);
    if (all) {
        obj->dev_locked = 0;
        sx_xunlock(&dev->wait_all_lock);
    }
}

static void
dev_lock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    sx_assert(&dev->wait_all_lock, SA_XLOCKED);
    mtx_lock(&obj->lock);
    obj->dev_locked = 1;
}

static void
dev_unlock_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    sx_assert(&dev->wait_all_lock, SA_XLOCKED);
    obj->dev_locked = 0;
    mtx_unlock(&obj->lock);
}

static bool
is_signaled(struct ntsync_obj *obj, uint32_t owner)
{
    switch (obj->type) {
    case NTSYNC_TYPE_SEM:
        return obj->u.sem.count > 0;
    case NTSYNC_TYPE_MUTEX:
        if (obj->u.mutex.count == UINT32_MAX)
            return false;
        return obj->u.mutex.owner == 0 || obj->u.mutex.owner == owner;
    case NTSYNC_TYPE_EVENT:
        return obj->u.event.signaled != 0;
    }
    return false;
}

static void
try_wake_any_sem(struct ntsync_obj *sem)
{
    struct ntsync_q_entry *entry;
    TAILQ_FOREACH(entry, &sem->any_waiters, node) {
        struct ntsync_q *q = entry->q;
        int old = NTSYNC_SIGNALED_NONE;
        if (!sem->u.sem.count) break;

        if (atomic_cmpset_int(&q->signaled, (u_int)old, entry->index)) {
            sem->u.sem.count--;
            mtx_lock(&q->lock);
            cv_signal(&q->cv);
            mtx_unlock(&q->lock);
        }
    }
}

static void
try_wake_any_mutex(struct ntsync_obj *mutex)
{
    struct ntsync_q_entry *entry;
    TAILQ_FOREACH(entry, &mutex->any_waiters, node) {
        struct ntsync_q *q = entry->q;
        int old = NTSYNC_SIGNALED_NONE;
        if (mutex->u.mutex.count == UINT32_MAX) break;
        if (mutex->u.mutex.owner != 0 && mutex->u.mutex.owner != q->owner) continue;

        if (atomic_cmpset_int(&q->signaled, (u_int)old, entry->index)) {
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
    struct ntsync_q_entry *entry;
    TAILQ_FOREACH(entry, &event->any_waiters, node) {
        struct ntsync_q *q = entry->q;
        int old = NTSYNC_SIGNALED_NONE;
        if (!event->u.event.signaled) break;

        if (atomic_cmpset_int(&q->signaled, (u_int)old, entry->index)) {
            if (!event->u.event.manual) event->u.event.signaled = 0;
            mtx_lock(&q->lock);
            cv_signal(&q->cv);
            mtx_unlock(&q->lock);
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
    uint32_t count = q->count;
    bool can_wake = true;
    int old = NTSYNC_SIGNALED_NONE;
    uint32_t i, num_to_lock = 0;
    struct ntsync_obj *sorted_objs[NTSYNC_MAX_WAIT_COUNT];

    sx_assert(&dev->wait_all_lock, SA_XLOCKED);

    /* Collect all objects that need locking */
    for (i = 0; i < count; i++) {
        if (q->entries[i].obj != locked_obj)
            sorted_objs[num_to_lock++] = q->entries[i].obj;
    }

    /* Sort objects by memory address to prevent WITNESS Lock Order Reversal warnings */
    if (num_to_lock > 1) {
        for (i = 0; i < num_to_lock - 1; i++) {
            for (uint32_t j = 0; j < num_to_lock - i - 1; j++) {
                if (sorted_objs[j] > sorted_objs[j+1]) {
                    struct ntsync_obj *tmp = sorted_objs[j];
                    sorted_objs[j] = sorted_objs[j+1];
                    sorted_objs[j+1] = tmp;
                }
            }
        }
    }

    for (i = 0; i < num_to_lock; i++) dev_lock_obj(dev, sorted_objs[i]);

    for (i = 0; i < count; i++) {
        if (!is_signaled(q->entries[i].obj, q->owner)) {
            can_wake = false;
            break;
        }
    }

    if (can_wake && atomic_cmpset_int(&q->signaled, (u_int)old, 0)) {
        for (i = 0; i < count; i++) {
            struct ntsync_obj *obj = q->entries[i].obj;
            switch (obj->type) {
            case NTSYNC_TYPE_SEM:
                obj->u.sem.count--;
                break;
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

    for (i = 0; i < num_to_lock; i++) dev_unlock_obj(dev, sorted_objs[i]);
}

static void
try_wake_all_obj(struct ntsync_device *dev, struct ntsync_obj *obj)
{
    struct ntsync_q_entry *entry;
    sx_assert(&dev->wait_all_lock, SA_XLOCKED);
    TAILQ_FOREACH(entry, &obj->all_waiters, node)
        try_wake_all(dev, entry->q, obj);
}

static int
ntsync_obj_get_fd(struct thread *td, struct ntsync_obj *obj, int *fdp)
{
    struct file *fp;
    int fd, error;

    error = falloc(td, &fp, &fd, O_CLOEXEC);
    if (error) return (error);

    finit(fp, FREAD | FWRITE, DTYPE_NONE, obj, &ntsync_obj_ops);
    *fdp = fd;
    fdrop(fp, td);
    return (0);
}

static int
ntsync_get_obj(struct thread *td, int fd, struct ntsync_obj **objp, struct file **fpp)
{
    struct file *fp;
    int error;

    error = fget(td, fd, &cap_no_rights, &fp);
    if (error) return (error);

    if (fp->f_ops != &ntsync_obj_ops) {
        fdrop(fp, td);
        return (EINVAL);
    }

    *objp = fp->f_data;
    if (fpp != NULL) *fpp = fp;
    else fdrop(fp, td);

    return (0);
}

static int
ntsync_obj_close(struct file *fp, struct thread *td)
{
    struct ntsync_obj *obj = fp->f_data;
    if (obj == NULL) return (0);

    mtx_destroy(&obj->lock);
    ntsync_dev_release(obj->dev);
    free(obj, M_NTSYNC);
    fp->f_data = NULL;
    return (0);
}

static int
ntsync_obj_ioctl(struct file *fp, u_long cmd, void *data, struct ucred *active_cred, struct thread *td)
{
    struct ntsync_obj *obj = fp->f_data;
    struct ntsync_device *dev = obj->dev;
    bool all;
    int error = 0;

    switch (cmd) {

    case NTSYNC_IOC_SEM_RELEASE: {
        uint32_t *args = (uint32_t *)data;
        uint32_t count = *args;
        uint32_t prev;

        if (obj->type != NTSYNC_TYPE_SEM) return (EINVAL);

        all = ntsync_lock_obj(dev, obj);
        prev = obj->u.sem.count;
        if (count > obj->u.sem.max - prev) {
            ntsync_unlock_obj(dev, obj, all);
            NTSYNC_DEBUG("SEM_RELEASE overflow: count=%u max=%u", count, obj->u.sem.max);
            return (EOVERFLOW);
        }
        obj->u.sem.count += count;

        /* Wake 'all' waiters first, then 'any' waiters */
        if (all) try_wake_all_obj(dev, obj);
        try_wake_any_sem(obj);

        ntsync_unlock_obj(dev, obj, all);
        *args = prev;
        return (0);
    }

    case NTSYNC_IOC_SEM_READ:
    case NTSYNC_IOC_SEM_READ_LINUX: {
        struct ntsync_sem_args *args = (struct ntsync_sem_args *)data;
        if (obj->type != NTSYNC_TYPE_SEM) return (EINVAL);
        mtx_lock(&obj->lock);
        args->count = obj->u.sem.count;
        args->max   = obj->u.sem.max;
        mtx_unlock(&obj->lock);
        return (0);
    }

    case NTSYNC_IOC_MUTEX_UNLOCK: {
        struct ntsync_mutex_args *args = (struct ntsync_mutex_args *)data;
        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);
        if (args->owner == 0) return (EINVAL);

        all = ntsync_lock_obj(dev, obj);
        if (obj->u.mutex.owner != args->owner) {
            error = EPERM;
        } else if (obj->u.mutex.count == 0) {
            error = EINVAL;
        } else {
            args->count = obj->u.mutex.count--;
            if (obj->u.mutex.count == 0) {
                obj->u.mutex.owner = 0;
                if (all) try_wake_all_obj(dev, obj);
                try_wake_any_mutex(obj);
            }
        }
        ntsync_unlock_obj(dev, obj, all);
        return (error);
    }

    case NTSYNC_IOC_MUTEX_KILL:
    case NTSYNC_IOC_MUTEX_KILL_LINUX: {
        uint32_t owner = *(uint32_t *)data;
        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);
        if (owner == 0) return (EINVAL);

        all = ntsync_lock_obj(dev, obj);
        if (obj->u.mutex.owner != owner) {
            error = EPERM;
        } else {
            obj->u.mutex.owner     = 0;
            obj->u.mutex.count     = 0;
            obj->u.mutex.abandoned = 1;
            
            if (all) try_wake_all_obj(dev, obj);
            try_wake_any_mutex(obj);
        }
        ntsync_unlock_obj(dev, obj, all);
        return (error);
    }

    case NTSYNC_IOC_MUTEX_READ:
    case NTSYNC_IOC_MUTEX_READ_LINUX: {
        struct ntsync_mutex_args *args = (struct ntsync_mutex_args *)data;
        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);
        mtx_lock(&obj->lock);
        if (obj->u.mutex.abandoned) {
            args->count = 0; args->owner = 0;
            error = EOWNERDEAD;
        } else {
            args->count = obj->u.mutex.count;
            args->owner = obj->u.mutex.owner;
        }
        mtx_unlock(&obj->lock);
        return (error);
    }

    case NTSYNC_IOC_EVENT_SET:
    case NTSYNC_IOC_EVENT_SET_LINUX: {
        uint32_t *args = (uint32_t *)data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        all = ntsync_lock_obj(dev, obj);
        *args = obj->u.event.signaled;
        obj->u.event.signaled = 1;

        if (all) try_wake_all_obj(dev, obj);
        try_wake_any_event(obj);

        ntsync_unlock_obj(dev, obj, all);
        return (0);
    }

    case NTSYNC_IOC_EVENT_RESET:
    case NTSYNC_IOC_EVENT_RESET_LINUX: {
        uint32_t *args = (uint32_t *)data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        all = ntsync_lock_obj(dev, obj);
        *args = obj->u.event.signaled;
        obj->u.event.signaled = 0;
        ntsync_unlock_obj(dev, obj, all);
        return (0);
    }

    case NTSYNC_IOC_EVENT_PULSE:
    case NTSYNC_IOC_EVENT_PULSE_LINUX: {
        uint32_t *args = (uint32_t *)data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        all = ntsync_lock_obj(dev, obj);
        *args = obj->u.event.signaled;
        obj->u.event.signaled = 1;

        if (all) try_wake_all_obj(dev, obj);
        try_wake_any_event(obj);

        obj->u.event.signaled = 0;
        ntsync_unlock_obj(dev, obj, all);
        return (0);
    }

    case NTSYNC_IOC_EVENT_READ:
    case NTSYNC_IOC_EVENT_READ_LINUX: {
        struct ntsync_event_args *args = (struct ntsync_event_args *)data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);
        mtx_lock(&obj->lock);
        args->manual   = obj->u.event.manual;
        args->signaled = obj->u.event.signaled;
        mtx_unlock(&obj->lock);
        return (0);
    }

    default:
        NTSYNC_DEBUG("obj_ioctl: unhandled cmd=0x%lx", cmd);
        return (ENOTTY);
    }
}

static int
ntsync_schedule(struct ntsync_q *q, uint64_t timeout_ns, int flags)
{
    int error = 0;
    sbintime_t sbt;

    mtx_lock(&q->lock);

    for (;;) {
        if ((int)atomic_load_acq_int((volatile u_int *)&q->signaled) != NTSYNC_SIGNALED_NONE) {
            error = 0;
            break;
        }

        if (timeout_ns == UINT64_MAX) {
            error = cv_wait_sig(&q->cv, &q->lock);
            if (error == ERESTART) error = EINTR;
            if (error) break;
            continue;
        }

        sbt = nstosbt(timeout_ns);

        if (flags & NTSYNC_WAIT_REALTIME) {
            struct timespec real_ts, mono_ts;
            nanotime(&real_ts);
            nanouptime(&mono_ts);
            sbintime_t real_sbt = tstosbt(real_ts);
            sbintime_t mono_sbt = tstosbt(mono_ts);

            if (sbt <= real_sbt) {
                error = ETIMEDOUT;
                break;
            }
            sbt = mono_sbt + (sbt - real_sbt);
        }

        error = cv_timedwait_sig_sbt(&q->cv, &q->lock, sbt, 0, C_ABSOLUTE);
        if (error == EWOULDBLOCK) { error = ETIMEDOUT; break; }
        if (error == ERESTART)    { error = EINTR;     break; }
        if (error) break;
    }

    mtx_unlock(&q->lock);
    return error;
}

static struct ntsync_q *
ntsync_alloc_q(struct thread *td, const struct ntsync_wait_args *args,
               struct ntsync_obj **objs, struct file **fps, uint32_t total)
{
    struct ntsync_q *q;
    size_t qsz = sizeof(*q) + total * sizeof(q->entries[0]);
    q = malloc(qsz, M_NTSYNC, M_WAITOK | M_ZERO);

    atomic_store_rel_int((volatile u_int *)&q->signaled, (u_int)NTSYNC_SIGNALED_NONE);
    q->all      = false;
    q->ownerdead= false;
    q->owner    = args->owner;
    q->count    = args->count;
    mtx_init(&q->lock, "ntsync_q", NULL, MTX_DEF);
    cv_init(&q->cv, "ntsync_wait");

    for (uint32_t i = 0; i < total; i++) {
        q->entries[i].q     = q;
        q->entries[i].obj   = objs[i];
        q->entries[i].index = i;
    }

    return q;
}

static void
ntsync_free_q(struct ntsync_q *q)
{
    cv_destroy(&q->cv);
    mtx_destroy(&q->lock);
    free(q, M_NTSYNC);
}

static int
ntsync_wait_any(struct ntsync_device *dev, struct thread *td, struct ntsync_wait_args *args)
{
    uint32_t total = args->count + (args->alert ? 1 : 0);
    struct ntsync_obj **objs;
    struct file       **fps;
    uint32_t           *fds;
    struct ntsync_q    *q;
    int error = 0;
    uint32_t i;

    if (args->pad || (args->flags & ~NTSYNC_WAIT_REALTIME)) return (EINVAL);
    if (args->count > NTSYNC_MAX_WAIT_COUNT) {
        NTSYNC_DEBUG("wait_any: too many objects %u", args->count);
        return (EINVAL);
    }

    if (total == 0) {
        struct ntsync_q dummy = {};
        atomic_store_rel_int((volatile u_int *)&dummy.signaled, (u_int)NTSYNC_SIGNALED_NONE);
        mtx_init(&dummy.lock, "ntsync_q", NULL, MTX_DEF);
        cv_init(&dummy.cv, "ntsync_wait");
        error = ntsync_schedule(&dummy, args->timeout, args->flags);
        cv_destroy(&dummy.cv);
        mtx_destroy(&dummy.lock);
        return (error == 0 ? ETIMEDOUT : error);
    }

    fds  = malloc(total * sizeof(*fds),  M_NTSYNC, M_WAITOK);
    objs = malloc(total * sizeof(*objs), M_NTSYNC, M_WAITOK | M_ZERO);
    fps  = malloc(total * sizeof(*fps),  M_NTSYNC, M_WAITOK | M_ZERO);

    if (args->count > 0) {
        error = copyin((void *)(uintptr_t)args->objs, fds, args->count * sizeof(uint32_t));
        if (error) goto out_free;
    }
    if (args->alert) fds[args->count] = args->alert;

    for (i = 0; i < total; i++) {
        error = ntsync_get_obj(td, fds[i], &objs[i], &fps[i]);
        if (error) { NTSYNC_DEBUG("wait_any: invalid object fd=%u", fds[i]); goto out_objs; }
        if (objs[i]->dev != dev) { NTSYNC_DEBUG("wait_any: object from wrong dev fd=%u", fds[i]); error = EINVAL; i++; goto out_objs; }
    }

    q = ntsync_alloc_q(td, args, objs, fps, total);

    /* Queue ourselves onto each object's any_waiters list */
    for (i = 0; i < total; i++) {
        bool all = ntsync_lock_obj(dev, objs[i]);
        TAILQ_INSERT_TAIL(&objs[i]->any_waiters, &q->entries[i], node);
        ntsync_unlock_obj(dev, objs[i], all);
    }

    /* Check if already signaled */
    for (i = 0; i < total; i++) {
        if ((int)atomic_load_acq_int((volatile u_int *)&q->signaled) != NTSYNC_SIGNALED_NONE) break;
        bool all = ntsync_lock_obj(dev, objs[i]);
        try_wake_any_obj(objs[i]);
        ntsync_unlock_obj(dev, objs[i], all);
    }

    error = ntsync_schedule(q, args->timeout, args->flags);

    /* Dequeue from objects */
    for (i = 0; i < total; i++) {
        bool all = ntsync_lock_obj(dev, objs[i]);
        TAILQ_REMOVE(&objs[i]->any_waiters, &q->entries[i], node);
        ntsync_unlock_obj(dev, objs[i], all);
    }

    int sig = (int)atomic_load_acq_int((volatile u_int *)&q->signaled);
    if (sig != NTSYNC_SIGNALED_NONE) {
        error = q->ownerdead ? EOWNERDEAD : 0;
        args->index = (uint32_t)sig;
    } else if (error == 0) {
        error = ETIMEDOUT;
    }

    ntsync_free_q(q);

out_objs:
    for (i = 0; i < total; i++) if (fps[i]) fdrop(fps[i], td);
out_free:
    free(objs, M_NTSYNC);
    free(fps,  M_NTSYNC);
    free(fds,  M_NTSYNC);
    return (error);
}

static int
ntsync_wait_all(struct ntsync_device *dev, struct thread *td, struct ntsync_wait_args *args)
{
    uint32_t total = args->count + (args->alert ? 1 : 0);
    struct ntsync_obj **objs;
    struct file       **fps;
    uint32_t           *fds;
    struct ntsync_q    *q;
    int error = 0;
    uint32_t i;

    if (args->pad || (args->flags & ~NTSYNC_WAIT_REALTIME)) return (EINVAL);
    if (args->count > NTSYNC_MAX_WAIT_COUNT) {
        NTSYNC_DEBUG("wait_all: too many objects %u", args->count);
        return (EINVAL);
    }

    if (total == 0) {
        struct ntsync_q dummy = {};
        atomic_store_rel_int((volatile u_int *)&dummy.signaled, (u_int)NTSYNC_SIGNALED_NONE);
        mtx_init(&dummy.lock, "ntsync_q", NULL, MTX_DEF);
        cv_init(&dummy.cv, "ntsync_wait");
        error = ntsync_schedule(&dummy, args->timeout, args->flags);
        cv_destroy(&dummy.cv);
        mtx_destroy(&dummy.lock);
        return (error == 0 ? ETIMEDOUT : error);
    }

    fds  = malloc(total * sizeof(*fds),  M_NTSYNC, M_WAITOK);
    objs = malloc(total * sizeof(*objs), M_NTSYNC, M_WAITOK | M_ZERO);
    fps  = malloc(total * sizeof(*fps),  M_NTSYNC, M_WAITOK | M_ZERO);

    if (args->count > 0) {
        error = copyin((void *)(uintptr_t)args->objs, fds, args->count * sizeof(uint32_t));
        if (error) goto out_free;
    }
    if (args->alert) fds[args->count] = args->alert;

    for (i = 0; i < total; i++) {
        error = ntsync_get_obj(td, fds[i], &objs[i], &fps[i]);
        if (error) { NTSYNC_DEBUG("wait_all: invalid object fd=%u", fds[i]); goto out_objs; }
        if (objs[i]->dev != dev) { NTSYNC_DEBUG("wait_all: object from wrong dev fd=%u", fds[i]); error = EINVAL; i++; goto out_objs; }
        
        /* Check that the objects are all distinct */
        for (uint32_t j = 0; j < i; j++) {
            if (objs[i] == objs[j] && i < args->count) {
                NTSYNC_DEBUG("wait_all: duplicate object detected");
                error = EINVAL; 
                i++; 
                goto out_objs;
            }
        }
    }

    q = ntsync_alloc_q(td, args, objs, fps, total);
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

    if (args->alert && (int)atomic_load_acq_int((volatile u_int *)&q->signaled) == NTSYNC_SIGNALED_NONE) {
        bool all = ntsync_lock_obj(dev, objs[args->count]);
        try_wake_any_obj(objs[args->count]);
        ntsync_unlock_obj(dev, objs[args->count], all);
    }

    error = ntsync_schedule(q, args->timeout, args->flags);

    sx_xlock(&dev->wait_all_lock);
    for (i = 0; i < args->count; i++) {
        TAILQ_REMOVE(&objs[i]->all_waiters, &q->entries[i], node);
        atomic_subtract_int(&objs[i]->all_hint, 1);
    }
    sx_xunlock(&dev->wait_all_lock);

    if (args->alert) {
        bool all = ntsync_lock_obj(dev, objs[args->count]);
        TAILQ_REMOVE(&objs[args->count]->any_waiters, &q->entries[args->count], node);
        ntsync_unlock_obj(dev, objs[args->count], all);
    }

    int sig = (int)atomic_load_acq_int((volatile u_int *)&q->signaled);
    if (sig != NTSYNC_SIGNALED_NONE) {
        error = q->ownerdead ? EOWNERDEAD : 0;
        args->index = (uint32_t)sig;
    } else if (error == 0) {
        error = ETIMEDOUT;
    }

    ntsync_free_q(q);

out_objs:
    for (i = 0; i < total; i++) if (fps[i]) fdrop(fps[i], td);
out_free:
    free(objs, M_NTSYNC);
    free(fps,  M_NTSYNC);
    free(fds,  M_NTSYNC);
    return (error);
}

static void
ntsync_dev_dtor(void *data)
{
    ntsync_dev_release((struct ntsync_device *)data);
}

static int
ntsync_open(struct cdev *cdev, int oflags, int devtype, struct thread *td)
{
    struct ntsync_device *dev;
    int error;

    dev = malloc(sizeof(*dev), M_NTSYNC, M_WAITOK | M_ZERO);
    refcount_init(&dev->refcount, 1);
    sx_init(&dev->wait_all_lock, "ntsync_wait_all");

    error = devfs_set_cdevpriv(dev, ntsync_dev_dtor);
    if (error) {
        ntsync_dev_dtor(dev);
        return (error);
    }
    return (0);
}

static int
ntsync_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
    struct ntsync_device *dev;
    struct ntsync_obj *obj;
    int error, fd;

    error = devfs_get_cdevpriv((void **)&dev);
    if (error) return (error);

    switch (cmd) {

    case NTSYNC_IOC_CREATE_SEM:
    case NTSYNC_IOC_CREATE_SEM_LINUX:
    case NTSYNC_IOC_CREATE_MUTEX:
    case NTSYNC_IOC_CREATE_MUTEX_LINUX:
    case NTSYNC_IOC_CREATE_EVENT:
    case NTSYNC_IOC_CREATE_EVENT_LINUX:
        obj = malloc(sizeof(*obj), M_NTSYNC, M_WAITOK | M_ZERO);
        mtx_init(&obj->lock, "ntsync_obj", NULL, MTX_DEF);
        TAILQ_INIT(&obj->any_waiters);
        TAILQ_INIT(&obj->all_waiters);
        obj->dev = dev;
        refcount_acquire(&dev->refcount);

        if (cmd == NTSYNC_IOC_CREATE_SEM || cmd == NTSYNC_IOC_CREATE_SEM_LINUX) {
            struct ntsync_sem_args *a = (struct ntsync_sem_args *)data;
            if (a->count > a->max) { 
                NTSYNC_DEBUG("CREATE_SEM invalid args: count=%u max=%u", a->count, a->max); 
                error = EINVAL; 
                goto alloc_fail; 
            }
            obj->type = NTSYNC_TYPE_SEM;
            obj->u.sem.count = a->count;
            obj->u.sem.max   = a->max;
        } else if (cmd == NTSYNC_IOC_CREATE_MUTEX || cmd == NTSYNC_IOC_CREATE_MUTEX_LINUX) {
            struct ntsync_mutex_args *a = (struct ntsync_mutex_args *)data;
            if ((a->owner == 0) != (a->count == 0)) { 
                NTSYNC_DEBUG("CREATE_MUTEX invalid state: owner=%u count=%u", a->owner, a->count);
                error = EINVAL; 
                goto alloc_fail; 
            }
            obj->type = NTSYNC_TYPE_MUTEX;
            obj->u.mutex.owner = a->owner;
            obj->u.mutex.count = a->count;
        } else {
            struct ntsync_event_args *a = (struct ntsync_event_args *)data;
            obj->type = NTSYNC_TYPE_EVENT;
            obj->u.event.manual   = a->manual;
            obj->u.event.signaled = a->signaled;
        }

        error = ntsync_obj_get_fd(td, obj, &fd);
        if (error) goto alloc_fail;

        td->td_retval[0] = fd;
        return (0);

    alloc_fail:
        mtx_destroy(&obj->lock);
        ntsync_dev_release(dev);
        free(obj, M_NTSYNC);
        return (error);

    case NTSYNC_IOC_WAIT_ANY:
        return ntsync_wait_any(dev, td, (struct ntsync_wait_args *)data);

    case NTSYNC_IOC_WAIT_ALL:
        return ntsync_wait_all(dev, td, (struct ntsync_wait_args *)data);

    default:
        NTSYNC_DEBUG("ioctl: unhandled cmd=0x%lx", cmd);
        return (ENOTTY);
    }
}

static int
ntsync_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
    return (0);
}

static int
ntsync_modevent(module_t mod, int type, void *arg)
{
    switch (type) {
    case MOD_LOAD:
        ntsync_dev = make_dev(&ntsync_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "ntsync");
        printf("ntsync: up and running.\n");
        break;
    case MOD_UNLOAD:
        destroy_dev(ntsync_dev);
        printf("ntsync: unloaded.\n");
        break;
    default:
        return (EOPNOTSUPP);
    }
    return (0);
}

