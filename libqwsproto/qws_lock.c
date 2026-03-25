/*
 * qws_lock.c - Qt and QWS locking primitives
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "qws_lock.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#ifdef QWS_IPC_POSIX
#include <semaphore.h>
#endif

#include <assert.h>

#ifdef QWS_IPC_POSIX
#warning UNTESTED AND AI GENERATED - might do everything or nothing: or even everything in between.
#endif

#define LOCK_NUM_SEMS 3
#define QLOCK_MAX_LOCKS 200   /* initial value of QLock counter semaphore */

/* ================================================================
 * Private struct definitions
 * ================================================================ */

/* Shared base — embedded as the first member of both lock structs. */
struct lock_base {
    bool  owned;
    int   posix_id;                    /* QWSLock numeric wire id */
#ifdef QWS_IPC_POSIX
    sem_t *posix_sems[LOCK_NUM_SEMS];
    char   posix_prefix[PATH_MAX];     /* "/qwslock_<hex>_" or "<filename><id>" */
#else
    int    sysv_semid;
#endif
};

/* QWSLock: per-client 3-semaphore lock with nestable acquire. */
struct qws_lock {
    struct lock_base base;
    int   lock_count[LOCK_NUM_SEMS];
};

/* QLock: reader-writer lock identified by (filename, id). */
struct q_lock {
    struct lock_base base;
    char  id;    /* differentiator character (also useful for debug) */
};

/* ================================================================
 * SysV semun union (not always defined in headers)
 * ================================================================ */

#if defined(__GNU_LIBRARY__) && !defined(_SEM_SEMUN_UNDEFINED)
/* union semun is defined by including <sys/sem.h> */
#else
union semun {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};
#endif

/* ================================================================
 * Internal: EINTR-safe wrappers
 * ================================================================ */

#ifdef QWS_IPC_POSIX

