/*
 * libsuco-trace.so — LD_PRELOAD I/O tracer for `suco run` auto-I/O-tracking (A4).
 *
 * Intercepts the file-open syscalls and appends one line per accessed path to the
 * file named in $SUCO_TRACE_FILE:
 *     R<TAB>/abs/path      (opened read-only            -> input candidate)
 *     W<TAB>/abs/path      (opened for write/create     -> output candidate)
 * `suco run` (learn mode) runs the command once locally under this shim, then
 * classifies cwd-relative paths into inputs (R, existed before) and outputs (W),
 * so the user no longer needs --in/--out. This mirrors IncrediBuild's transparent
 * dependency capture, but portably (no kernel module).
 *
 * Deliberately minimal & fail-open: any error just skips logging — never breaks
 * the traced command. Built with -fvisibility=hidden except the wrappers.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_access(char kind, const char *path) {
    if (!path || !path[0]) return;
    const char *tf = getenv("SUCO_TRACE_FILE");
    if (!tf || !tf[0]) return;
    /* Skip the trace file itself and obvious non-dependency noise. */
    if (strcmp(path, tf) == 0) return;

    /* Resolve to an absolute path so the classifier can compare against cwd. */
    char abs[4096];
    if (path[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", path);
    } else {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
        else snprintf(abs, sizeof(abs), "%s", path);
    }

    pthread_mutex_lock(&g_lock);
    FILE *f = fopen(tf, "a");
    if (f) { fprintf(f, "%c\t%s\n", kind, abs); fclose(f); }
    pthread_mutex_unlock(&g_lock);
}

static char classify(int flags) {
    /* O_WRONLY/O_RDWR or creating/truncating => the command produces this file. */
    if ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR) return 'W';
    if (flags & (O_CREAT | O_TRUNC)) return 'W';
    return 'R';
}

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef FILE *(*fopen_fn)(const char *, const char *);

int open(const char *path, int flags, ...) {
    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int fd = real(path, flags, mode);
    if (fd >= 0) log_access(classify(flags), path);
    return fd;
}

int open64(const char *path, int flags, ...) {
    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open64");
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int fd = real(path, flags, mode);
    if (fd >= 0) log_access(classify(flags), path);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...) {
    static openat_fn real = NULL;
    if (!real) real = (openat_fn)dlsym(RTLD_NEXT, "openat");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int fd = real(dirfd, path, flags, mode);
    /* Only log absolute or AT_FDCWD-relative paths (the common case). */
    if (fd >= 0 && (path[0] == '/' || dirfd == AT_FDCWD)) log_access(classify(flags), path);
    return fd;
}

FILE *fopen(const char *path, const char *mode) {
    static fopen_fn real = NULL;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    FILE *f = real(path, mode);
    if (f && mode) log_access((mode[0] == 'r' && !strchr(mode, '+')) ? 'R' : 'W', path);
    return f;
}

FILE *fopen64(const char *path, const char *mode) {
    static fopen_fn real = NULL;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen64");
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    FILE *f = real(path, mode);
    if (f && mode) log_access((mode[0] == 'r' && !strchr(mode, '+')) ? 'R' : 'W', path);
    return f;
}
