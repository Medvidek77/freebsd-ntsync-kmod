/* ntsync_uapi.h - FreeBSD port */
#ifndef _NTSYNC_UAPI_H
#define _NTSYNC_UAPI_H

#include <sys/types.h>

#ifdef __FreeBSD__
#include <sys/ioccom.h>
#endif

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

#define NTSYNC_IOC_CREATE_SEM    0x40084e80U
#define NTSYNC_IOC_WAIT_ANY      0xc0204e82U
#define NTSYNC_IOC_WAIT_ALL      0xc0204e83U
#define NTSYNC_IOC_CREATE_MUTEX  0x40084e84U
#define NTSYNC_IOC_CREATE_EVENT  0x40084e87U

#define NTSYNC_IOC_SEM_RELEASE   0xc0044e81U
#define NTSYNC_IOC_MUTEX_UNLOCK  0xc0084e85U
#define NTSYNC_IOC_MUTEX_KILL    0x40044e86U
#define NTSYNC_IOC_EVENT_SET     0x80044e88U
#define NTSYNC_IOC_EVENT_RESET   0x80044e89U
#define NTSYNC_IOC_EVENT_PULSE   0x80044e8aU
#define NTSYNC_IOC_SEM_READ      0x80084e8bU
#define NTSYNC_IOC_MUTEX_READ    0x80084e8cU
#define NTSYNC_IOC_EVENT_READ    0x80084e8dU

#endif /* _NTSYNC_UAPI_H */
