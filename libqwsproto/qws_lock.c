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


/* ################################################################
 * QLock: display read/write lock
 * ################################################################
 *
 * SysV: single counting semaphore, init=MAX_LOCKS.
 *   Read:  sem_op = -1    (one reader slot)
 *   Write: sem_op = -MAX  (all slots = exclusive)
 *
 * POSIX: three named semaphores (matching Qt 4.8.7 qlock.cpp):
 *   <base>c = counter  (init MAX_LOCKS) — reader permit pool
 *   <base>r = rsem     (init 1)         — read-mode gate
 *   <base>w = wsem     (init 1)         — write-mode gate
 *
 * The <base> is formed from socket_path + id_char, matching:
 *   data->file = filename.toLocal8Bit() + id;
 * in the Qt source.
 */

int qws_display_lock_create(qws_display_lock_t *lock, qws_ipc_type_t type,
                              const char *socket_path, char id_char,
                              bool create)
{
    memset(lock, 0, sizeof(*lock));
    lock->ipc_type = type;
    lock->owned = create;
    lock->sysv_semid = -1;
    lock->posix_counter = SEM_FAILED;
    lock->posix_rsem = SEM_FAILED;
    lock->posix_wsem = SEM_FAILED;

    if (type == QWS_IPC_SYSV) {
        key_t key = ftok(socket_path, (int)id_char);
        if (key == (key_t)-1) {
            perror("qws_display_lock: ftok");
            return -1;
        }

        if (create) {
            /* Remove any stale semaphore */
            int old = semget(key, 0, 0);
            if (old != -1) {
                union semun arg;
                arg.val = 0;
                semctl(old, 0, IPC_RMID, arg);
            }

            int semid = semget(key, 1, IPC_CREAT | 0600);
            if (semid == -1) {
                perror("qws_display_lock: semget create");
                return -1;
            }

            union semun arg;
            arg.val = QWS_DISPLAY_LOCK_MAX;
            if (semctl(semid, 0, SETVAL, arg) == -1) {
                perror("qws_display_lock: semctl SETVAL");
                semctl(semid, 0, IPC_RMID);
                return -1;
            }

            lock->sysv_semid = semid;
        } else {
            int semid = semget(key, 0, 0);
            if (semid == -1) {
                perror("qws_display_lock: semget open");
                return -1;
            }
            lock->sysv_semid = semid;
        }
        return 0;

    } else { /* POSIX — three named sems matching Qt 4.8 QLock */
        /* Qt forms the base as: filename.toLocal8Bit() + id_char */
        snprintf(lock->posix_base, sizeof(lock->posix_base),
                 "%s%c", socket_path, id_char);

        /* Suffix chars and initial values per Qt source */
        const char suffixes[3] = { 'c', 'r', 'w' };
        const unsigned int init_vals[3] = { QWS_DISPLAY_LOCK_MAX, 1, 1 };
        sem_t **sems[3] = { &lock->posix_counter,
                            &lock->posix_rsem,
                            &lock->posix_wsem };

        for (int i = 0; i < 3; i++) {
            char name[140];
            snprintf(name, sizeof(name), "%s%c", lock->posix_base, suffixes[i]);

            if (create) {
                /* Try open first, close + unlink, then create fresh */
                sem_t *existing;
                do {
                    existing = sem_open(name, 0, 0666, 0);
                } while (existing == SEM_FAILED && errno == EINTR);
                if (existing != SEM_FAILED) {
                    sem_close(existing);
                    sem_unlink(name);
                }
                do {
                    *sems[i] = sem_open(name, O_CREAT, 0666, init_vals[i]);
                } while (*sems[i] == SEM_FAILED && errno == EINTR);
            } else {
                do {
                    *sems[i] = sem_open(name, 0, 0666, 0);
                } while (*sems[i] == SEM_FAILED && errno == EINTR);
            }

            if (*sems[i] == SEM_FAILED) {
                perror("qws_display_lock: sem_open");
                /* Cleanup already-opened ones */
                for (int j = 0; j < i; j++) {
                    sem_close(*sems[j]);
                    if (create) {
                        char n2[140];
                        snprintf(n2, sizeof(n2), "%s%c",
                                 lock->posix_base, suffixes[j]);
                        sem_unlink(n2);
                    }
                    *sems[j] = SEM_FAILED;
                }
                return -1;
            }
        }
        return 0;
    }
}

