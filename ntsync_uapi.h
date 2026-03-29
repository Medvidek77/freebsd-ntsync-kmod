/* ntsync_uapi.h - FreeBSD port */
#ifndef _NTSYNC_UAPI_H
#define _NTSYNC_UAPI_H

#include <sys/types.h>
#include <sys/ioccom.h>

struct ntsync_sem_args {
    uint32_t    count;
    uint32_t    max;
};

struct ntsync_mutex_args {
    uint32_t    owner;
    uint32_t    count;
};

struct ntsync_event_args {
    uint32_t    manual;
    uint32_t    signaled;
};

#define NTSYNC_WAIT_REALTIME 0x1

struct ntsync_wait_args {
    uint64_t    timeout;
    uint64_t    objs;       /* pointer to uint32_t array of fds */
    uint32_t    count;
    uint32_t    index;      /* out: which obj woke us */
    uint32_t    alert;
    uint32_t    flags;
    uint32_t    owner;
    uint32_t    pad;
};

#define NTSYNC_MAX_WAIT_COUNT 64

#define NTSYNC_IOC_CREATE_SEM    _IOWR(0x9d, 0x00, struct ntsync_sem_args)
#define NTSYNC_IOC_CREATE_MUTEX  _IOWR(0x9d, 0x01, struct ntsync_mutex_args)
#define NTSYNC_IOC_CREATE_EVENT  _IOWR(0x9d, 0x02, struct ntsync_event_args)
#define NTSYNC_IOC_WAIT_ANY      _IOWR(0x9d, 0x03, struct ntsync_wait_args)
#define NTSYNC_IOC_WAIT_ALL      _IOWR(0x9d, 0x04, struct ntsync_wait_args)
#define NTSYNC_IOC_SEM_POST      _IOWR(0x9d, 0x05, uint32_t)
#define NTSYNC_IOC_SEM_READ      _IOR( 0x9d, 0x06, struct ntsync_sem_args)
#define NTSYNC_IOC_MUTEX_UNLOCK  _IOWR(0x9d, 0x07, struct ntsync_mutex_args)
#define NTSYNC_IOC_MUTEX_KILL    _IOW( 0x9d, 0x08, uint32_t)
#define NTSYNC_IOC_MUTEX_READ    _IOR( 0x9d, 0x09, struct ntsync_mutex_args)
#define NTSYNC_IOC_EVENT_SET     _IOR( 0x9d, 0x0a, uint32_t)
#define NTSYNC_IOC_EVENT_RESET   _IOR( 0x9d, 0x0b, uint32_t)
#define NTSYNC_IOC_EVENT_PULSE   _IOR( 0x9d, 0x0c, uint32_t)
#define NTSYNC_IOC_EVENT_READ    _IOR( 0x9d, 0x0d, struct ntsync_event_args)

#endif /* _NTSYNC_UAPI_H */
