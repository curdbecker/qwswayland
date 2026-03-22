/*
 * qws_lock.h - QWS locking primitives
 *
 * QWSLock: Per-client lock with 3 semaphores used for synchronizing
 *  client↔server communication. Created by the client; its ID is sent
 *  to the client which attaches to the same semaphore set.
 *
 *  Semaphore indices:
 *    [0] BackingStore    - guards shared pixel buffer access
 *    [1] Communication   - guards command/event socket exchanges
 *    [2] RegionEvent     - signals pending region events to client
 *
 * We provide SysV IPC and POSIX IPC backends, selectable at creation time,
 * since the server doesn't know which the client was compiled to use.
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

#ifdef __cplusplus
}
#endif

#endif /* QWS_LOCK_H */