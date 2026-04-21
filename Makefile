KMOD=   ntsync
SRCS=   ntsync.c
#CFLAGS += -DNTSYNC_NO_STATS

# FreeBSD make relies on bsd.kmod.mk
.include <bsd.kmod.mk>
