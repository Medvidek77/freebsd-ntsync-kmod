/*
 * FreeBSD ntsync.ko - NT synchronization primitive emulation
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

#include "ntsync_uapi.h"

MALLOC_DEFINE(M_NTSYNC, "ntsync", "NT sync objects");

/* Object types */
enum ntsync_type {
    NTSYNC_TYPE_SEM,
    NTSYNC_TYPE_MUTEX,
    NTSYNC_TYPE_EVENT,
};

struct ntsync_device {
    u_int refcount;
    struct mtx wait_all_lock;
    struct cv cv;          /* Global cv for WAIT_ANY/ALL to sleep on */
};

static void ntsync_dev_release(struct ntsync_device *dev)
{
    if (refcount_release(&dev->refcount)) {
        mtx_destroy(&dev->wait_all_lock);
        cv_destroy(&dev->cv);
        free(dev, M_NTSYNC);
    }
}

/* Internal representation of a synchronization object */
struct ntsync_obj {
    enum ntsync_type type;
    struct mtx lock;       /* protects object state */
    struct ntsync_device *dev; /* parent device for waking up waits */
    union {
        struct {
            uint32_t count;
            uint32_t max;
        } sem;
        struct {
            uint32_t owner;
            uint32_t count;
            int      abandoned; /* tracking EOWNERDEAD */
        } mutex;
        struct {
            uint32_t manual;
            uint32_t signaled;
        } event;
    } u;
};

/* Device node for /dev/ntsync */
static struct cdev *ntsync_dev;

/* Forward declarations */
static fo_ioctl_t    ntsync_obj_ioctl;
static fo_close_t    ntsync_obj_close;
static fo_stat_t     ntsync_obj_stat;

static struct fileops ntsync_obj_ops = {
    .fo_read = invfo_rdwr,
    .fo_write = invfo_rdwr,
    .fo_truncate = invfo_truncate,
    .fo_ioctl = ntsync_obj_ioctl,
    .fo_poll = invfo_poll,
    .fo_kqfilter = invfo_kqfilter,
    .fo_stat = ntsync_obj_stat,
    .fo_close = ntsync_obj_close,
    .fo_chmod = invfo_chmod,
    .fo_chown = invfo_chown,
    .fo_sendfile = invfo_sendfile,
};

static int
ntsync_obj_stat(struct file *fp, struct stat *sb, struct ucred *active_cred)
{
    return (ENXIO);
}

static d_open_t      ntsync_open;
static d_ioctl_t     ntsync_ioctl;
static d_close_t     ntsync_close;

static struct cdevsw ntsync_cdevsw = {
    .d_version = D_VERSION,
    .d_name = "ntsync",
    .d_open = ntsync_open,
    .d_ioctl = ntsync_ioctl,
    .d_close = ntsync_close,
};

/* Helper to allocate a file descriptor for an ntsync_obj */
static int
ntsync_obj_get_fd(struct thread *td, struct ntsync_obj *obj, int *fdp)
{
    struct file *fp;
    int fd, error;

    error = falloc(td, &fp, &fd, O_CLOEXEC);
    if (error)
        return (error);

    finit(fp, FREAD | FWRITE, DTYPE_NONE, obj, &ntsync_obj_ops);

    *fdp = fd;
    fdrop(fp, td);

    return (0);
}

/* Helper to lookup ntsync_obj from fd */
static int
ntsync_get_obj(struct thread *td, int fd, struct ntsync_obj **objp, struct file **fpp)
{
    struct file *fp;
    int error;

    /* Get file structure from fd without cap rights restriction for now */
    error = fget(td, fd, &cap_no_rights, &fp);
    if (error)
        return (error);

    if (fp->f_ops != &ntsync_obj_ops) {
        fdrop(fp, td);
        return (EINVAL);
    }

    *objp = fp->f_data;
    if (fpp != NULL)
        *fpp = fp;
    else
        fdrop(fp, td);

    return (0);
}

