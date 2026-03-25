/*
 * qws_lock.h - Qt and QWS locking primitives
 *
 * QWSLock: Per-client lock with 3 semaphores used for synchronizing
 *  client↔server communication. Created by the server; its ID is sent
 *  to the client which attaches to the same semaphore set.
 *  Mirrors Qt 4.8's QWSLock class.
 *
 *  Semaphore indices:
 *    [0] BackingStore    - guards shared pixel buffer access
 *    [1] Communication   - guards command/event socket exchanges
 *    [2] RegionEvent     - signals pending region events to client
 *
 * QLock: General reader-writer lock identified by (filename, char id).
 *  Multiple locks may share a filename, differentiated by the id char.
 *  Mirrors Qt 4.8's QLock class.
 *
 * The IPC backend (SysV vs POSIX) is selected at compile time via define
 * QWS_IPC_POSIX (meson: ipc_backend=posix). Default is SysV.
 * See README.md for full protocol details.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QWS_LOCK_H
#define QWS_LOCK_H

#include <stdint.h>
#include <stdbool.h>

#include "qws_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * QWSLock: per-client 3-semaphore lock
 * ================================================================ */

/* Semaphore indices — must match Qt 4.8 QWSLock::LockType */
typedef enum {
    QWS_LOCK_BACKINGSTORE  = 0,
    QWS_LOCK_COMMUNICATION = 1,
    QWS_LOCK_REGIONEVENT   = 2,
} qwslock_type_t;

/* ================================================================
 * QLock: reader-writer lock
 * ================================================================ */

/* Mirrors Qt 4.8 QLock::Type */
typedef enum {
    QWS_QLOCK_READ  = 0,   /* shared read lock  */
    QWS_QLOCK_WRITE = 1,   /* exclusive write lock */
} qlock_type_t;

/* ================================================================
 * Opaque handle types (struct definitions live in qws_lock.c)
 * ================================================================ */

typedef struct qws_lock qwslock_t;   /* QWSLock opaque handle */
typedef struct q_lock   qlock_t;     /* QLock opaque handle */

/* ================================================================
 * QWSLock API
 * ================================================================ */

/* Create a new QWSLock (server-side). Allocates the semaphores.
 * Returns pointer on success, NULL on failure.
 * Use qwslock_id() to get the id to send to the client. */
qwslock_t  *qwslock_create(void);

/* Attach to an existing QWSLock by ID (client-side).
 * For SysV, id is the semaphore set id.
 * For POSIX, id is the numeric identifier used in the sem names.
 * Returns pointer on success, NULL on failure. */
qwslock_t  *qwslock_open(int id);

/* Destroy / detach the lock. If owned, removes the semaphores.
 * Also frees the allocation. NULL-safe. */
void qwslock_destroy(qwslock_t *lock);

/* Get the ID to send to the client (for the Connected event or
 * IdentifyCommand). */
int  qwslock_id(const qwslock_t *lock);

/* Lock operations (sem_down). Blocks if already locked.
 * `which`: QWS_LOCK_BACKINGSTORE, QWS_LOCK_COMMUNICATION, or QWS_LOCK_REGIONEVENT */
int  qwslock_lock(qwslock_t *lock, qwslock_type_t which);

/* Unlock (sem_up). */
int  qwslock_unlock(qwslock_t *lock, qwslock_type_t which);

/* Wait for a semaphore to become non-zero (used for RegionEvent).
 * This is a blocking wait + immediate re-lock pattern. */
int  qwslock_wait(qwslock_t *lock, qwslock_type_t which);

/* Query current value of a semaphore. Returns value or -1 on error. */
int  qwslock_get_value(const qwslock_t *lock, qwslock_type_t which);

/* ================================================================
 * QLock API
 * ================================================================ */

/* Create a new QLock (server/creator side).
 * filename + id identifies the lock; multiple locks may share a filename.
 * Returns pointer on success, NULL on failure. */
qlock_t *qlock_create(const char *filename, char id);

/* Attach to an existing QLock (client side).
 * Returns pointer on success, NULL on failure. */
qlock_t *qlock_open(const char *filename, char id);

/* Destroy / detach the lock. If owned, removes the semaphores.
 * Also frees the allocation. NULL-safe. */
void qlock_destroy(qlock_t *lock);

/* Lock/unlock with Read or Write type (type = qlock_type_t).
 * Read = shared; Write = exclusive. */
int  qlock_lock(qlock_t *lock, qlock_type_t type);
int  qlock_unlock(qlock_t *lock, qlock_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* QWS_LOCK_H */
