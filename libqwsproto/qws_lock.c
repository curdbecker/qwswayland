/*
 * qws_lock.c - QWS locking primitives implementation
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
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

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
 * Internal: EINTR-safe semop wrapper
 * ================================================================ */

static int sysv_semop_eintr(int semid, struct sembuf *sops, size_t nsops)
{
    int ret;
    do {
        ret = semop(semid, sops, nsops);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

/* ================================================================
 * Internal: EINTR-safe sem_wait wrapper
 * ================================================================ */

static int posix_sem_wait_eintr(sem_t *sem)
{
    int ret;
    do {
        ret = sem_wait(sem);
    } while (ret == -1 && errno == EINTR);
    return ret;
}


/* ################################################################
 * QWSLock: per-client 3-semaphore lock
 * ################################################################ */

/* Initial values: BackingStore=1, Communication=1, RegionEvent=0 */
static const unsigned short qws_lock_init_vals[QWS_LOCK_NUM_SEMS] = { 1, 1, 0 };

/* POSIX sem name suffixes */
static const char *qws_lock_posix_suffixes[QWS_LOCK_NUM_SEMS] = {
    "BackingStore",
    "Communication",
    "RegionEvent",
};

/* ----------------------------------------------------------------
 * SysV backend helpers
 * ---------------------------------------------------------------- */

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

static int sysv_sem_trydown(int semid, int idx)
{
    struct sembuf op = { .sem_num = (unsigned short)idx, .sem_op = -1, .sem_flg = IPC_NOWAIT };
    return semop(semid, &op, 1);
}

static int sysv_sem_getval(int semid, int idx)
{
    return semctl(semid, idx, GETVAL);
}

/* ----------------------------------------------------------------
 * Create (server-side)
 * ---------------------------------------------------------------- */

int qws_lock_create(qws_lock_t *lock, qws_ipc_type_t type)
{
    memset(lock, 0, sizeof(*lock));
    lock->ipc_type = type;
    lock->owned = true;
    lock->sysv_semid = -1;
    for (int i = 0; i < QWS_LOCK_NUM_SEMS; i++)
        lock->posix_sems[i] = SEM_FAILED;

    if (type == QWS_IPC_SYSV) {
        int semid = semget(IPC_PRIVATE, QWS_LOCK_NUM_SEMS, IPC_CREAT | 0666);
        if (semid == -1) {
            perror("qws_lock_create: semget");
            return -1;
        }

        union semun arg;
        arg.array = (unsigned short *)qws_lock_init_vals;
        if (semctl(semid, 0, SETALL, arg) == -1) {
            perror("qws_lock_create: semctl SETALL");
            semctl(semid, 0, IPC_RMID);
            return -1;
        }

        lock->sysv_semid = semid;
        return 0;

    } else { /* POSIX */
        /*
         * Generate a unique numeric ID for naming.
         * Qt 4.8 uses (getpid() << 16) + atomic_counter.
         * We use (getpid() << 8) + a small counter.
         */
        static int posix_counter = 0;
        lock->posix_id = (getpid() << 8) | (posix_counter++ & 0xFF);

        char name[96];
        for (int i = 0; i < QWS_LOCK_NUM_SEMS; i++) {
            snprintf(name, sizeof(name), "/qwslock_%x_%s",
                     lock->posix_id, qws_lock_posix_suffixes[i]);

            /* Remove stale semaphore */
            sem_unlink(name);

            sem_t *sem;
            do {
                sem = sem_open(name, O_CREAT, 0666, qws_lock_init_vals[i]);
            } while (sem == SEM_FAILED && errno == EINTR);

            if (sem == SEM_FAILED) {
                perror("qws_lock_create: sem_open");
                /* Clean up already created sems */
                for (int j = 0; j < i; j++) {
                    sem_close(lock->posix_sems[j]);
                    snprintf(name, sizeof(name), "/qwslock_%x_%s",
                             lock->posix_id, qws_lock_posix_suffixes[j]);
                    sem_unlink(name);
                }
                return -1;
            }
            lock->posix_sems[i] = sem;
        }
        return 0;
    }
}

/* ----------------------------------------------------------------
 * Open (client-side attach)
 * ---------------------------------------------------------------- */

int qws_lock_open(qws_lock_t *lock, qws_ipc_type_t type, int id)
{
    memset(lock, 0, sizeof(*lock));
    lock->ipc_type = type;
    lock->owned = false;
    lock->sysv_semid = -1;
    for (int i = 0; i < QWS_LOCK_NUM_SEMS; i++)
        lock->posix_sems[i] = SEM_FAILED;

    if (type == QWS_IPC_SYSV) {
        /* For SysV, the id IS the semaphore set id */
        lock->sysv_semid = id;
        return 0;

    } else { /* POSIX */
        lock->posix_id = id;
        char name[96];
        for (int i = 0; i < QWS_LOCK_NUM_SEMS; i++) {
            snprintf(name, sizeof(name), "/qwslock_%x_%s",
                     id, qws_lock_posix_suffixes[i]);

            sem_t *sem;
            do {
                sem = sem_open(name, 0);
            } while (sem == SEM_FAILED && errno == EINTR);

            if (sem == SEM_FAILED) {
                perror("qws_lock_open: sem_open");
                for (int j = 0; j < i; j++)
                    sem_close(lock->posix_sems[j]);
                return -1;
            }
            lock->posix_sems[i] = sem;
        }
        return 0;
    }
}

/* ----------------------------------------------------------------
 * Destroy
 * ---------------------------------------------------------------- */

void qws_lock_destroy(qws_lock_t *lock)
{
    if (!lock)
        return;

    if (lock->ipc_type == QWS_IPC_SYSV) {
        if (lock->owned && lock->sysv_semid >= 0)
            semctl(lock->sysv_semid, 0, IPC_RMID);
    } else {
        char name[96];
        for (int i = 0; i < QWS_LOCK_NUM_SEMS; i++) {
            if (lock->posix_sems[i] != SEM_FAILED) {
                sem_close(lock->posix_sems[i]);
                if (lock->owned) {
                    snprintf(name, sizeof(name), "/qwslock_%x_%s",
                             lock->posix_id, qws_lock_posix_suffixes[i]);
                    sem_unlink(name);
                }
            }
        }
    }

    memset(lock, 0, sizeof(*lock));
    lock->sysv_semid = -1;
    for (int i = 0; i < QWS_LOCK_NUM_SEMS; i++)
        lock->posix_sems[i] = SEM_FAILED;
}

/* ----------------------------------------------------------------
 * ID accessor
 * ---------------------------------------------------------------- */

int qws_lock_id(const qws_lock_t *lock)
{
    if (lock->ipc_type == QWS_IPC_SYSV)
        return lock->sysv_semid;
    else
        return lock->posix_id;
}

/* ----------------------------------------------------------------
 * Lock / unlock / trylock
 * ---------------------------------------------------------------- */

int qws_lock_lock(qws_lock_t *lock, int which)
{
    if (which < 0 || which >= QWS_LOCK_NUM_SEMS)
        return -1;

    /* Track nesting for BackingStore and Communication */
    if (which < 2) {
        if (lock->lock_count[which] > 0) {
            lock->lock_count[which]++;
            return 0;
        }
    }

    int ret;
    if (lock->ipc_type == QWS_IPC_SYSV) {
        ret = sysv_sem_down(lock->sysv_semid, which);
    } else {
        if (lock->posix_sems[which] == SEM_FAILED)
            return -1;
        ret = posix_sem_wait_eintr(lock->posix_sems[which]);
    }

    if (ret == 0 && which < 2)
        lock->lock_count[which] = 1;

    return ret;
}

int qws_lock_unlock(qws_lock_t *lock, int which)
{
    if (which < 0 || which >= QWS_LOCK_NUM_SEMS)
        return -1;

    /* Track nesting */
    if (which < 2) {
        if (lock->lock_count[which] > 1) {
            lock->lock_count[which]--;
            return 0;
        }
        lock->lock_count[which] = 0;
    }

    if (lock->ipc_type == QWS_IPC_SYSV) {
        return sysv_sem_up(lock->sysv_semid, which);
    } else {
        if (lock->posix_sems[which] == SEM_FAILED)
            return -1;
        return sem_post(lock->posix_sems[which]);
    }
}

int qws_lock_trylock(qws_lock_t *lock, int which)
{
    if (which < 0 || which >= QWS_LOCK_NUM_SEMS)
        return -1;

    if (which < 2 && lock->lock_count[which] > 0) {
        lock->lock_count[which]++;
        return 0;
    }

    int ret;
    if (lock->ipc_type == QWS_IPC_SYSV) {
        ret = sysv_sem_trydown(lock->sysv_semid, which);
    } else {
        if (lock->posix_sems[which] == SEM_FAILED)
            return -1;
        ret = sem_trywait(lock->posix_sems[which]);
    }

    if (ret == 0 && which < 2)
        lock->lock_count[which] = 1;

    return ret;
}

int qws_lock_wait(qws_lock_t *lock, int which)
{
    /* Wait blocks until the semaphore becomes > 0, then decrements it.
     * This is the same as lock() — the name differs for clarity
     * (used for RegionEvent signaling). */
    return qws_lock_lock(lock, which);
}

int qws_lock_get_value(const qws_lock_t *lock, int which)
{
    if (which < 0 || which >= QWS_LOCK_NUM_SEMS)
        return -1;

    if (lock->ipc_type == QWS_IPC_SYSV) {
        return sysv_sem_getval(lock->sysv_semid, which);
    } else {
        if (lock->posix_sems[which] == SEM_FAILED)
            return -1;
        int val = 0;
        if (sem_getvalue(lock->posix_sems[which], &val) == -1)
            return -1;
        return val;
    }
}

bool qws_lock_has_lock(const qws_lock_t *lock, int which)
{
    if (which < 0 || which >= 2)
        return false;
    return lock->lock_count[which] > 0;
}