static int posix_sem_wait_eintr(sem_t *sem)
{
    int ret;
    do {
        ret = sem_wait(sem);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

#else

static int sysv_semop_eintr(int semid, struct sembuf *sops, size_t nsops)
{
    int ret;
    do {
        ret = semop(semid, sops, nsops);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

#endif /* QWS_IPC_POSIX */

/* ================================================================
 * Initial semaphore values
 * ================================================================ */

/* BackingStore=1, Communication=1, RegionEvent=0 */
static const unsigned short qwslock_init_vals[LOCK_NUM_SEMS] = { 1, 1, 0 };

/* QLock: c=MAX_LOCKS, r=1, w=1 */
static const unsigned short qlock_init_vals[3] = { QLOCK_MAX_LOCKS, 1, 1 };

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Initialise base fields after malloc+memset. */
static void _lock_base_init(struct lock_base *base, bool owned)
{
    base->owned    = owned;
    base->posix_id = 0;
#ifdef QWS_IPC_POSIX
    for (int i = 0; i < LOCK_NUM_SEMS; i++)
        base->posix_sems[i] = SEM_FAILED;
#else
    base->sysv_semid = -1;
#endif
}

#ifndef QWS_IPC_POSIX

static int _sysv_create(struct lock_base *base, key_t key, int n_sems,
                         const unsigned short *init_vals)
{
    int semid = semget(key, n_sems, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("qws_lock: semget");
        return -1;
    }

    union semun arg;
    arg.array = (unsigned short *)init_vals;
    if (semctl(semid, 0, SETALL, arg) == -1) {
        perror("qws_lock: semctl SETALL");
        semctl(semid, 0, IPC_RMID);
        return -1;
    }

    base->sysv_semid = semid;
    return 0;
}

static int sysv_sem_up(int semid, int idx)
{
    struct sembuf op = { .sem_num = (unsigned short)idx, .sem_op = 1, .sem_flg = 0 };
    return sysv_semop_eintr(semid, &op, 1);
}

static int sysv_sem_down(int semid, int idx)
{
    struct sembuf op = { .sem_num = (unsigned short)idx, .sem_op = -1, .sem_flg = 0 };
    return sysv_semop_eintr(semid, &op, 1);
}

static int sysv_sem_getval(int semid, int idx)
{
    return semctl(semid, idx, GETVAL);
}

#else /* QWS_IPC_POSIX */

static const char *qwslock_posix_suffixes[LOCK_NUM_SEMS] = {
    "BackingStore",
    "Communication",
    "RegionEvent",
};

/* Generate the full per-sem POSIX name from the stored prefix.
 * QWSLock (qlock=false): "/qwslock_<hex>_BackingStore" etc.
 * QLock   (qlock=true):  "<filename><id>c" / "...r" / "...w" */
static void _posix_name(const struct lock_base *base, int idx,
                         char *buf, size_t buflen, bool qlock)
{
    if (qlock)
        snprintf(buf, buflen, "%s%c", base->posix_prefix, "crw"[idx]);
    else
        snprintf(buf, buflen, "%s%s", base->posix_prefix, qwslock_posix_suffixes[idx]);
}

/* Open or create n POSIX semaphores for base.
 * On failure: closes/unlinks already-opened sems (0..i-1), returns -1.
 * Caller is responsible for freeing the containing struct. */
static int _posix_sems(struct lock_base *base, int n,
                        const unsigned short *init_vals, bool create, bool qlock)
{
    char name[PATH_MAX];
    for (int i = 0; i < n; i++) {
        _posix_name(base, i, name, sizeof(name), qlock);
        sem_t *sem;
        if (create) {
            sem_unlink(name);
            do {
                sem = sem_open(name, O_CREAT, 0666, init_vals[i]);
            } while (sem == SEM_FAILED && errno == EINTR);
        } else {
            do {
                sem = sem_open(name, 0);
            } while (sem == SEM_FAILED && errno == EINTR);
        }
        if (sem == SEM_FAILED) {
            perror("qws_lock: sem_open");
            for (int j = 0; j < i; j++) {
                sem_close(base->posix_sems[j]);
                if (create) {
                    _posix_name(base, j, name, sizeof(name), qlock);
                    sem_unlink(name);
                }
                base->posix_sems[j] = SEM_FAILED;
            }
            return -1;
        }
        base->posix_sems[i] = sem;
    }
    return 0;
}

#endif /* QWS_IPC_POSIX */

/* Shared IPC cleanup for both destroy functions. */
static void _lock_base_cleanup(struct lock_base *base, bool qlock)
{
#ifndef QWS_IPC_POSIX
    if (base->owned && base->sysv_semid >= 0)
        semctl(base->sysv_semid, 0, IPC_RMID);
#else
    char name[PATH_MAX];
    for (int i = 0; i < LOCK_NUM_SEMS; i++) {
        if (base->posix_sems[i] != SEM_FAILED) {
            sem_close(base->posix_sems[i]);
            if (base->owned) {
                _posix_name(base, i, name, sizeof(name), qlock);
                sem_unlink(name);
            }
        }
    }
#endif
}

/* ################################################################
 * QWSLock: create / open / destroy / id
 * ################################################################ */

qwslock_t *qwslock_create(void)
{
    qwslock_t *lock = malloc(sizeof(struct qws_lock));
    if (!lock) return NULL;
    memset(lock, 0, sizeof(struct qws_lock));
    _lock_base_init(&lock->base, true);

#ifndef QWS_IPC_POSIX
    if (_sysv_create(&lock->base, IPC_PRIVATE, LOCK_NUM_SEMS, qwslock_init_vals) != 0) {
        free(lock);
        return NULL;
    }
#else
    static int posix_counter = 0;
    lock->base.posix_id = (getpid() << 8) | (posix_counter++ & 0xFF);
    snprintf(lock->base.posix_prefix, PATH_MAX, "/qwslock_%x_", lock->base.posix_id);
    if (_posix_sems(&lock->base, LOCK_NUM_SEMS, qwslock_init_vals, true, false) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

qwslock_t *qwslock_open(int id)
{
    qwslock_t *lock = malloc(sizeof(struct qws_lock));
    if (!lock) return NULL;
    memset(lock, 0, sizeof(struct qws_lock));
    _lock_base_init(&lock->base, false);

#ifndef QWS_IPC_POSIX
    lock->base.sysv_semid = id;
#else
    lock->base.posix_id = id;
    snprintf(lock->base.posix_prefix, PATH_MAX, "/qwslock_%x_", id);
    if (_posix_sems(&lock->base, LOCK_NUM_SEMS, qwslock_init_vals, false, false) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

void qwslock_destroy(qwslock_t *lock)
{
    if (!lock) return;
    _lock_base_cleanup(&lock->base, false);
    free(lock);
}

int qwslock_id(const qwslock_t *lock)
{
#ifndef QWS_IPC_POSIX
    return lock->base.sysv_semid;
#else
    return lock->base.posix_id;
#endif
}

/* ################################################################
 * QWSLock: lock / unlock / wait / get_value
 * ################################################################ */

int qwslock_lock(qwslock_t *lock, qwslock_type_t which)
{
    if (which < 0 || which >= LOCK_NUM_SEMS)
        return -1;

    /* Track nesting for BackingStore and Communication */
    if (which < 2) {
        if (lock->lock_count[which] > 0) {
            lock->lock_count[which]++;
            return 0;
        }
    }

    int ret;
#ifndef QWS_IPC_POSIX
    ret = sysv_sem_down(lock->base.sysv_semid, which);
#else
    if (lock->base.posix_sems[which] == SEM_FAILED)
        return -1;
    ret = posix_sem_wait_eintr(lock->base.posix_sems[which]);
#endif

    if (ret == 0 && which < 2)
        lock->lock_count[which] = 1;

    return ret;
}

int qwslock_unlock(qwslock_t *lock, qwslock_type_t which)
{
    if (which < 0 || which >= LOCK_NUM_SEMS)
        return -1;

    /* Track nesting */
    if (which < 2) {
        if (lock->lock_count[which] > 1) {
            lock->lock_count[which]--;
            return 0;
        }
        lock->lock_count[which] = 0;
    }

#ifndef QWS_IPC_POSIX
    return sysv_sem_up(lock->base.sysv_semid, which);
#else
    if (lock->base.posix_sems[which] == SEM_FAILED)
        return -1;
    return sem_post(lock->base.posix_sems[which]);
#endif
}

int qwslock_wait(qwslock_t *lock, qwslock_type_t which)
{
    return qwslock_lock(lock, which);
}

int qwslock_get_value(const qwslock_t *lock, qwslock_type_t which)
{
    if (which < 0 || which >= LOCK_NUM_SEMS)
        return -1;
#ifndef QWS_IPC_POSIX
    return sysv_sem_getval(lock->base.sysv_semid, which);
#else
    if (lock->base.posix_sems[which] == SEM_FAILED)
        return -1;
    int val = 0;
    if (sem_getvalue(lock->base.posix_sems[which], &val) == -1)
        return -1;
    return val;
#endif
}

/* ################################################################
 * QLock: create / open / destroy
 * ################################################################ */

qlock_t *qlock_create(const char *filename, char id)
{
    qlock_t *lock = malloc(sizeof(struct q_lock));
    if (!lock) return NULL;
    memset(lock, 0, sizeof(struct q_lock));
    _lock_base_init(&lock->base, true);
    lock->id = id;

#ifndef QWS_IPC_POSIX
    key_t key = ftok(filename, (int)(unsigned char)id);
    if (key == (key_t)-1) {
        perror("qlock_create: ftok");
        free(lock);
        return NULL;
    }
    if (_sysv_create(&lock->base, key, 3, qlock_init_vals) != 0) {
        free(lock);
        return NULL;
    }
#else
    snprintf(lock->base.posix_prefix, PATH_MAX, "%s%c", filename, id);
    if (_posix_sems(&lock->base, 3, qlock_init_vals, true, true) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

qlock_t *qlock_open(const char *filename, char id)
{
    qlock_t *lock = malloc(sizeof(struct q_lock));
    if (!lock) return NULL;
    memset(lock, 0, sizeof(struct q_lock));
    _lock_base_init(&lock->base, false);
    lock->id = id;

#ifndef QWS_IPC_POSIX
    key_t key = ftok(filename, (int)(unsigned char)id);
    if (key == (key_t)-1) {
        perror("qlock_open: ftok");
        free(lock);
        return NULL;
    }
    lock->base.sysv_semid = semget(key, 1, 0);
    if (lock->base.sysv_semid == -1) {
        perror("qlock_open: semget");
        free(lock);
        return NULL;
    }
#else
    snprintf(lock->base.posix_prefix, PATH_MAX, "%s%c", filename, id);
    if (_posix_sems(&lock->base, 3, qlock_init_vals, false, true) != 0) {
        free(lock);
        return NULL;
    }
#endif
    return lock;
}

void qlock_destroy(qlock_t *lock)
{
    if (!lock) return;
    _lock_base_cleanup(&lock->base, true);
    free(lock);
}

/* ################################################################
 * QLock: lock / unlock
 *
 * POSIX path implements Qt 4.8's qlock.cpp algorithm exactly.
 * Semaphores: c (counter, init MAX_LOCKS), rsem (read gate, init 1),
 *             wsem (write mutex, init 1).
 * ################################################################ */

int qlock_lock(qlock_t *lock, qlock_type_t type)
{
#ifndef QWS_IPC_POSIX
    short op_val = (type == QWS_QLOCK_WRITE) ? -(short)QLOCK_MAX_LOCKS : -1;
    struct sembuf op = { .sem_num = 0, .sem_op = op_val, .sem_flg = 0 };
    return sysv_semop_eintr(lock->base.sysv_semid, &op, 1);
#else
    sem_t *c    = lock->base.posix_sems[0]; /* counter */
    sem_t *rsem = lock->base.posix_sems[1]; /* read gate */
    sem_t *wsem = lock->base.posix_sems[2]; /* write mutex */

    if (type == QWS_QLOCK_WRITE) {
        if (posix_sem_wait_eintr(rsem) == -1) return -1;
        if (posix_sem_wait_eintr(wsem) == -1) { sem_post(rsem); return -1; }
    } else {
        if (posix_sem_wait_eintr(wsem) == -1) return -1;
        sem_trywait(rsem);   /* mark "readers active"; EAGAIN is fine */
        if (posix_sem_wait_eintr(c) == -1) {
            int v = 0;
            sem_getvalue(c, &v);
            if (v == QLOCK_MAX_LOCKS) sem_post(rsem);
            sem_post(wsem);
            return -1;
        }
        sem_post(wsem);
    }
    return 0;
#endif
}

int qlock_unlock(qlock_t *lock, qlock_type_t type)
{
#ifndef QWS_IPC_POSIX
    short op_val = (type == QWS_QLOCK_WRITE) ? (short)QLOCK_MAX_LOCKS : 1;
    struct sembuf op = { .sem_num = 0, .sem_op = op_val, .sem_flg = 0 };
    return sysv_semop_eintr(lock->base.sysv_semid, &op, 1);
#else
    sem_t *c    = lock->base.posix_sems[0];
    sem_t *rsem = lock->base.posix_sems[1];
    sem_t *wsem = lock->base.posix_sems[2];

    if (type == QWS_QLOCK_WRITE) {
        sem_post(wsem);
        return sem_post(rsem);
    } else {
        if (posix_sem_wait_eintr(wsem) == -1) return -1;
        sem_post(c);
        int v = 0;
        sem_getvalue(c, &v);
        if (v == QLOCK_MAX_LOCKS) sem_post(rsem);
        return sem_post(wsem);
    }
#endif
}
