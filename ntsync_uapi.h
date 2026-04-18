/*
 * ntsync_uapi.h - FreeBSD port
 * User-space API for NT synchronization primitive emulation.
 */
#ifndef _NTSYNC_UAPI_H
#define _NTSYNC_UAPI_H

#include <sys/types.h>

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

/* 
 * Memory layout strictly matches the upstream Linux/Proton uAPI
 * to ensure ABI compatibility.
 */
struct ntsync_wait_args {
    uint64_t    timeout;
    uint64_t    objs;       /* pointer to uint32_t array of fds */
    uint32_t    count;
    uint32_t    owner;
    uint32_t    index;      /* out: which obj woke us */
    uint32_t    alert;
    uint32_t    flags;
    uint32_t    pad;
};

#define NTSYNC_MAX_WAIT_COUNT 64

/*
 * Native FreeBSD IOCTL definitions.
 * Used when Wine/Proton is compiled natively for FreeBSD.
 */
#define NTSYNC_IOC_CREATE_SEM    0x80084e80
#define NTSYNC_IOC_WAIT_ANY      0xc0284e82
#define NTSYNC_IOC_WAIT_ALL      0xc0284e83
#define NTSYNC_IOC_CREATE_MUTEX  0x80084e84
#define NTSYNC_IOC_CREATE_EVENT  0x80084e87

#define NTSYNC_IOC_SEM_RELEASE   0xc0044e81
#define NTSYNC_IOC_SEM_READ      0x40084e8b
#define NTSYNC_IOC_MUTEX_UNLOCK  0xc0084e85
#define NTSYNC_IOC_MUTEX_KILL    0x80044e86
#define NTSYNC_IOC_MUTEX_READ    0x40084e8c
#define NTSYNC_IOC_EVENT_SET     0x40044e88
#define NTSYNC_IOC_EVENT_RESET   0x40044e89
#define NTSYNC_IOC_EVENT_PULSE   0x40044e8a
#define NTSYNC_IOC_EVENT_READ    0x40084e8d

/*
 * Linux compatible IOCTL definitions.
 * Used for running unmodified Linux Proton binaries via Linuxator.
 * Note: _IOWR macros (0xc0...) are binary compatible between Linux and FreeBSD.
 */
#define NTSYNC_IOC_CREATE_SEM_LINUX    0x40084e80
#define NTSYNC_IOC_CREATE_MUTEX_LINUX  0x40084e84
#define NTSYNC_IOC_CREATE_EVENT_LINUX  0x40084e87
#define NTSYNC_IOC_MUTEX_KILL_LINUX    0x40044e86
#define NTSYNC_IOC_SEM_READ_LINUX      0x80084e8b
#define NTSYNC_IOC_MUTEX_READ_LINUX    0x80084e8c
#define NTSYNC_IOC_EVENT_SET_LINUX     0x80044e88
#define NTSYNC_IOC_EVENT_RESET_LINUX   0x80044e89
#define NTSYNC_IOC_EVENT_PULSE_LINUX   0x80044e8a
#define NTSYNC_IOC_EVENT_READ_LINUX    0x80084e8d

#endif /* _NTSYNC_UAPI_H */
