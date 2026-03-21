/*
 * qws_lock.h - QWS locking primitives (clean-room)
 *
 * Implements two distinct lock types used by Qt 4.8 QWS:
 *
 * 1. QWSLock: Per-client lock with 3 semaphores used for synchronizing
 *    client↔server communication. Created by the server; its ID is sent
 *    to the client which attaches to the same semaphore set.
 *
 *    Semaphore indices (initial values):
 *      [0] BackingStore   (1) - guards shared pixel buffer access
 *      [1] Communication  (1) - guards command/event socket exchanges
 *      [2] RegionEvent    (0) - signals pending region events to client
 *
 * 2. QLock: Display-level read/write lock protecting the framebuffer
 *    and shared display memory. Uses a counting semaphore: multiple
 *    readers allowed, writers get exclusive access.
 *
 * Both have SysV IPC and POSIX IPC backends, selectable at creation time,
 * since we (the server) don't know which the client was compiled to use.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_LOCK_H
#define QWS_LOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>

#include "qws_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * QWSLock: per-client 3-semaphore lock
 * ================================================================ */

/* Semaphore indices — must match Qt 4.8 QWSLock::LockType */
enum qws_lock_type {
    QWS_LOCK_BACKINGSTORE  = 0,
    QWS_LOCK_COMMUNICATION = 1,
    QWS_LOCK_REGIONEVENT   = 2,
};

#define QWS_LOCK_NUM_SEMS 3

typedef struct {
    qws_ipc_type_t  ipc_type;
    bool             owned;       /* true = we created it (server) */

    /* SysV backend */
    int              sysv_semid;  /* semaphore set id, or -1 */

    /* POSIX backend */
    sem_t           *posix_sems[QWS_LOCK_NUM_SEMS];  /* SEM_FAILED if unused */
    int              posix_id;    /* numeric id used in sem names */

    /* Lock counts (for nestable locking by same process) */
    int              lock_count[2];  /* only BackingStore and Communication are counted */
} qws_lock_t;

/* Create a new lock (server-side). Allocates the semaphores.
 * Returns 0 on success. The id() can then be sent to the client. */
int  qws_lock_create(qws_lock_t *lock, qws_ipc_type_t type);

/* Attach to an existing lock by ID (client-side).
 * For SysV, id is the semaphore set id.
 * For POSIX, id is the numeric identifier used in the sem names. */
int  qws_lock_open(qws_lock_t *lock, qws_ipc_type_t type, int id);

/* Destroy / detach the lock. If owned, removes the semaphores. */
void qws_lock_destroy(qws_lock_t *lock);

/* Get the ID to send to the client (for the Connected event or
 * IdentifyCommand). */
int  qws_lock_id(const qws_lock_t *lock);

/* Lock operations (sem_down). Blocks if already locked.
 * `which`: QWS_LOCK_BACKINGSTORE, QWS_LOCK_COMMUNICATION, or QWS_LOCK_REGIONEVENT */
int  qws_lock_lock(qws_lock_t *lock, int which);

/* Unlock (sem_up). */
int  qws_lock_unlock(qws_lock_t *lock, int which);

/* Non-blocking try-lock. Returns 0 if acquired, -1 if would block. */
int  qws_lock_trylock(qws_lock_t *lock, int which);

/* Wait for a semaphore to become non-zero (used for RegionEvent).
 * This is a blocking wait + immediate re-lock pattern. */
int  qws_lock_wait(qws_lock_t *lock, int which);

/* Query current value of a semaphore. Returns value or -1 on error. */
int  qws_lock_get_value(const qws_lock_t *lock, int which);

/* Check if we hold a lock (based on lock_count). Only valid for
 * BackingStore and Communication. */
bool qws_lock_has_lock(const qws_lock_t *lock, int which);

/* ================================================================
 * QLock: display read/write lock
 * ================================================================
 *
 * This protects the shared framebuffer / display memory region.
 *
 * SysV backend:
 *   Single counting semaphore initialized to MAX_LOCKS (200).
 *   Read lock:  sem_op = -1
 *   Write lock: sem_op = -MAX_LOCKS (exclusive)
 *   Keyed by ftok(socket_path, id_char).
 *
 * POSIX backend (QT_POSIX_IPC):
 *   Three named semaphores, named <socket_path><id_char>{c,r,w}:
 *     'c' = counter (init MAX_LOCKS) — read permit counter
 *     'r' = rsem    (init 1)         — read-mode lock
 *     'w' = wsem    (init 1)         — write-mode lock
 *   The lock/unlock protocol uses a combination of these three
 *   to implement reader-writer semantics.
 *
 * File-locking backend (QT_NO_SEMAPHORE, e.g. macOS):
 *   Uses flock() on a file named <socket_path><id_char>.
 *   Not implemented here since our target is embedded Linux.
 */

#define QWS_DISPLAY_LOCK_MAX 200  /* MAX_LOCKS in Qt 4.8 QLock */

typedef enum {
    QWS_DLOCK_READ,
    QWS_DLOCK_WRITE,
} qws_dlock_mode_t;

typedef struct {
    qws_ipc_type_t  ipc_type;
    bool             owned;
    qws_dlock_mode_t last_mode;  /* mode used for current lock (for unlock) */

    /* SysV backend */
    int              sysv_semid;

    /* POSIX backend — three named semaphores matching Qt 4.8 */
    sem_t           *posix_counter;    /* 'c': resource counter, init=MAX_LOCKS */
    sem_t           *posix_rsem;       /* 'r': read mode lock, init=1 */
    sem_t           *posix_wsem;       /* 'w': write mode lock, init=1 */
    char             posix_base[128];  /* base path for sem names */

    /* Nesting */
    int              count;       /* number of times locked by this process */
} qws_display_lock_t;

/* Create or open the display lock.
 * For SysV: socket_path + id_char are used with ftok().
 * For POSIX: id_char is used to form the named semaphore path.
 * If create=true, the semaphore is created (server side).
 * Returns 0 on success. */
int  qws_display_lock_create(qws_display_lock_t *lock, qws_ipc_type_t type,
                               const char *socket_path, char id_char,
                               bool create);

/* Destroy / detach. */
void qws_display_lock_destroy(qws_display_lock_t *lock);

/* Acquire the lock in the given mode. Blocks if necessary. */
int  qws_display_lock_lock(qws_display_lock_t *lock, qws_dlock_mode_t mode);

/* Release the lock. Must match the mode used in lock(). */
int  qws_display_lock_unlock(qws_display_lock_t *lock, qws_dlock_mode_t mode);

/* Check if currently locked by this process. */
bool qws_display_lock_is_locked(const qws_display_lock_t *lock);

/* Check if the lock is valid (successfully created/opened). */
bool qws_display_lock_is_valid(const qws_display_lock_t *lock);

#ifdef __cplusplus
}
#endif

#endif /* QWS_LOCK_H */