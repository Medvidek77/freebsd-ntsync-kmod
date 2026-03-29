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
    uint32_t    flags;
    uint32_t    owner;
    uint32_t    alert;
    uint32_t    pad;
};

#define NTSYNC_MAX_WAIT_COUNT 64

#define NTSYNC_IOC_CREATE_SEM    _IOW ('N', 0x80, struct ntsync_sem_args)
#define NTSYNC_IOC_WAIT_ANY      _IOWR('N', 0x82, struct ntsync_wait_args)
#define NTSYNC_IOC_WAIT_ALL      _IOWR('N', 0x83, struct ntsync_wait_args)
#define NTSYNC_IOC_CREATE_MUTEX  _IOW ('N', 0x84, struct ntsync_mutex_args)
#define NTSYNC_IOC_CREATE_EVENT  _IOW ('N', 0x87, struct ntsync_event_args)

#define NTSYNC_IOC_SEM_RELEASE   _IOWR('N', 0x81, uint32_t)
#define NTSYNC_IOC_SEM_READ      _IOR ('N', 0x8b, struct ntsync_sem_args)
#define NTSYNC_IOC_MUTEX_UNLOCK  _IOWR('N', 0x85, struct ntsync_mutex_args)
#define NTSYNC_IOC_MUTEX_KILL    _IOW ('N', 0x86, uint32_t)
#define NTSYNC_IOC_MUTEX_READ    _IOR ('N', 0x8c, struct ntsync_mutex_args)
#define NTSYNC_IOC_EVENT_SET     _IOR ('N', 0x88, uint32_t)
#define NTSYNC_IOC_EVENT_RESET   _IOR ('N', 0x89, uint32_t)
#define NTSYNC_IOC_EVENT_PULSE   _IOR ('N', 0x8a, uint32_t)
#define NTSYNC_IOC_EVENT_READ    _IOR ('N', 0x8d, struct ntsync_event_args)

#endif /* _NTSYNC_UAPI_H */
