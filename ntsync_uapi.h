/*
 * ntsync_uapi.h - FreeBSD port of Linux NT sync uAPI
 * Layout matches upstream linux/ntsync.h exactly for ABI compat.
 */

#ifndef __NTSYNC_UAPI_H__
#define __NTSYNC_UAPI_H__

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

#define NTSYNC_WAIT_REALTIME    0x1

struct ntsync_wait_args {
    uint64_t    timeout;
    uint64_t    objs;
    uint32_t    count;
    uint32_t    index;
    uint32_t    flags;
    uint32_t    owner;
    uint32_t    alert;
    uint32_t    pad;
};

#define NTSYNC_MAX_WAIT_COUNT   64

/*
 * FreeBSD native IOCTLs — computed with FreeBSD ioccom.h macros.
 * FreeBSD _IOW = IOC_IN (0x80000000), _IOR = IOC_OUT (0x40000000).
 * These match what wine-proton FreeBSD native binary sends.
 */
#define NTSYNC_IOC_CREATE_SEM   _IOW ('N', 0x80, struct ntsync_sem_args)
#define NTSYNC_IOC_SEM_RELEASE  _IOWR('N', 0x81, uint32_t)
#define NTSYNC_IOC_WAIT_ANY     _IOWR('N', 0x82, struct ntsync_wait_args)
#define NTSYNC_IOC_WAIT_ALL     _IOWR('N', 0x83, struct ntsync_wait_args)
#define NTSYNC_IOC_CREATE_MUTEX _IOW ('N', 0x84, struct ntsync_mutex_args)
#define NTSYNC_IOC_MUTEX_UNLOCK _IOWR('N', 0x85, struct ntsync_mutex_args)
#define NTSYNC_IOC_MUTEX_KILL   _IOW ('N', 0x86, uint32_t)
#define NTSYNC_IOC_CREATE_EVENT _IOW ('N', 0x87, struct ntsync_event_args)
#define NTSYNC_IOC_EVENT_SET    _IOR ('N', 0x88, uint32_t)
#define NTSYNC_IOC_EVENT_RESET  _IOR ('N', 0x89, uint32_t)
#define NTSYNC_IOC_EVENT_PULSE  _IOR ('N', 0x8a, uint32_t)
#define NTSYNC_IOC_SEM_READ     _IOR ('N', 0x8b, struct ntsync_sem_args)
#define NTSYNC_IOC_MUTEX_READ   _IOR ('N', 0x8c, struct ntsync_mutex_args)
#define NTSYNC_IOC_EVENT_READ   _IOR ('N', 0x8d, struct ntsync_event_args)

/*
 * Linux-encoded variants for linuxulator path (Linux _IOW = 0x40000000).
 * Only needed if running unpatched Linux Proton binaries via linuxulator.
 */
#define NTSYNC_IOC_CREATE_SEM_LINUX     0x40084e80
#define NTSYNC_IOC_SEM_RELEASE_LINUX    0xc0044e81
#define NTSYNC_IOC_CREATE_MUTEX_LINUX   0x40084e84
#define NTSYNC_IOC_MUTEX_KILL_LINUX     0x40044e86
#define NTSYNC_IOC_CREATE_EVENT_LINUX   0x40084e87
#define NTSYNC_IOC_EVENT_SET_LINUX      0x80044e88
#define NTSYNC_IOC_EVENT_RESET_LINUX    0x80044e89
#define NTSYNC_IOC_EVENT_PULSE_LINUX    0x80044e8a
#define NTSYNC_IOC_SEM_READ_LINUX       0x80084e8b
#define NTSYNC_IOC_MUTEX_READ_LINUX     0x80084e8c
#define NTSYNC_IOC_EVENT_READ_LINUX     0x80084e8d

#endif /* __NTSYNC_UAPI_H__ */
