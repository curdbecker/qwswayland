# libqwsproto — locking internals

## IPC backend

The IPC backend (SysV vs POSIX) is selected at **compile time**:

| Meson option | Preprocessor define | Backend used |
|---|---|---|
| `ipc_backend=sysv` (default) | _(none)_ | SysV IPC (`semget`/`semop`/`semctl`) |
| `ipc_backend=posix` | `QWS_IPC_POSIX` | POSIX named semaphores (`sem_open`) |

This mirrors Qt 4.8's `QT_POSIX_IPC` compile flag. Match this option to your Qt build.

---

## QWSLock

Per-client lock with 3 semaphores used for client↔server synchronisation.
The server creates the lock and sends its numeric ID to the client in the
`CMD_IDENTIFY` message; the client attaches via `qws_lock_open`.

### Semaphore indices (`qws_lock_type_t`)

| Index | Name | Initial value | Purpose |
|---|---|---|---|
| 0 | `QWS_LOCK_BACKINGSTORE` | 1 | Guards shared pixel buffer access |
| 1 | `QWS_LOCK_COMMUNICATION` | 1 | Guards command/event socket exchanges |
| 2 | `QWS_LOCK_REGIONEVENT` | 0 | Signals pending region events to client |

### Key / name derivation

| Backend | Identification | Semaphore name |
|---|---|---|
| SysV | numeric `semid` (from `IPC_PRIVATE`) | sent to client as-is |
| POSIX | numeric `posix_id` = `(pid << 8) + counter` | `/qwslock_<hex_id>_BackingStore`, `…_Communication`, `…_RegionEvent` |

---

## QLock

General reader-writer lock. Identified by `(filename, id)` — multiple locks may
share a filename, differentiated by the `id` char. Mirrors Qt 4.8's `QLock` class.

### SysV backend

Single semaphore keyed via `ftok(filename, id)`. Initial value = `MAX_LOCKS` (200).

| Operation | `sem_op` |
|---|---|
| Read lock | `-1` |
| Read unlock | `+1` |
| Write lock | `-MAX_LOCKS` (blocks until all readers have released) |
| Write unlock | `+MAX_LOCKS` |

### POSIX backend

Three named semaphores: `<filename><id>c`, `<filename><id>r`, `<filename><id>w`.
Initial values: `c = MAX_LOCKS`, `r = 1`, `w = 1`.

| Semaphore | Role |
|---|---|
| `c` | capacity counter — one slot per concurrent reader (init MAX_LOCKS) |
| `rsem` (`r`) | read gate — held by readers; writer waits on it until readers finish |
| `wsem` (`w`) | write mutex — prevents concurrent writers / serialises read lock entry |

Reader-writer protocol (Qt 4.8 `qlock.cpp` algorithm):

| Operation | Sequence |
|---|---|
| Read lock | `wait(wsem)` → `trywait(rsem)` (EAGAIN OK) → `wait(c)` → `post(wsem)` |
| Read unlock | `wait(wsem)` → `post(c)` → if `val(c)==MAX_LOCKS`: `post(rsem)` → `post(wsem)` |
| Write lock | `wait(rsem)` → `wait(wsem)` |
| Write unlock | `post(wsem)` → `post(rsem)` |

`rsem` is taken by the first reader and held until the last reader releases (detected by
`c` returning to `MAX_LOCKS`). A writer waiting on `rsem` is unblocked only when all
readers have exited.