static int
ntsync_obj_close(struct file *fp, struct thread *td)
{
    struct ntsync_obj *obj = fp->f_data;

    if (obj == NULL)
        return (0);

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
    int error = 0;

    switch (cmd) {
    case NTSYNC_IOC_SEM_RELEASE: {
        uint32_t *args = (uint32_t *)data;
        uint32_t count = *args;
        uint32_t prev_count;

        if (obj->type != NTSYNC_TYPE_SEM) return (EINVAL);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);
        prev_count = obj->u.sem.count;
        if (count > obj->u.sem.max - prev_count) {
            mtx_unlock(&obj->lock);
            mtx_unlock(&obj->dev->wait_all_lock);
            return (EOVERFLOW);
        }
        obj->u.sem.count += count;
        if (prev_count == 0 && count > 0)
            cv_broadcast(&obj->dev->cv);
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);

        *args = prev_count;
        return (0);
    }
    case NTSYNC_IOC_SEM_READ: {
        struct ntsync_sem_args *args = (struct ntsync_sem_args *)data;
        if (obj->type != NTSYNC_TYPE_SEM) return (EINVAL);

        mtx_lock(&obj->lock);
        args->count = obj->u.sem.count;
        args->max = obj->u.sem.max;
        mtx_unlock(&obj->lock);
        return (0);
    }

    case NTSYNC_IOC_MUTEX_UNLOCK: {
        struct ntsync_mutex_args *args = (struct ntsync_mutex_args *)data;
        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);
        if (args->owner == 0) return (EINVAL);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);
        if (obj->u.mutex.owner != args->owner) {
            error = EPERM;
        } else {
            args->count = obj->u.mutex.count;
            if (obj->u.mutex.count == 0) {
                error = EINVAL;
            } else {
                obj->u.mutex.count--;
                if (obj->u.mutex.count == 0) {
                    obj->u.mutex.owner = 0;
                    /* clear abandoned flag since it was properly unlocked */
                    obj->u.mutex.abandoned = 0;
                    cv_broadcast(&obj->dev->cv);
                }
            }
        }
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);
        return (error);
    }
    case NTSYNC_IOC_MUTEX_KILL: {
        uint32_t *owner_ptr = (uint32_t *)data;
        uint32_t owner = *owner_ptr;

        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);
        if (owner == 0) return (EINVAL);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);
        if (obj->u.mutex.owner != owner) {
            error = EPERM;
        } else {
            /* Mutex abandoned by owner (process died etc) */
            obj->u.mutex.owner = 0;
            obj->u.mutex.count = 0;
            obj->u.mutex.abandoned = 1;
            cv_broadcast(&obj->dev->cv);
        }
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);
        return (error);
    }
    case NTSYNC_IOC_MUTEX_READ: {
        struct ntsync_mutex_args *args = (struct ntsync_mutex_args *)data;
        if (obj->type != NTSYNC_TYPE_MUTEX) return (EINVAL);

        mtx_lock(&obj->lock);
        if (obj->u.mutex.abandoned) {
            args->count = 0;
            args->owner = 0;
            error = EOWNERDEAD;
        } else {
            args->count = obj->u.mutex.count;
            args->owner = obj->u.mutex.owner;
        }
        mtx_unlock(&obj->lock);
        return (error);
    }

    case NTSYNC_IOC_EVENT_SET: {
        uint32_t *args = (uint32_t *)data;
        uint32_t prev_state;

        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);
        prev_state = obj->u.event.signaled;
        obj->u.event.signaled = 1;
        if (prev_state == 0) {
            cv_broadcast(&obj->dev->cv);
        }
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);

        *args = prev_state;
        return (0);
    }
    case NTSYNC_IOC_EVENT_RESET: {
        uint32_t *args = (uint32_t *)data;
        uint32_t prev_state;

        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);
        prev_state = obj->u.event.signaled;
        obj->u.event.signaled = 0;
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);

        *args = prev_state;
        return (0);
    }
    case NTSYNC_IOC_EVENT_PULSE: {
        uint32_t *args = (uint32_t *)data;
        uint32_t prev_state;

        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);
        prev_state = obj->u.event.signaled;
        obj->u.event.signaled = 1;
        cv_broadcast(&obj->dev->cv);

        /* Pause briefly so waiting threads can wake up and check the condition.
         * We MUST drop wait_all_lock here because wait_any/wait_all loops
         * need it to re-check the signaled state!
         */
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);

        pause_sbt("ntspulse", SBT_1MS, 0, C_CATCH);

        mtx_lock(&obj->dev->wait_all_lock);
        mtx_lock(&obj->lock);

        obj->u.event.signaled = 0;
        mtx_unlock(&obj->lock);
        mtx_unlock(&obj->dev->wait_all_lock);

        *args = prev_state;
        return (0);
    }
    case NTSYNC_IOC_EVENT_READ: {
        struct ntsync_event_args *args = (struct ntsync_event_args *)data;
        if (obj->type != NTSYNC_TYPE_EVENT) return (EINVAL);

        mtx_lock(&obj->lock);
        args->manual = obj->u.event.manual;
        args->signaled = obj->u.event.signaled;
        mtx_unlock(&obj->lock);
        return (0);
    }

    default:
        printf("ntsync: Unknown Object IOCTL 0x%lx\n", cmd);
        return (ENOTTY);
    }
}

