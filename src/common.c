#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void init_paths(char runtime_dir[MAX_FIFO_PATH], char fifo_path[MAX_FIFO_PATH], char log_path[MAX_FIFO_PATH]) {
    const char *runtime_env = getenv("TP_RUNTIME_DIR");
    const char *fifo_env = getenv("TP_CONTROLLER_FIFO");
    const char *log_env = getenv("TP_LOG_PATH");

    if (runtime_env != NULL && runtime_env[0] != '\0') {
        snprintf(runtime_dir, MAX_FIFO_PATH, "%s", runtime_env);
    } else {
        snprintf(runtime_dir, MAX_FIFO_PATH, "/tmp/so_tp_%ld", (long)getuid());
    }

    if (fifo_env != NULL && fifo_env[0] != '\0') {
        snprintf(fifo_path, MAX_FIFO_PATH, "%s", fifo_env);
    } else {
        snprintf(fifo_path, MAX_FIFO_PATH, "%s/controller.fifo", runtime_dir);
    }

    if (log_env != NULL && log_env[0] != '\0') {
        snprintf(log_path, MAX_FIFO_PATH, "%s", log_env);
    } else {
        snprintf(log_path, MAX_FIFO_PATH, "%s", TP_LOG_FILE);
    }
}

const char *tp_runtime_dir(void) {
    static int initialized = 0;
    static char runtime_dir[MAX_FIFO_PATH];
    static char fifo_path[MAX_FIFO_PATH];
    static char log_path[MAX_FIFO_PATH];

    if (!initialized) {
        init_paths(runtime_dir, fifo_path, log_path);
        initialized = 1;
    }

    return runtime_dir;
}

const char *tp_controller_fifo(void) {
    static int initialized = 0;
    static char runtime_dir[MAX_FIFO_PATH];
    static char fifo_path[MAX_FIFO_PATH];
    static char log_path[MAX_FIFO_PATH];

    if (!initialized) {
        init_paths(runtime_dir, fifo_path, log_path);
        initialized = 1;
    }

    return fifo_path;
}

const char *tp_log_path(void) {
    static int initialized = 0;
    static char runtime_dir[MAX_FIFO_PATH];
    static char fifo_path[MAX_FIFO_PATH];
    static char log_path[MAX_FIFO_PATH];

    if (!initialized) {
        init_paths(runtime_dir, fifo_path, log_path);
        initialized = 1;
    }

    return log_path;
}

ssize_t write_full(int fd, const void *buf, size_t count) {
    const char *ptr = (const char *)buf;
    size_t written = 0;

    while (written < count) {
        ssize_t n = write(fd, ptr + written, count - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        written += (size_t)n;
    }

    return (ssize_t)written;
}

ssize_t read_full(int fd, void *buf, size_t count) {
    char *ptr = (char *)buf;
    size_t total = 0;

    while (total < count) {
        ssize_t n = read(fd, ptr + total, count - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }

    return (ssize_t)total;
}

int ensure_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        return 0;
    }

    return -1;
}

long timeval_diff_ms(struct timeval start, struct timeval end) {
    long sec = (long)(end.tv_sec - start.tv_sec);
    long usec = (long)(end.tv_usec - start.tv_usec);
    return sec * 1000L + usec / 1000L;
}

int safe_snprintf(char *dst, size_t dst_size, const char *fmt, ...) {
    va_list ap;
    int rc;

    if (dst_size == 0) {
        return -1;
    }

    va_start(ap, fmt);
    rc = vsnprintf(dst, dst_size, fmt, ap);
    va_end(ap);

    if (rc < 0 || (size_t)rc >= dst_size) {
        dst[dst_size - 1] = '\0';
        return -1;
    }

    return rc;
}

int build_command_line(char *out, size_t out_size, char *const argv[], int start_idx, int argc) {
    size_t used = 0;

    if (out_size == 0) {
        return -1;
    }

    out[0] = '\0';

    for (int i = start_idx; i < argc; ++i) {
        const char *token = argv[i];
        size_t token_len = strlen(token);

        if (i > start_idx) {
            if (used + 1 >= out_size) {
                return -1;
            }
            out[used++] = ' ';
            out[used] = '\0';
        }

        if (used + token_len >= out_size) {
            return -1;
        }

        memcpy(out + used, token, token_len);
        used += token_len;
        out[used] = '\0';
    }

    return 0;
}

int send_response_to_fifo(const char *fifo_path, const ResponseMessage *resp) {
    int fd = open(fifo_path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t written = write_full(fd, resp, sizeof(*resp));
    int saved_errno = errno;
    close(fd);

    if (written != (ssize_t)sizeof(*resp)) {
        errno = saved_errno;
        return -1;
    }

    return 0;
}

int write_request_to_controller(const RequestMessage *req, int nonblock) {
    int flags = O_WRONLY;
    if (nonblock) {
        flags |= O_NONBLOCK;
    }

    int fd = open(tp_controller_fifo(), flags);
    if (fd < 0) {
        return -1;
    }

    ssize_t written = write_full(fd, req, sizeof(*req));
    int saved_errno = errno;
    close(fd);

    if (written != (ssize_t)sizeof(*req)) {
        errno = saved_errno;
        return -1;
    }

    return 0;
}

int generate_command_id(pid_t pid) {
    struct timeval tv;
    static unsigned int seq = 0;
    unsigned int hash;
    unsigned int local_seq;

    gettimeofday(&tv, NULL);
    local_seq = ++seq;
    hash = ((unsigned int)(tv.tv_sec & 0x7FFFF) << 11) ^
           ((unsigned int)(tv.tv_usec & 0xFFFFF)) ^
           ((unsigned int)pid << 3) ^
           local_seq;

    return (int)(hash & 0x7FFFFFFF);
}