void qws_display_lock_destroy(qws_display_lock_t *lock)
{
    if (!lock)
        return;

    /* Unlock any held locks */
    while (lock->count > 0)
        qws_display_lock_unlock(lock, lock->last_mode);

    if (lock->ipc_type == QWS_IPC_SYSV) {
        if (lock->owned && lock->sysv_semid >= 0) {
            union semun arg;
            arg.val = 0;
            semctl(lock->sysv_semid, 0, IPC_RMID, arg);
        }
    } else {
        const char suffixes[3] = { 'c', 'r', 'w' };
        sem_t *sems[3] = { lock->posix_counter,
                           lock->posix_rsem,
                           lock->posix_wsem };
        for (int i = 0; i < 3; i++) {
            if (sems[i] != SEM_FAILED)
                sem_close(sems[i]);
            if (lock->owned) {
                char name[140];
                snprintf(name, sizeof(name), "%s%c",
                         lock->posix_base, suffixes[i]);
                sem_unlink(name);
            }
        }
    }

    memset(lock, 0, sizeof(*lock));
    lock->sysv_semid = -1;
    lock->posix_counter = SEM_FAILED;
    lock->posix_rsem = SEM_FAILED;
    lock->posix_wsem = SEM_FAILED;
}

int qws_display_lock_lock(qws_display_lock_t *lock, qws_dlock_mode_t mode)
{
    if (!qws_display_lock_is_valid(lock))
        return -1;

    /* Nestable: if already locked, just bump count */
    if (lock->count > 0) {
        lock->count++;
        return 0;
    }

    lock->last_mode = mode;
    int rv;

    if (lock->ipc_type == QWS_IPC_SYSV) {
        struct sembuf op = {
            .sem_num = 0,
            .sem_op = (short)(mode == QWS_DLOCK_WRITE
                              ? -QWS_DISPLAY_LOCK_MAX : -1),
            .sem_flg = SEM_UNDO,
        };
        rv = sysv_semop_eintr(lock->sysv_semid, &op, 1);

    } else {
        /* POSIX: follows exact Qt 4.8.7 qlock.cpp lock() logic */
        if (mode == QWS_DLOCK_WRITE) {
            rv = posix_sem_wait_eintr(lock->posix_rsem);
            if (rv != -1) {
                rv = posix_sem_wait_eintr(lock->posix_wsem);
                if (rv == -1)
                    sem_post(lock->posix_rsem);
            }
        } else { /* READ */
            rv = posix_sem_wait_eintr(lock->posix_wsem);
            if (rv != -1) {
                int trv = sem_trywait(lock->posix_rsem);
                if (trv != -1 || errno == EAGAIN) {
                    rv = posix_sem_wait_eintr(lock->posix_counter);
                    if (rv == -1) {
                        int semval = 0;
                        sem_getvalue(lock->posix_counter, &semval);
                        if (semval == QWS_DISPLAY_LOCK_MAX)
                            sem_post(lock->posix_rsem);
                    }
                }
                sem_post(lock->posix_wsem);
            }
        }
    }

    if (rv == 0)
        lock->count = 1;
    return rv;
}

int qws_display_lock_unlock(qws_display_lock_t *lock, qws_dlock_mode_t mode)
{
    if (!qws_display_lock_is_valid(lock))
        return -1;

    if (lock->count <= 0)
        return -1;

    if (lock->count > 1) {
        lock->count--;
        return 0;
    }

    lock->count = 0;
    int rv;

    if (lock->ipc_type == QWS_IPC_SYSV) {
        struct sembuf op = {
            .sem_num = 0,
            .sem_op = (short)(mode == QWS_DLOCK_WRITE
                              ? QWS_DISPLAY_LOCK_MAX : 1),
            .sem_flg = SEM_UNDO,
        };
        rv = sysv_semop_eintr(lock->sysv_semid, &op, 1);

    } else {
        /* POSIX: follows exact Qt 4.8.7 qlock.cpp unlock() logic */
        if (mode == QWS_DLOCK_WRITE) {
            sem_post(lock->posix_wsem);
            rv = sem_post(lock->posix_rsem) == 0 ? 0 : -1;
        } else { /* READ */
            rv = posix_sem_wait_eintr(lock->posix_wsem);
            if (rv != -1) {
                sem_post(lock->posix_counter);
                int semval = 0;
                sem_getvalue(lock->posix_counter, &semval);
                if (semval == QWS_DISPLAY_LOCK_MAX)
                    sem_post(lock->posix_rsem);
                rv = sem_post(lock->posix_wsem) == 0 ? 0 : -1;
            }
        }
    }

    return rv;
}

bool qws_display_lock_is_locked(const qws_display_lock_t *lock)
{
    return lock->count > 0;
}

bool qws_display_lock_is_valid(const qws_display_lock_t *lock)
{
    if (lock->ipc_type == QWS_IPC_SYSV)
        return lock->sysv_semid >= 0;
    else
        return lock->posix_counter != SEM_FAILED
            && lock->posix_rsem != SEM_FAILED
            && lock->posix_wsem != SEM_FAILED;
}