/* Timeout conversion helper */
static int
ntsync_wait_timeout(struct cv *cv, struct mtx *lock, uint64_t timeout_ns, int flags)
{
    struct timespec ts, now;
    struct timeval tv;
    int ticks_left, error;

    if (timeout_ns == 0)
        return (EWOULDBLOCK);

    if (timeout_ns == UINT64_MAX) {
        /* infinite wait */
        return cv_wait_sig(cv, lock);
    }

    if (flags & NTSYNC_WAIT_REALTIME) {
        nanotime(&now);
    } else {
        nanouptime(&now);
    }
    ts.tv_sec  = timeout_ns / 1000000000ULL;
    ts.tv_nsec = timeout_ns % 1000000000ULL;

    if (timespeccmp(&ts, &now, <=))
        return (EWOULDBLOCK);
    timespecsub(&ts, &now, &ts);

    TIMESPEC_TO_TIMEVAL(&tv, &ts);
    ticks_left = tvtohz(&tv);
    if (ticks_left == 0)
        ticks_left = 1;

    error = cv_timedwait_sig(cv, lock, ticks_left);
    return (error == EWOULDBLOCK ? ETIMEDOUT : error);
}

/*
 * Check if object is signaled and acquire it if possible.
 * Returns:
 *   1 if acquired
 *   0 if not acquired
 *   -EOWNERDEAD if mutex was abandoned and is now acquired by us
 */
static int
ntsync_try_acquire(struct ntsync_obj *obj, uint32_t owner)
{
    int ret = 0;

    mtx_lock(&obj->lock);
    switch (obj->type) {
    case NTSYNC_TYPE_SEM:
        if (obj->u.sem.count > 0) {
            obj->u.sem.count--;
            ret = 1;
        }
        break;

    case NTSYNC_TYPE_MUTEX:
        if (obj->u.mutex.owner == 0 || obj->u.mutex.owner == owner) {
            obj->u.mutex.owner = owner;
            obj->u.mutex.count++;
            if (obj->u.mutex.abandoned) {
                obj->u.mutex.abandoned = 0;
                ret = -EOWNERDEAD;
            } else {
                ret = 1;
            }
        }
        break;

    case NTSYNC_TYPE_EVENT:
        if (obj->u.event.signaled) {
            if (!obj->u.event.manual)
                obj->u.event.signaled = 0;
            ret = 1;
        }
        break;
    }
    mtx_unlock(&obj->lock);

    return (ret);
}

static int
ntsync_wait_any(struct ntsync_device *dev, struct thread *td, struct ntsync_wait_args *args)
{
    struct ntsync_obj **objs;
    struct file **fps;
    uint32_t *fds;
    int i, error = 0, acquired_idx = -1;
    uint64_t timeout_ns = args->timeout;
    int ownerdead = 0;

    if (args->pad || (args->flags & ~NTSYNC_WAIT_REALTIME))
        return (EINVAL);

    if (args->count == 0 || args->count > NTSYNC_MAX_WAIT_COUNT)
        return (EINVAL);

    fds = malloc(args->count * sizeof(uint32_t), M_NTSYNC, M_WAITOK);
    error = copyin((void *)(uintptr_t)args->objs, fds, args->count * sizeof(uint32_t));
    if (error) {
        free(fds, M_NTSYNC);
        return (error);
    }

    objs = malloc(args->count * sizeof(struct ntsync_obj *), M_NTSYNC, M_WAITOK | M_ZERO);
    fps = malloc(args->count * sizeof(struct file *), M_NTSYNC, M_WAITOK | M_ZERO);

    /* Lookup all objects */
    for (i = 0; i < args->count; i++) {
        error = ntsync_get_obj(td, fds[i], &objs[i], &fps[i]);
        if (error) {
            goto out;
        }
        if (objs[i]->dev != dev) {
            error = EINVAL;
            goto out;
        }
    }

    mtx_lock(&dev->wait_all_lock);

    /* Wait loop */
    for (;;) {
        /* Check if any object can be acquired */
        for (i = 0; i < args->count; i++) {
            int acq = ntsync_try_acquire(objs[i], args->owner);
            if (acq == 1 || acq == -EOWNERDEAD) {
                acquired_idx = i;
                if (acq == -EOWNERDEAD)
                    ownerdead = 1;
                break;
            }
        }

        if (acquired_idx != -1) {
            args->index = acquired_idx;
            error = ownerdead ? EOWNERDEAD : 0;
            break;
        }

        /* If non-blocking wait */
        if (timeout_ns == 0) {
            error = ETIMEDOUT;
            break;
        }

        error = ntsync_wait_timeout(&dev->cv, &dev->wait_all_lock, timeout_ns, args->flags);

        if (error == EINTR || error == ERESTART) {
            if (error == ERESTART) error = EINTR;
            break;
        }
        if (error == ETIMEDOUT) {
            break;
        }
    }

    mtx_unlock(&dev->wait_all_lock);

out:
    /* Drop file references */
    for (i = 0; i < args->count; i++) {
        if (fps[i] != NULL)
            fdrop(fps[i], td);
    }
    free(objs, M_NTSYNC);
    free(fps, M_NTSYNC);
    free(fds, M_NTSYNC);
    return (error);
}

