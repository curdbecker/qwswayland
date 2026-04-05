/*
 * linuxfb_interposer.c - LD_PRELOAD interposer for the Linux framebuffer
 * SPDX-License-Identifier: MIT
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "linuxfb_interposer.h"

/* ISO C forbids direct assignment of void * to a function pointer.
 * Copy through memcpy to stay pedantic-clean. */
#define DLSYM(ptr, name)                                                       \
    do {                                                                       \
        void *_s = dlsym(RTLD_NEXT, (name));                                   \
        memcpy(&(ptr), &_s, sizeof(ptr));                                      \
    } while (0)

/* =========================================================================
 * Internal state
 * ========================================================================= */

/* Single dummy memfd handed out for every open("/dev/fb*", ...) call.
 * Lives for the process lifetime; close() on it is silently ignored. */
static int fb_fd = -1;

/* Screen info read once from the POSIX shm region at first fb open. */
static linuxfb_screen_info_t fb_info;
static bool fb_info_valid = false;

/* =========================================================================
 * Real function pointers — initialised in the constructor below
 * ========================================================================= */

static int (*real_access)(const char *, int);
static int (*real_faccessat)(int, const char *, int, int);
static int (*real_open)(const char *, int, ...);
#ifdef HAVE_DISTINCT_OPEN64
static int (*real_open64)(const char *, int, ...);
#endif
static int (*real_openat)(int, const char *, int, ...);
#ifdef HAVE_DISTINCT_OPENAT64
static int (*real_openat64)(int, const char *, int, ...);
#endif
static int (*real_stat)(const char *, struct stat *);
static int (*real_lstat)(const char *, struct stat *);
static int (*real_fstat)(int, struct stat *);
#ifdef HAVE_DISTINCT_STAT64
static int (*real_stat64)(const char *, struct stat64 *);
#endif
#ifdef HAVE_DISTINCT_LSTAT64
static int (*real_lstat64)(const char *, struct stat64 *);
#endif
#ifdef HAVE_DISTINCT_FSTAT64
static int (*real_fstat64)(int, struct stat64 *);
#endif
static int (*real_close)(int);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
#ifdef HAVE_DISTINCT_MMAP64
static void *(*real_mmap64)(void *, size_t, int, int, int, off64_t);
#endif
static int (*real_ioctl)(int, unsigned long, ...);

__attribute__((constructor)) static void linuxfb_interposer_init(void) {
    DLSYM(real_access, "access");
    DLSYM(real_faccessat, "faccessat");
    DLSYM(real_stat, "stat");
    DLSYM(real_lstat, "lstat");
    DLSYM(real_fstat, "fstat");
    DLSYM(real_open, "open");
#ifdef HAVE_DISTINCT_OPEN64
    DLSYM(real_open64, "open64");
#endif
    DLSYM(real_openat, "openat");
#ifdef HAVE_DISTINCT_OPENAT64
    DLSYM(real_openat64, "openat64");
#endif
#ifdef HAVE_DISTINCT_STAT64
    DLSYM(real_stat64, "stat64");
#endif
#ifdef HAVE_DISTINCT_LSTAT64
    DLSYM(real_lstat64, "lstat64");
#endif
#ifdef HAVE_DISTINCT_FSTAT64
    DLSYM(real_fstat64, "fstat64");
#endif
    DLSYM(real_close, "close");
    DLSYM(real_read, "read");
    DLSYM(real_write, "write");
    DLSYM(real_mmap, "mmap");
#ifdef HAVE_DISTINCT_MMAP64
    DLSYM(real_mmap64, "mmap64");
#endif
    DLSYM(real_ioctl, "ioctl");
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Read screen info from the server-created shm region into the cache. */
static bool load_fb_info(const char *dev_path) {
    int sfd = linuxfb_shm_open(linuxfb_devname(dev_path), false);
    if (sfd < 0) {
        fprintf(stderr, "linuxfb_interposer: failed to open shm for %s: %s\n",
                dev_path, strerror(errno));
        return false;
    }
    ssize_t n = read(sfd, &fb_info, sizeof(fb_info));
    close(sfd);
    if (n != (ssize_t)sizeof(fb_info)) {
        fprintf(stderr, "linuxfb_interposer: short read from shm for %s\n",
                dev_path);
        return false;
    }
    fb_info_valid = true;
    return true;
}

static void fill_fake_stat(struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0444;
    st->st_rdev = makedev(29, 0); /* Linux framebuffer: major 29, minor 0 */
    st->st_nlink = 1;
}

#if defined(HAVE_DISTINCT_STAT64) || defined(HAVE_DISTINCT_LSTAT64) ||         \
    defined(HAVE_DISTINCT_FSTAT64)
static void fill_fake_stat64(struct stat64 *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0444;
    st->st_rdev = makedev(29, 0);
    st->st_nlink = 1;
}
#endif

/* Core open logic shared by open / open64 / openat.
 * real_fn is called for non-fb paths so each variant uses its own real pointer.
 */
static int open_fb_or_passthrough(const char *path, int flags, mode_t mode,
                                  int (*real_fn)(const char *, int, ...)) {
    if (!linuxfb_is_interposer_path(path))
        return real_fn(path, flags, mode);

    /* Appear as a read-only device: reject any write access attempt. */
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EACCES;
        return -1;
    }

    /* Load screen info from the server-created shm region on first open. */
    if (!fb_info_valid && !load_fb_info(path)) {
        errno = ENOENT;
        return -1;
    }

    /* Open /dev/null once and reuse the fd for the process lifetime. */
    if (fb_fd == -1) {
        fb_fd = real_open("/dev/null", O_RDONLY);
        if (fb_fd == -1)
            return -1;
    }

    return fb_fd;
}

