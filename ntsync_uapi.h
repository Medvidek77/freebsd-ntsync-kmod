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

/*
 * FreeBSD uses different direction bits for _IOW (0x80) and _IOR (0x40)
 * than Linux (_IOW=0x40, _IOR=0x80). Wine expects the exact 32-bit Linux
 * numbers. So we hardcode the ioctls to perfectly match Linux's generated
 * values, ignoring FreeBSD's built-in _IOW/_IOR macros to prevent ENOTTY.
 */
#define NTSYNC_IOC_CREATE_SEM    0x40084e80
#define NTSYNC_IOC_WAIT_ANY      0xc0204e82
#define NTSYNC_IOC_WAIT_ALL      0xc0204e83
#define NTSYNC_IOC_CREATE_MUTEX  0x40084e84
#define NTSYNC_IOC_CREATE_EVENT  0x40084e87

#define NTSYNC_IOC_SEM_RELEASE   0xc0044e81
#define NTSYNC_IOC_SEM_READ      0x80084e8b
#define NTSYNC_IOC_MUTEX_UNLOCK  0xc0084e85
#define NTSYNC_IOC_MUTEX_KILL    0x40044e86
#define NTSYNC_IOC_MUTEX_READ    0x80084e8c
#define NTSYNC_IOC_EVENT_SET     0x80044e88
#define NTSYNC_IOC_EVENT_RESET   0x80044e89
#define NTSYNC_IOC_EVENT_PULSE   0x80044e8a
#define NTSYNC_IOC_EVENT_READ    0x80084e8d

#endif /* _NTSYNC_UAPI_H */