static int
ntsync_wait_all(struct ntsync_device *dev, struct thread *td, struct ntsync_wait_args *args)
{
    struct ntsync_obj **objs;
    struct file **fps;
    uint32_t *fds;
    int i, error = 0;
    uint64_t timeout_ns = args->timeout;
    int ownerdead = 0;

    if (args->pad || (args->flags & ~NTSYNC_WAIT_REALTIME))
        return (EINVAL);

    if (args->count == 0 || args->count > NTSYNC_MAX_WAIT_COUNT)
        return (EINVAL);

    fds = malloc(args->count * sizeof(uint32_t), M_NTSYNC, M_WAITOK);
    error = copyin((void *)(uintptr_t)args->objs, fds, args->count * sizeof(uint32_t));
    if (error) {
        free(fds, M_NTSYNC);
        return (error);
    }

    objs = malloc(args->count * sizeof(struct ntsync_obj *), M_NTSYNC, M_WAITOK | M_ZERO);
    fps = malloc(args->count * sizeof(struct file *), M_NTSYNC, M_WAITOK | M_ZERO);

    /* Lookup all objects */
    for (i = 0; i < args->count; i++) {
        error = ntsync_get_obj(td, fds[i], &objs[i], &fps[i]);
        if (error) {
            goto out;
        }
        if (objs[i]->dev != dev) {
            error = EINVAL;
            goto out;
        }
    }

    mtx_lock(&dev->wait_all_lock);

    /* Wait loop */
    for (;;) {
        int all_signaled = 1;
        ownerdead = 0;

        /* Atomically check if ALL objects are signaled */
        for (i = 0; i < args->count; i++) {
            struct ntsync_obj *obj = objs[i];
            int acq = 0;

            mtx_lock(&obj->lock);
            switch (obj->type) {
            case NTSYNC_TYPE_SEM:
                if (obj->u.sem.count > 0) acq = 1;
                break;
            case NTSYNC_TYPE_MUTEX:
                if (obj->u.mutex.owner == 0 || obj->u.mutex.owner == args->owner) {
                    acq = 1;
                }
                break;
            case NTSYNC_TYPE_EVENT:
                if (obj->u.event.signaled) acq = 1;
                break;
            }
            mtx_unlock(&obj->lock);

            if (!acq) {
                all_signaled = 0;
                break; /* fast fail if any is not signaled */
            }
        }

        /* If all signaled, acquire them all */
        if (all_signaled) {
            for (i = 0; i < args->count; i++) {
                struct ntsync_obj *obj = objs[i];
                mtx_lock(&obj->lock);
                switch (obj->type) {
                case NTSYNC_TYPE_SEM:
                    obj->u.sem.count--;
                    break;
                case NTSYNC_TYPE_MUTEX:
                    if (obj->u.mutex.owner == 0 || obj->u.mutex.owner == args->owner) {
                        obj->u.mutex.owner = args->owner;
                        obj->u.mutex.count++;
                        if (obj->u.mutex.abandoned) {
                            obj->u.mutex.abandoned = 0;
                            ownerdead = 1;
                        }
                    }
                    break;
                case NTSYNC_TYPE_EVENT:
                    if (!obj->u.event.manual)
                        obj->u.event.signaled = 0;
                    break;
                }
                mtx_unlock(&obj->lock);
            }
            error = ownerdead ? EOWNERDEAD : 0;
            args->index = 0;
            break;
        }

        /* If non-blocking wait */
        if (timeout_ns == 0) {
            error = ETIMEDOUT;
            break;
        }

        error = ntsync_wait_timeout(&dev->cv, &dev->wait_all_lock, timeout_ns, args->flags);

        if (error == EINTR || error == ERESTART) {
            if (error == ERESTART) error = EINTR;
            break;
        }
        if (error == ETIMEDOUT) {
            break;
        }
    }

    mtx_unlock(&dev->wait_all_lock);

out:
    /* Drop file references */
    for (i = 0; i < args->count; i++) {
        if (fps[i] != NULL)
            fdrop(fps[i], td);
    }
    free(objs, M_NTSYNC);
    free(fps, M_NTSYNC);
    free(fds, M_NTSYNC);
    return (error);
}