/* =========================================================================
 * Interposed functions
 * ========================================================================= */

int access(const char *path, int mode) {
    if (linuxfb_is_interposer_path(path)) {
        /* Present as read-only: R_OK succeeds, W_OK fails. */
        if (mode & W_OK) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    return real_access(path, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    if (linuxfb_is_interposer_path(path)) {
        if (mode & W_OK) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    return real_faccessat(dirfd, path, mode, flags);
}

int stat(const char *path, struct stat *st) {
    if (linuxfb_is_interposer_path(path)) {
        fill_fake_stat(st);
        return 0;
    }
    return real_stat(path, st);
}

int lstat(const char *path, struct stat *st) {
    if (linuxfb_is_interposer_path(path)) {
        fill_fake_stat(st);
        return 0;
    }
    return real_lstat(path, st);
}

int fstat(int fd, struct stat *st) {
    if (fd == fb_fd && fb_fd != -1) {
        fill_fake_stat(st);
        return 0;
    }
    return real_fstat(fd, st);
}

int open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    return open_fb_or_passthrough(path, flags, mode, real_open);
}

#ifdef HAVE_DISTINCT_OPEN64
int open64(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);
    return open_fb_or_passthrough(path, flags, mode, real_open64);
}
#endif

int openat(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);

    if (linuxfb_is_interposer_path(path))
        return open_fb_or_passthrough(path, flags, mode, real_open);
    return real_openat(dirfd, path, flags, mode);
}

#ifdef HAVE_DISTINCT_OPENAT64
int openat64(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
    va_end(ap);

    if (linuxfb_is_interposer_path(path))
        return open_fb_or_passthrough(path, flags, mode, real_open);
    return real_openat64(dirfd, path, flags, mode);
}
#endif

#ifdef HAVE_DISTINCT_STAT64
int stat64(const char *path, struct stat64 *st) {
    if (linuxfb_is_interposer_path(path)) {
        fill_fake_stat64(st);
        return 0;
    }
    return real_stat64(path, st);
}
#endif

#ifdef HAVE_DISTINCT_LSTAT64
int lstat64(const char *path, struct stat64 *st) {
    if (linuxfb_is_interposer_path(path)) {
        fill_fake_stat64(st);
        return 0;
    }
    return real_lstat64(path, st);
}
#endif

#ifdef HAVE_DISTINCT_FSTAT64
int fstat64(int fd, struct stat64 *st) {
    if (fd == fb_fd && fb_fd != -1) {
        fill_fake_stat64(st);
        return 0;
    }
    return real_fstat64(fd, st);
}
#endif

#ifdef HAVE_DISTINCT_MMAP64
void *mmap64(void *addr, size_t len, int prot, int mflags, int fd,
             off64_t off) {
    if (fd == fb_fd && fb_fd != -1) {
        errno = ENODEV;
        return MAP_FAILED;
    }
    return real_mmap64(addr, len, prot, mflags, fd, off);
}
#endif

/* Silently ignore: the dummy fd lives for the process lifetime. */
int close(int fd) {
    if (fd == fb_fd && fb_fd != -1)
        return 0;
    return real_close(fd);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (fd == fb_fd && fb_fd != -1) {
        errno = EBADF;
        return -1;
    }
    return real_read(fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd == fb_fd && fb_fd != -1) {
        errno = EBADF;
        return -1;
    }
    return real_write(fd, buf, count);
}

void *mmap(void *addr, size_t len, int prot, int mflags, int fd, off_t off) {
    if (fd == fb_fd && fb_fd != -1) {
        errno = ENODEV;
        return MAP_FAILED;
    }
    return real_mmap(addr, len, prot, mflags, fd, off);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (fd != fb_fd || fb_fd == -1)
        return real_ioctl(fd, req, arg);

    switch (req) {
    case FBIOGET_FSCREENINFO:
        if (!fb_info_valid) {
            errno = ENODEV;
            return -1;
        }
        memcpy(arg, &fb_info.finfo, sizeof(fb_info.finfo));
        return 0;
    case FBIOGET_VSCREENINFO:
        if (!fb_info_valid) {
            errno = ENODEV;
            return -1;
        }
        memcpy(arg, &fb_info.vinfo, sizeof(fb_info.vinfo));
        return 0;
    case FBIOPUT_VSCREENINFO:
        errno = EROFS;
        return -1;
    case FBIOGETCMAP:
    case FBIOPUTCMAP:
        errno = EINVAL;
        return -1;
    case FBIOBLANK:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}
