#include "common.h"
#include "runner_exec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void copy_cstr(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static void runner_print_stdout(const char *fmt, ...) {
    char buffer[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }

    if ((size_t)n >= sizeof(buffer)) {
        n = (int)sizeof(buffer) - 1;
    }

    write_full(STDOUT_FILENO, buffer, (size_t)n);
}

static void runner_print_stderr(const char *fmt, ...) {
    char buffer[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    if (n <= 0) {
        return;
    }

    if ((size_t)n >= sizeof(buffer)) {
        n = (int)sizeof(buffer) - 1;
    }

    write_full(STDERR_FILENO, buffer, (size_t)n);
}

static int setup_reply_channel(char reply_fifo[MAX_FIFO_PATH], int *reply_fd) {
    int fd;
    const char *runtime_dir = tp_runtime_dir();

    if (ensure_directory(runtime_dir, 0700) < 0) {
        return -1;
    }

    if (safe_snprintf(reply_fifo, MAX_FIFO_PATH, "%s/runner_%ld_%d.fifo",
                      runtime_dir, (long)getpid(), generate_command_id(getpid())) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }

    unlink(reply_fifo);
    if (mkfifo(reply_fifo, 0600) < 0) {
        return -1;
    }

    fd = open(reply_fifo, O_RDWR);
    if (fd < 0) {
        int saved_errno = errno;
        unlink(reply_fifo);
        errno = saved_errno;
        return -1;
    }

    *reply_fd = fd;
    return 0;
}

static void cleanup_reply_channel(const char *reply_fifo, int reply_fd) {
    if (reply_fd >= 0) {
        close(reply_fd);
    }
    if (reply_fifo != NULL && *reply_fifo != '\0') {
        unlink(reply_fifo);
    }
}

static int send_request_and_wait(RequestMessage *req, ResponseMessage *resp) {
    char reply_fifo[MAX_FIFO_PATH] = {0};
    int reply_fd = -1;
    ssize_t bytes;

    if (setup_reply_channel(reply_fifo, &reply_fd) < 0) {
        return -1;
    }

    copy_cstr(req->reply_fifo, sizeof(req->reply_fifo), reply_fifo);

    if (write_request_to_controller(req, 1) < 0) {
        int saved_errno = errno;
        cleanup_reply_channel(reply_fifo, reply_fd);
        errno = saved_errno;
        return -1;
    }

    bytes = read_full(reply_fd, resp, sizeof(*resp));
    cleanup_reply_channel(reply_fifo, reply_fd);

    if (bytes != (ssize_t)sizeof(*resp)) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int send_finish_notification(const char *user_id, int command_id, int exit_status) {
    RequestMessage req;

    memset(&req, 0, sizeof(req));
    req.type = REQ_FINISH;
    req.command_id = command_id;
    req.runner_pid = getpid();
    req.exit_status = exit_status;
    copy_cstr(req.user_id, sizeof(req.user_id), user_id);

    return write_request_to_controller(&req, 1);
}

static int is_duplicate_id_reject(const char *payload) {
    if (payload == NULL) {
        return 0;
    }
    return strstr(payload, "duplicate command-id") != NULL;
}

static int handle_execute(int argc, char *argv[]) {
    RequestMessage req;
    ResponseMessage resp;
    char command_line[MAX_COMMAND_LINE];
    const char *user_id;
    int command_id;
    int exit_code;
    int got_ack = 0;

    if (argc < 4) {
        runner_print_stderr("usage: ./runner -e <user-id> <command> [args...]\n");
        return 1;
    }

    user_id = argv[2];

    if (build_command_line(command_line, sizeof(command_line), argv, 3, argc) < 0) {
        runner_print_stderr("[runner] command line too long\n");
        return 1;
    }

    for (int attempt = 0; attempt < 5; ++attempt) {
        command_id = generate_command_id(getpid() + attempt);

        memset(&req, 0, sizeof(req));
        req.type = REQ_EXEC;
        req.command_id = command_id;
        req.runner_pid = getpid();
        copy_cstr(req.user_id, sizeof(req.user_id), user_id);
        copy_cstr(req.command_line, sizeof(req.command_line), command_line);

        runner_print_stdout("[runner] command %d submitted\n", command_id);

        if (send_request_and_wait(&req, &resp) < 0) {
            runner_print_stderr("[runner] failed to contact controller: %s\n", strerror(errno));
            return 1;
        }

        if (resp.type == RESP_REJECT && is_duplicate_id_reject(resp.payload)) {
            continue;
        }

        if (resp.type == RESP_REJECT) {
            if (resp.payload[0] != '\0') {
                runner_print_stdout("%s\n", resp.payload);
            } else {
                runner_print_stdout("[runner] command %d rejected\n", command_id);
            }
            return 1;
        }

        if (resp.type != RESP_ACK_EXEC) {
            runner_print_stderr("[runner] unexpected response from controller\n");
            return 1;
        }

        if (resp.command_id > 0) {
            command_id = resp.command_id;
        }
        got_ack = 1;
        break;
    }

    if (!got_ack) {
        runner_print_stderr("[runner] failed to allocate unique command id\n");
        return 1;
    }

    runner_print_stdout("[runner] executing command %d...\n", command_id);
    exit_code = execute_user_command(command_line);

    if (send_finish_notification(user_id, command_id, exit_code) < 0) {
        runner_print_stderr("[runner] failed to notify completion: %s\n", strerror(errno));
    }

    runner_print_stdout("[runner] command %d finished\n", command_id);
    return exit_code;
}

static int handle_list(void) {
    RequestMessage req;
    ResponseMessage resp;
    char reply_fifo[MAX_FIFO_PATH] = {0};
    int reply_fd = -1;
    ssize_t bytes;

    if (setup_reply_channel(reply_fifo, &reply_fd) < 0) {
        runner_print_stderr("[runner] failed to create list channel: %s\n", strerror(errno));
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.type = REQ_LIST;
    req.command_id = 0;
    req.runner_pid = getpid();
    copy_cstr(req.reply_fifo, sizeof(req.reply_fifo), reply_fifo);

    if (write_request_to_controller(&req, 1) < 0) {
        int saved_errno = errno;
        cleanup_reply_channel(reply_fifo, reply_fd);
        runner_print_stderr("[runner] failed to query controller: %s\n", strerror(saved_errno));
        return 1;
    }

    while (1) {
        bytes = read_full(reply_fd, &resp, sizeof(resp));
        if (bytes != (ssize_t)sizeof(resp)) {
            cleanup_reply_channel(reply_fifo, reply_fd);
            runner_print_stderr("[runner] failed to read list response\n");
            return 1;
        }

        if (resp.type != RESP_LIST) {
            cleanup_reply_channel(reply_fifo, reply_fd);
            runner_print_stderr("[runner] unexpected response from controller\n");
            return 1;
        }

        if (resp.payload[0] != '\0') {
            write_full(STDOUT_FILENO, resp.payload, strlen(resp.payload));
        }

        if (resp.status == 1) {
            break;
        }
    }

    cleanup_reply_channel(reply_fifo, reply_fd);
    return 0;
}

static int handle_shutdown(void) {
    RequestMessage req;
    ResponseMessage resp;
    char reply_fifo[MAX_FIFO_PATH] = {0};
    int reply_fd = -1;
    ssize_t bytes;

    memset(&req, 0, sizeof(req));
    req.type = REQ_SHUTDOWN;
    req.runner_pid = getpid();

    if (setup_reply_channel(reply_fifo, &reply_fd) < 0) {
        runner_print_stderr("[runner] failed to create shutdown channel: %s\n", strerror(errno));
        return 1;
    }

    copy_cstr(req.reply_fifo, sizeof(req.reply_fifo), reply_fifo);

    runner_print_stdout("[runner] sent shutdown notification\n");

    if (write_request_to_controller(&req, 1) < 0) {
        int saved_errno = errno;
        cleanup_reply_channel(reply_fifo, reply_fd);
        runner_print_stderr("[runner] failed to notify controller: %s\n", strerror(saved_errno));
        return 1;
    }

    runner_print_stdout("[runner] waiting for controller to shutdown...\n");

    bytes = read_full(reply_fd, &resp, sizeof(resp));
    cleanup_reply_channel(reply_fifo, reply_fd);

    if (bytes != (ssize_t)sizeof(resp) || resp.type != RESP_SHUTDOWN_DONE) {
        runner_print_stderr("[runner] controller shutdown failed\n");
        return 1;
    }

    runner_print_stdout("[runner] controller exited.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        runner_print_stderr("usage: ./runner -e|-c|-s ...\n");
        return 1;
    }

    if (strcmp(argv[1], "-e") == 0) {
        return handle_execute(argc, argv);
    }

    if (strcmp(argv[1], "-c") == 0) {
        return handle_list();
    }

    if (strcmp(argv[1], "-s") == 0) {
        return handle_shutdown();
    }

    runner_print_stderr("usage: ./runner -e|-c|-s ...\n");
    return 1;
}
