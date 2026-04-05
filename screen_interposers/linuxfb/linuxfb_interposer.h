/*
 * linuxfb_interposer.h - shared types and helpers for the linuxfb interposer
 * SPDX-License-Identifier: MIT
 */

#ifndef LINUXFB_INTERPOSER_H
#define LINUXFB_INTERPOSER_H

#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* Binary contents of POSIX shm region /linuxfb_{DEVNAME}.
 * Created and written by the QWSWayland server; read-only for the interposer.
 * E.g. devname "fb0"  ->  shm_open("/linuxfb_fb0", ...) */
typedef struct {
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
} linuxfb_screen_info_t;

/* Magic device path prefix used by QWSWayland.
 * QWS clients are told to use /dev/qwswl<devname> (e.g. /dev/qwswlfb0)
 * instead of the real /dev/fb<N>, so the interposer can intercept
 * without ambiguity regardless of what the user passes on the command line. */
#define LINUXFB_INTERPOSER_PREFIX "/dev/qwswl"

/* Returns true if path is a QWSWayland-managed framebuffer path. */
static inline int linuxfb_is_interposer_path(const char *path) {
    return path != NULL && strncmp(path, LINUXFB_INTERPOSER_PREFIX,
                                   sizeof(LINUXFB_INTERPOSER_PREFIX) - 1) == 0;
}

/* Returns a pointer to the device name portion of a managed path.
 * E.g. "/dev/qwswlfb0" -> "fb0" */
static inline const char *linuxfb_devname(const char *path) {
    return path + sizeof(LINUXFB_INTERPOSER_PREFIX) - 1;
}

/* Writes the managed path for a given device name into buf.
 * E.g. "fb0" -> "/dev/qwswlfb0"
 * Returns buf for convenience. */
static inline char *linuxfb_interposer_path(const char *devname, char *buf,
                                            size_t bufsz) {
    snprintf(buf, bufsz, "%s%s", LINUXFB_INTERPOSER_PREFIX, devname);
    return buf;
}

/* Derives the POSIX shm name for a given device name into buf.
 * E.g. "fb0" -> "/linuxfb_fb0". Returns buf. */
static inline char *linuxfb_shm_name(const char *devname, char *buf,
                                     size_t bufsz) {
    snprintf(buf, bufsz, "/linuxfb_%s", devname);
    return buf;
}

/* Opens the shm region for devname.
 * If create is true, the region is created/replaced (O_RDWR|O_CREAT|O_TRUNC);
 * otherwise it is opened read-only.
 * Returns the file descriptor on success, -1 on error. */
static inline int linuxfb_shm_open(const char *devname, bool create) {
    char name[64];
    linuxfb_shm_name(devname, name, sizeof(name));
    if (create)
        return shm_open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    return shm_open(name, O_RDONLY, 0);
}

#endif /* LINUXFB_INTERPOSER_H */