/* Device operations */

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
    mtx_init(&dev->wait_all_lock, "ntsync_wait_all", NULL, MTX_DEF);
    cv_init(&dev->cv, "ntsync_dev_cv");

    error = devfs_set_cdevpriv(dev, ntsync_dev_dtor);
    if (error) {
        ntsync_dev_dtor(dev);
        return (error);
    }

    printf("ntsync: open(/dev/ntsync) by pid %d\n", td->td_proc->p_pid);
    return (0);
}

static int
ntsync_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
    struct ntsync_device *dev;
    struct ntsync_obj *obj;
    int error, fd;

    error = devfs_get_cdevpriv((void **)&dev);
    if (error)
        return (error);

    switch (cmd) {
    case NTSYNC_IOC_CREATE_SEM:
    case NTSYNC_IOC_CREATE_MUTEX:
    case NTSYNC_IOC_CREATE_EVENT:
        obj = malloc(sizeof(*obj), M_NTSYNC, M_WAITOK | M_ZERO);
        mtx_init(&obj->lock, "ntsync_lock", NULL, MTX_DEF);
        obj->dev = dev;
        refcount_acquire(&dev->refcount);

        if (cmd == NTSYNC_IOC_CREATE_SEM) {
            struct ntsync_sem_args *args = (struct ntsync_sem_args *)data;
            if (args->count > args->max) {
                error = EINVAL;
                goto alloc_fail;
            }
            obj->type = NTSYNC_TYPE_SEM;
            obj->u.sem.count = args->count;
            obj->u.sem.max = args->max;
            printf("ntsync: Created SEM count=%u max=%u\n", args->count, args->max);
        } else if (cmd == NTSYNC_IOC_CREATE_MUTEX) {
            struct ntsync_mutex_args *args = (struct ntsync_mutex_args *)data;
            obj->type = NTSYNC_TYPE_MUTEX;
            obj->u.mutex.owner = args->owner;
            obj->u.mutex.count = args->count;
            obj->u.mutex.abandoned = 0;
            printf("ntsync: Created MUTEX owner=%u count=%u\n", args->owner, args->count);
        } else if (cmd == NTSYNC_IOC_CREATE_EVENT) {
            struct ntsync_event_args *args = (struct ntsync_event_args *)data;
            obj->type = NTSYNC_TYPE_EVENT;
            obj->u.event.manual = args->manual;
            obj->u.event.signaled = args->signaled;
            printf("ntsync: Created EVENT manual=%u signaled=%u\n", args->manual, args->signaled);
        }

        error = ntsync_obj_get_fd(td, obj, &fd);
        if (error) {
            goto alloc_fail;
        }

        td->td_retval[0] = fd;
        return (0);

    alloc_fail:
        mtx_destroy(&obj->lock);
        ntsync_dev_release(dev);
        free(obj, M_NTSYNC);
        return (error);

    case NTSYNC_IOC_WAIT_ANY:
        printf("ntsync: NTSYNC_IOC_WAIT_ANY called\n");
        return ntsync_wait_any(dev, td, (struct ntsync_wait_args *)data);

    case NTSYNC_IOC_WAIT_ALL:
        printf("ntsync: NTSYNC_IOC_WAIT_ALL called\n");
        return ntsync_wait_all(dev, td, (struct ntsync_wait_args *)data);

    default:
        printf("ntsync: Unknown IOCTL 0x%lx\n", cmd);
        return (ENOTTY);
    }
}

static int
ntsync_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
    printf("ntsync: close(/dev/ntsync) by pid %d\n", td->td_proc->p_pid);
    return (0);
}

/* Module event handler */
static int
ntsync_modevent(module_t mod, int type, void *arg)
{
    int error = 0;

    switch (type) {
    case MOD_LOAD:
        ntsync_dev = make_dev(&ntsync_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "ntsync");
        printf("ntsync: module loaded, /dev/ntsync created.\n");
        break;

    case MOD_UNLOAD:
        destroy_dev(ntsync_dev);
        printf("ntsync: module unloaded.\n");
        break;

    default:
        error = EOPNOTSUPP;
        break;
    }

    return (error);
}

DEV_MODULE(ntsync, ntsync_modevent, NULL);
MODULE_VERSION(ntsync, 1);
