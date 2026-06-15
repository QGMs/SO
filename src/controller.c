#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    POLICY_FIFO = 1,
    POLICY_RR = 2
} SchedulingPolicy;

typedef struct CommandEntry {
    int command_id;
    pid_t runner_pid;
    int exit_status;
    char user_id[MAX_USER_ID];
    char command_line[MAX_COMMAND_LINE];
    char reply_fifo[MAX_FIFO_PATH];
    struct timeval submitted_at;
    struct timeval dispatched_at;
    struct CommandEntry *next;
} CommandEntry;

typedef struct ShutdownWatcher {
    char reply_fifo[MAX_FIFO_PATH];
    struct ShutdownWatcher *next;
} ShutdownWatcher;

typedef struct {
    int parallel_limit;
    SchedulingPolicy policy;
    const char *policy_name;
    int request_fd;
    int keepalive_fd;
    int log_fd;
    int shutdown_requested;
    char rr_last_user[MAX_USER_ID];
    CommandEntry *scheduled_head;
    CommandEntry *scheduled_tail;
    CommandEntry *running_head;
    int scheduled_count;
    int running_count;
    ShutdownWatcher *watchers;
} ControllerState;

static void write_log_entry(ControllerState *state, const CommandEntry *entry, struct timeval finished_at);

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

static int write_response_fd(int fd, const ResponseMessage *resp) {
    ssize_t written = write_full(fd, resp, sizeof(*resp));
    return written == (ssize_t)sizeof(*resp) ? 0 : -1;
}

static void free_command_list(CommandEntry *head) {
    while (head != NULL) {
        CommandEntry *next = head->next;
        free(head);
        head = next;
    }
}

static void free_watchers(ShutdownWatcher *head) {
    while (head != NULL) {
        ShutdownWatcher *next = head->next;
        free(head);
        head = next;
    }
}

static void enqueue_scheduled(ControllerState *state, CommandEntry *entry) {
    entry->next = NULL;

    if (state->scheduled_tail == NULL) {
        state->scheduled_head = entry;
        state->scheduled_tail = entry;
    } else {
        state->scheduled_tail->next = entry;
        state->scheduled_tail = entry;
    }

    state->scheduled_count++;
}

static void add_running(ControllerState *state, CommandEntry *entry) {
    entry->next = state->running_head;
    state->running_head = entry;
    state->running_count++;
}

static CommandEntry *pop_scheduled_head(ControllerState *state) {
    CommandEntry *entry = state->scheduled_head;

    if (entry == NULL) {
        return NULL;
    }

    state->scheduled_head = entry->next;
    if (state->scheduled_head == NULL) {
        state->scheduled_tail = NULL;
    }

    entry->next = NULL;
    state->scheduled_count--;
    return entry;
}

