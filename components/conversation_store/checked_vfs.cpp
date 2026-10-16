#include "checked_vfs.hpp"
#include "sqlite3.h"
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef CONVERSATION_STORE_HOST
#include <sys/random.h>
#include <sys/file.h>
#include <atomic>
#else
#include "esp_random.h"
#endif

namespace conversation_store::internal {
namespace {
char database_path[240] = {};
#ifdef CONVERSATION_STORE_HOST
std::atomic<int> fault_kind{0}, fault_after{0};
bool fail(int kind) {
    if (fault_kind.load() != kind) return false;
    if (fault_after.fetch_sub(1) > 0) return false;
    fault_kind.store(0);
    return true;
}
#else
bool fail(int) { return false; }
#endif
struct File { sqlite3_file base; int fd; int lock; };
File *file(sqlite3_file *f) { return reinterpret_cast<File *>(f); }
int close_file(sqlite3_file *f) {
    int result = close(file(f)->fd);
    f->pMethods = nullptr;
    return result == 0 ? SQLITE_OK : SQLITE_IOERR_CLOSE;
}
int read_file(sqlite3_file *f, void *out, int n, sqlite3_int64 offset) {
    if (offset < 0 || lseek(file(f)->fd, offset, SEEK_SET) != offset) return SQLITE_IOERR_SEEK;
    ssize_t got;
    do { got = read(file(f)->fd, out, fail(1) ? n / 2 : n); } while (got < 0 && errno == EINTR);
    if (got < 0) return SQLITE_IOERR_READ;
    if (got != n) {
        memset(static_cast<char *>(out) + got, 0, n - got);
        struct stat st;
        if (fstat(file(f)->fd, &st)) return SQLITE_IOERR_FSTAT;
        // SQLite accepts SHORT_READ as zero-filled EOF. Missing bytes that
        // still belong to the file are an actual I/O failure, never EOF.
        return offset < st.st_size && got < st.st_size - offset
            ? SQLITE_IOERR_READ : SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}
int write_file(sqlite3_file *f, const void *in, int n, sqlite3_int64 offset) {
    if (fail(5)) return SQLITE_FULL;
    if (offset < 0 || lseek(file(f)->fd, offset, SEEK_SET) != offset) return SQLITE_IOERR_SEEK;
    ssize_t put;
    do { put = write(file(f)->fd, in, fail(2) ? n / 2 : n); } while (put < 0 && errno == EINTR);
    if (put < 0) return errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
    return put == n ? SQLITE_OK : SQLITE_IOERR_WRITE;
}
int truncate_file(sqlite3_file *f, sqlite3_int64 size) {
    if (fail(4)) return SQLITE_IOERR_TRUNCATE;
    return ftruncate(file(f)->fd, size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}
int sync_file(sqlite3_file *f, int) {
    if (fail(3)) return SQLITE_IOERR_FSYNC;
    return fsync(file(f)->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}
int size_file(sqlite3_file *f, sqlite3_int64 *out) {
    struct stat st;
    if (fstat(file(f)->fd, &st)) return SQLITE_IOERR_FSTAT;
    *out = st.st_size;
    return SQLITE_OK;
}
// Only this component's one connection may open this VFS. ESP has no second process;
// host additionally takes flock(LOCK_EX|LOCK_NB) for the connection's whole lifetime.
// SQLite's intra-connection lock transitions are bookkeeping, not fake cross-process locking.
int lock_file(sqlite3_file *f, int lock) { file(f)->lock = lock; return SQLITE_OK; }
int unlock_file(sqlite3_file *f, int lock) { file(f)->lock = lock; return SQLITE_OK; }
int reserved(sqlite3_file *, int *out) { *out = 0; return SQLITE_OK; }
int control(sqlite3_file *f, int op, void *arg) {
    if (op == SQLITE_FCNTL_LOCKSTATE) { *static_cast<int *>(arg) = file(f)->lock; return SQLITE_OK; }
    if (op == SQLITE_FCNTL_VFSNAME) { *static_cast<char **>(arg) = sqlite3_mprintf("arvel-checked-fat"); return SQLITE_OK; }
    return SQLITE_NOTFOUND;
}
int sector(sqlite3_file *) { return 512; }
int characteristics(sqlite3_file *) { return 0; } // No atomic-write/safe-append/powersafe-overwrite claims.
const sqlite3_io_methods io = {1, close_file, read_file, write_file, truncate_file, sync_file,
    size_file, lock_file, unlock_file, reserved, control, sector, characteristics,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
bool allowed(const char *path) {
    if (!path || !database_path[0]) return false;
    size_t len = strlen(database_path);
    return strcmp(path, database_path) == 0 ||
           (strncmp(path, database_path, len) == 0 && strcmp(path + len, "-journal") == 0);
}
int open_file(sqlite3_vfs *, const char *path, sqlite3_file *out, int flags, int *actual) {
    memset(out, 0, sizeof(File));
    if (!allowed(path)) return SQLITE_CANTOPEN;
    int mode = flags & SQLITE_OPEN_READONLY ? O_RDONLY : O_RDWR;
    bool existed = access(path, F_OK) == 0;
    if (flags & SQLITE_OPEN_CREATE) mode |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) mode |= O_EXCL;
    int fd = open(path, mode, 0600);
    if (fd < 0) return errno == ENOSPC ? SQLITE_FULL : SQLITE_CANTOPEN;
#ifdef CONVERSATION_STORE_HOST
    if ((flags & SQLITE_OPEN_MAIN_DB) && flock(fd, LOCK_EX | LOCK_NB)) { close(fd); return SQLITE_BUSY; }
    // POSIX requires syncing the parent directory after creation. FAT f_sync
    // itself commits the file's directory entry; ESP's VFS does not open directories as fds.
    if (!existed && (flags & SQLITE_OPEN_CREATE)) {
        char parent[240]; snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
        int dir = open(slash ? parent : ".", O_RDONLY | O_DIRECTORY);
        if (dir < 0 || fsync(dir)) { if (dir >= 0) close(dir); close(fd); return SQLITE_IOERR_DIR_FSYNC; }
        if (close(dir)) { close(fd); return SQLITE_IOERR_CLOSE; }
    }
#else
    (void)existed;
#endif
    file(out)->fd = fd;
    file(out)->lock = SQLITE_LOCK_NONE;
    out->pMethods = &io;
    if (actual) *actual = flags;
    return SQLITE_OK;
}
int delete_file(sqlite3_vfs *, const char *path, int sync_dir) {
    if (!allowed(path)) return SQLITE_IOERR_DELETE;
    if (unlink(path) && errno != ENOENT) return SQLITE_IOERR_DELETE;
#ifdef CONVERSATION_STORE_HOST
    if (sync_dir) {
        char parent[240]; snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
        int fd = open(slash ? parent : ".", O_RDONLY | O_DIRECTORY);
        if (fd < 0) return SQLITE_IOERR_DIR_FSYNC;
        int rc = fsync(fd), closed = close(fd);
        if (rc || closed) return SQLITE_IOERR_DIR_FSYNC;
    }
#else
    (void)sync_dir; // Normal operation is PERSIST; no journal unlink at commit.
#endif
    return SQLITE_OK;
}
int access_file(sqlite3_vfs *, const char *path, int flags, int *out) {
    if (!allowed(path)) {
        const size_t length = strlen(database_path);
        if (!path || !length || strncmp(path, database_path, length) ||
            (strcmp(path + length, "-wal") && strcmp(path + length, "-shm")))
            return SQLITE_IOERR_ACCESS;
        // SQLite probes for an associated WAL even in rollback-journal mode.
        // Absence is normal; a present/inaccessible unsupported journal must
        // refuse opening, never be hidden or discarded as if recovery succeeded.
        *out = 0;
        return access(path, F_OK) != 0 && errno == ENOENT ? SQLITE_OK : SQLITE_IOERR_ACCESS;
    }
    int mode = flags == SQLITE_ACCESS_READWRITE ? R_OK | W_OK : flags == SQLITE_ACCESS_READ ? R_OK : F_OK;
    *out = access(path, mode) == 0;
    return *out || errno == ENOENT || errno == EACCES ? SQLITE_OK : SQLITE_IOERR_ACCESS;
}
int full_path(sqlite3_vfs *, const char *path, int size, char *out) {
    if (!allowed(path) || path[0] != '/' || strlen(path) >= static_cast<size_t>(size)) return SQLITE_CANTOPEN;
    strcpy(out, path); return SQLITE_OK;
}
int random_bytes(sqlite3_vfs *, int n, char *out) {
#ifdef CONVERSATION_STORE_HOST
    int done = 0;
    while (done < n) { ssize_t got = getrandom(out + done, n - done, 0); if (got < 0 && errno == EINTR) continue; if (got <= 0) return done; done += got; }
#else
    esp_fill_random(out, n);
#endif
    return n;
}
int sleep_us(sqlite3_vfs *, int us) { usleep(us); return us; }
int current_time(sqlite3_vfs *, double *out) {
    struct timeval tv; if (gettimeofday(&tv, nullptr)) return SQLITE_ERROR;
    *out = 2440587.5 + tv.tv_sec / 86400.0 + tv.tv_usec / 86400000000.0; return SQLITE_OK;
}
int last_error(sqlite3_vfs *, int n, char *out) { if (n > 0) snprintf(out, n, "%s", strerror(errno)); return errno; }
sqlite3_vfs vfs = {1, sizeof(File), 240, nullptr, "arvel-checked-fat", nullptr,
    open_file, delete_file, access_file, full_path, nullptr, nullptr, nullptr, nullptr,
    random_bytes, sleep_us, current_time, last_error, nullptr, nullptr, nullptr, nullptr};
}
int configure_vfs(const char *path) {
    if (!path || path[0] != '/' || strlen(path) > 220 || strstr(path, "/../") || strstr(path, "/./")) return SQLITE_CANTOPEN;
    strcpy(database_path, path); return SQLITE_OK;
}
bool random(void *out, size_t bytes) { return random_bytes(nullptr, static_cast<int>(bytes), static_cast<char *>(out))==static_cast<int>(bytes); }
#ifdef CONVERSATION_STORE_HOST
void set_fault(int kind, int after) { fault_after.store(after); fault_kind.store(kind); }
#endif
}
extern "C" int sqlite3_os_init(void) { return sqlite3_vfs_register(&conversation_store::internal::vfs, 1); }
extern "C" int sqlite3_os_end(void) { return SQLITE_OK; }
