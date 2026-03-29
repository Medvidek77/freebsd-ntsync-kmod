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
                    obj->u.mutex.owner = args->owner;
                    obj->u.mutex.count++;
                    if (obj->u.mutex.abandoned) {
                        obj->u.mutex.abandoned = 0;
                        ownerdead = 1;
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

        error = ntsync_wait_timeout(&dev->cv, &dev->wait_all_lock, timeout_ns);

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
