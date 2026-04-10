KMOD=   ntsync
SRCS=   ntsync.c

# FreeBSD make relies on bsd.kmod.mk
.include <bsd.kmod.mk>