static CommandEntry *dequeue_next_scheduled(ControllerState *state) {
    if (state->scheduled_head == NULL) {
        return NULL;
    }

    if (state->policy == POLICY_FIFO || state->rr_last_user[0] == '\0') {
        CommandEntry *entry = pop_scheduled_head(state);
        if (entry != NULL && state->policy == POLICY_RR) {
            copy_cstr(state->rr_last_user, sizeof(state->rr_last_user), entry->user_id);
        }
        return entry;
    }

    CommandEntry *prev = NULL;
    CommandEntry *cur = state->scheduled_head;

    while (cur != NULL) {
        if (strncmp(cur->user_id, state->rr_last_user, MAX_USER_ID) != 0) {
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    if (cur == NULL) {
        cur = state->scheduled_head;
        state->scheduled_head = cur->next;
        if (state->scheduled_head == NULL) {
            state->scheduled_tail = NULL;
        }
    } else if (prev == NULL) {
        state->scheduled_head = cur->next;
        if (state->scheduled_head == NULL) {
            state->scheduled_tail = NULL;
        }
    } else {
        prev->next = cur->next;
        if (state->scheduled_tail == cur) {
            state->scheduled_tail = prev;
        }
    }

    cur->next = NULL;
    state->scheduled_count--;

    copy_cstr(state->rr_last_user, sizeof(state->rr_last_user), cur->user_id);

    return cur;
}

static CommandEntry *remove_running(ControllerState *state, int command_id, const char *user_id, pid_t runner_pid) {
    CommandEntry *prev = NULL;
    CommandEntry *cur = state->running_head;

    while (cur != NULL) {
        if (cur->command_id == command_id &&
            cur->runner_pid == runner_pid &&
            strncmp(cur->user_id, user_id, MAX_USER_ID) == 0) {
            if (prev == NULL) {
                state->running_head = cur->next;
            } else {
                prev->next = cur->next;
            }
            cur->next = NULL;
            state->running_count--;
            return cur;
        }
        prev = cur;
        cur = cur->next;
    }

    return NULL;
}

static int command_id_in_use(const ControllerState *state, int command_id) {
    CommandEntry *cur;

    cur = state->running_head;
    while (cur != NULL) {
        if (cur->command_id == command_id) {
            return 1;
        }
        cur = cur->next;
    }

    cur = state->scheduled_head;
    while (cur != NULL) {
        if (cur->command_id == command_id) {
            return 1;
        }
        cur = cur->next;
    }

    return 0;
}

static int runner_is_alive(pid_t runner_pid) {
    if (runner_pid <= 0) {
        return 0;
    }

    if (kill(runner_pid, 0) == 0) {
        return 1;
    }

    if (errno == EPERM) {
        return 1;
    }

    return 0;
}

static void purge_stale_scheduled(ControllerState *state) {
    CommandEntry *prev = NULL;
    CommandEntry *cur = state->scheduled_head;

    while (cur != NULL) {
        CommandEntry *next = cur->next;

        if (runner_is_alive(cur->runner_pid)) {
            prev = cur;
            cur = next;
            continue;
        }

        if (prev == NULL) {
            state->scheduled_head = next;
        } else {
            prev->next = next;
        }

        if (state->scheduled_tail == cur) {
            state->scheduled_tail = prev;
        }

        state->scheduled_count--;
        free(cur);
        cur = next;
    }
}

static void purge_stale_running(ControllerState *state) {
    CommandEntry *prev = NULL;
    CommandEntry *cur = state->running_head;

    while (cur != NULL) {
        CommandEntry *next = cur->next;

        if (runner_is_alive(cur->runner_pid)) {
            prev = cur;
            cur = next;
            continue;
        }

        if (prev == NULL) {
            state->running_head = next;
        } else {
            prev->next = next;
        }

        state->running_count--;
        cur->next = NULL;
        cur->exit_status = 255;

        struct timeval finished_at;
        gettimeofday(&finished_at, NULL);
        write_log_entry(state, cur, finished_at);

        free(cur);
        cur = next;
    }
}

static void add_shutdown_watcher(ControllerState *state, const char *reply_fifo) {
    ShutdownWatcher *cur = state->watchers;

    while (cur != NULL) {
        if (strncmp(cur->reply_fifo, reply_fifo, MAX_FIFO_PATH) == 0) {
            return;
        }
        cur = cur->next;
    }

    ShutdownWatcher *watcher = (ShutdownWatcher *)calloc(1, sizeof(*watcher));
    if (watcher == NULL) {
        return;
    }

    copy_cstr(watcher->reply_fifo, sizeof(watcher->reply_fifo), reply_fifo);
    watcher->next = state->watchers;
    state->watchers = watcher;
}

static void write_log_entry(ControllerState *state, const CommandEntry *entry, struct timeval finished_at) {
    char line[2048];
    long total_ms;
    long queue_ms;
    long run_ms;

    if (state->log_fd < 0 || entry == NULL) {
        return;
    }

    total_ms = timeval_diff_ms(entry->submitted_at, finished_at);
    queue_ms = timeval_diff_ms(entry->submitted_at, entry->dispatched_at);
    run_ms = timeval_diff_ms(entry->dispatched_at, finished_at);

    if (safe_snprintf(
            line,
            sizeof(line),
            "user=%s command=%d submit=%ld.%06ld dispatch=%ld.%06ld finish=%ld.%06ld wait_ms=%ld run_ms=%ld total_ms=%ld exit=%d policy=%s parallel=%d cmd=\"%s\"\n",
            entry->user_id,
            entry->command_id,
            (long)entry->submitted_at.tv_sec,
            (long)entry->submitted_at.tv_usec,
            (long)entry->dispatched_at.tv_sec,
            (long)entry->dispatched_at.tv_usec,
            (long)finished_at.tv_sec,
            (long)finished_at.tv_usec,
            queue_ms,
            run_ms,
            total_ms,
            entry->exit_status,
            state->policy_name,
            state->parallel_limit,
            entry->command_line) < 0) {
        return;
    }

    write_full(state->log_fd, line, strlen(line));
}

static int send_list_chunk(int fd, const char *text, int done) {
    ResponseMessage resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = RESP_LIST;
    resp.status = done ? 1 : 0;

    if (text != NULL && *text != '\0') {
        copy_cstr(resp.payload, sizeof(resp.payload), text);
    }

    return write_response_fd(fd, &resp);
}

static void send_scheduled_in_dispatch_order(const ControllerState *state, int fd) {
    int n = state->scheduled_count;
    CommandEntry **arr;
    int *used;
    char last_user[MAX_USER_ID] = {0};
    char line[256];
    int i;

    if (n <= 0) {
        return;
    }

    arr = (CommandEntry **)calloc((size_t)n, sizeof(*arr));
    used = (int *)calloc((size_t)n, sizeof(*used));
    if (arr == NULL || used == NULL) {
        free(arr);
        free(used);
        return;
    }

    CommandEntry *cur = state->scheduled_head;
    i = 0;
    while (cur != NULL && i < n) {
        arr[i++] = cur;
        cur = cur->next;
    }
    n = i;

    copy_cstr(last_user, sizeof(last_user), state->rr_last_user);

    for (int step = 0; step < n; ++step) {
        int idx = -1;

        if (state->policy == POLICY_FIFO || last_user[0] == '\0') {
            for (i = 0; i < n; ++i) {
                if (!used[i]) {
                    idx = i;
                    break;
                }
            }
        } else {
            for (i = 0; i < n; ++i) {
                if (!used[i] && strncmp(arr[i]->user_id, last_user, MAX_USER_ID) != 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                for (i = 0; i < n; ++i) {
                    if (!used[i]) {
                        idx = i;
                        break;
                    }
                }
            }
        }

        if (idx < 0) {
            break;
        }

        used[idx] = 1;
        if (safe_snprintf(line, sizeof(line), "user-id %s - command-id %d\n",
                          arr[idx]->user_id, arr[idx]->command_id) >= 0) {
            if (send_list_chunk(fd, line, 0) < 0) {
                break;
            }
        }

        copy_cstr(last_user, sizeof(last_user), arr[idx]->user_id);
    }

    free(arr);
    free(used);
}

static void send_list_response(const ControllerState *state, const char *reply_fifo) {
    int fd = open(reply_fifo, O_WRONLY);
    if (fd < 0) {
        return;
    }

    if (send_list_chunk(fd, "---\n", 0) < 0 ||
        send_list_chunk(fd, "Executing\n", 0) < 0) {
        close(fd);
        return;
    }

    CommandEntry *cur = state->running_head;
    while (cur != NULL) {
        char line[256];
        if (safe_snprintf(line, sizeof(line), "user-id %s - command-id %d\n",
                          cur->user_id, cur->command_id) >= 0) {
            if (send_list_chunk(fd, line, 0) < 0) {
                close(fd);
                return;
            }
        }
        cur = cur->next;
    }

    if (send_list_chunk(fd, "---\n", 0) < 0 ||
        send_list_chunk(fd, "Scheduled\n", 0) < 0) {
        close(fd);
        return;
    }

    if (state->policy == POLICY_FIFO) {
        cur = state->scheduled_head;
        while (cur != NULL) {
            char line[256];
            if (safe_snprintf(line, sizeof(line), "user-id %s - command-id %d\n",
                              cur->user_id, cur->command_id) >= 0) {
                if (send_list_chunk(fd, line, 0) < 0) {
                    close(fd);
                    return;
                }
            }
            cur = cur->next;
        }
    } else {
        send_scheduled_in_dispatch_order(state, fd);
    }

    (void)send_list_chunk(fd, "", 1);
    close(fd);
}

static void dispatch_ready_commands(ControllerState *state) {
    while (state->running_count < state->parallel_limit) {
        CommandEntry *entry = dequeue_next_scheduled(state);
        ResponseMessage resp;

        if (entry == NULL) {
            break;
        }

        memset(&resp, 0, sizeof(resp));
        resp.type = RESP_ACK_EXEC;
        resp.command_id = entry->command_id;
        resp.status = 1;

        if (send_response_to_fifo(entry->reply_fifo, &resp) < 0) {
            free(entry);
            continue;
        }

        gettimeofday(&entry->dispatched_at, NULL);
        add_running(state, entry);
    }
}

static void send_shutdown_notifications(ControllerState *state) {
    ShutdownWatcher *cur = state->watchers;
    ResponseMessage resp;

    memset(&resp, 0, sizeof(resp));
    resp.type = RESP_SHUTDOWN_DONE;

    while (cur != NULL) {
        send_response_to_fifo(cur->reply_fifo, &resp);
        cur = cur->next;
    }
}

static void handle_exec_request(ControllerState *state, const RequestMessage *req) {
    CommandEntry *entry;

    if (state->shutdown_requested) {
        ResponseMessage resp;
        memset(&resp, 0, sizeof(resp));
        resp.type = RESP_REJECT;
        resp.command_id = req->command_id;
        copy_cstr(resp.payload, sizeof(resp.payload), "[runner] controller is shutting down");
        send_response_to_fifo(req->reply_fifo, &resp);
        return;
    }

    if (command_id_in_use(state, req->command_id)) {
        ResponseMessage resp;
        memset(&resp, 0, sizeof(resp));
        resp.type = RESP_REJECT;
        resp.command_id = req->command_id;
        copy_cstr(resp.payload, sizeof(resp.payload), "[runner] duplicate command-id, retry");
        send_response_to_fifo(req->reply_fifo, &resp);
        return;
    }

    entry = (CommandEntry *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        ResponseMessage resp;
        memset(&resp, 0, sizeof(resp));
        resp.type = RESP_REJECT;
        resp.command_id = req->command_id;
        copy_cstr(resp.payload, sizeof(resp.payload), "[runner] controller out of memory");
        send_response_to_fifo(req->reply_fifo, &resp);
        return;
    }

    entry->command_id = req->command_id;
    entry->runner_pid = req->runner_pid;
    copy_cstr(entry->user_id, sizeof(entry->user_id), req->user_id);
    copy_cstr(entry->command_line, sizeof(entry->command_line), req->command_line);
    copy_cstr(entry->reply_fifo, sizeof(entry->reply_fifo), req->reply_fifo);
    gettimeofday(&entry->submitted_at, NULL);

    enqueue_scheduled(state, entry);
}

static void handle_finish_request(ControllerState *state, const RequestMessage *req) {
    CommandEntry *entry;
    struct timeval finished_at;

    entry = remove_running(state, req->command_id, req->user_id, req->runner_pid);
    if (entry == NULL) {
        return;
    }

    entry->exit_status = req->exit_status;
    gettimeofday(&finished_at, NULL);
    write_log_entry(state, entry, finished_at);
    free(entry);
}

static void handle_list_request(const ControllerState *state, const RequestMessage *req) {
    pid_t pid = fork();

    if (pid == 0) {
        send_list_response(state, req->reply_fifo);
        _exit(0);
    }

    if (pid < 0) {
        send_list_response(state, req->reply_fifo);
    }
}

static void handle_shutdown_request(ControllerState *state, const RequestMessage *req) {
    state->shutdown_requested = 1;
    add_shutdown_watcher(state, req->reply_fifo);
}

static void handle_request(ControllerState *state, const RequestMessage *req) {
    switch (req->type) {
        case REQ_EXEC:
            handle_exec_request(state, req);
            break;
        case REQ_FINISH:
            handle_finish_request(state, req);
            break;
        case REQ_LIST:
            handle_list_request(state, req);
            break;
        case REQ_SHUTDOWN:
            handle_shutdown_request(state, req);
            break;
        default:
            break;
    }
}

static void reap_finished_children(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static int should_exit(const ControllerState *state) {
    return state->shutdown_requested && state->running_count == 0 && state->scheduled_head == NULL;
}

static SchedulingPolicy parse_policy(const char *text) {
    if (strcmp(text, "fifo") == 0) {
        return POLICY_FIFO;
    }
    if (strcmp(text, "rr") == 0 || strcmp(text, "round-robin") == 0 || strcmp(text, "round_robin") == 0) {
        return POLICY_RR;
    }
    return 0;
}

static void cleanup_state(ControllerState *state) {
    if (state->request_fd >= 0) {
        close(state->request_fd);
    }
    if (state->keepalive_fd >= 0) {
        close(state->keepalive_fd);
    }
    if (state->log_fd >= 0) {
        close(state->log_fd);
    }

    unlink(tp_controller_fifo());

    free_command_list(state->scheduled_head);
    free_command_list(state->running_head);
    free_watchers(state->watchers);
}

int main(int argc, char *argv[]) {
    ControllerState state;

    memset(&state, 0, sizeof(state));
    state.request_fd = -1;
    state.keepalive_fd = -1;
    state.log_fd = -1;

    if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: ./controller <parallel-commands> <sched-policy>\n");
        return 1;
    }

    state.parallel_limit = atoi(argv[1]);
    if (state.parallel_limit <= 0) {
        dprintf(STDERR_FILENO, "[controller] invalid parallel-commands value\n");
        return 1;
    }

    state.policy = parse_policy(argv[2]);
    if (state.policy == 0) {
        dprintf(STDERR_FILENO, "[controller] invalid policy. use fifo or rr\n");
        return 1;
    }

    state.policy_name = (state.policy == POLICY_FIFO) ? "fifo" : "rr";

    signal(SIGPIPE, SIG_IGN);

    if (ensure_directory(tp_runtime_dir(), 0700) < 0) {
        dprintf(STDERR_FILENO, "[controller] failed to create runtime directory: %s\n", strerror(errno));
        return 1;
    }

    if (ensure_directory(TP_LOG_DIR, 0700) < 0) {
        dprintf(STDERR_FILENO, "[controller] failed to create log directory: %s\n", strerror(errno));
        return 1;
    }

    unlink(tp_controller_fifo());
    if (mkfifo(tp_controller_fifo(), 0660) < 0 && errno != EEXIST) {
        dprintf(STDERR_FILENO, "[controller] failed to create fifo: %s\n", strerror(errno));
        return 1;
    }

    state.request_fd = open(tp_controller_fifo(), O_RDWR);
    if (state.request_fd < 0) {
        dprintf(STDERR_FILENO, "[controller] failed to open request fifo: %s\n", strerror(errno));
        cleanup_state(&state);
        return 1;
    }

    state.keepalive_fd = -1;

    state.log_fd = open(tp_log_path(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (state.log_fd < 0) {
        dprintf(STDERR_FILENO, "[controller] failed to open log file: %s\n", strerror(errno));
        cleanup_state(&state);
        return 1;
    }

    while (1) {
        RequestMessage req;
        ssize_t bytes;

        purge_stale_scheduled(&state);
        purge_stale_running(&state);
        dispatch_ready_commands(&state);
        reap_finished_children();

        if (should_exit(&state)) {
            send_shutdown_notifications(&state);
            break;
        }

        bytes = read(state.request_fd, &req, sizeof(req));

        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            dprintf(STDERR_FILENO, "[controller] read failed: %s\n", strerror(errno));
            cleanup_state(&state);
            return 1;
        }

        if (bytes == 0) {
            continue;
        }

        if (bytes != (ssize_t)sizeof(req)) {
            continue;
        }

        handle_request(&state, &req);
        purge_stale_scheduled(&state);
        purge_stale_running(&state);
        dispatch_ready_commands(&state);
        reap_finished_children();

        if (should_exit(&state)) {
            send_shutdown_notifications(&state);
            cleanup_state(&state);
            return 0;
        }
    }

    cleanup_state(&state);
    return 0;
}
