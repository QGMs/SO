#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/time.h>
#include <sys/types.h>

#define TP_LOG_DIR "tmp"
#define TP_LOG_FILE TP_LOG_DIR "/controller.log"

#define MAX_USER_ID 64
#define MAX_FIFO_PATH 256
#define MAX_COMMAND_LINE 512
#define MAX_RESPONSE_PAYLOAD 3072

typedef enum {
    REQ_EXEC = 1,
    REQ_FINISH = 2,
    REQ_LIST = 3,
    REQ_SHUTDOWN = 4
} RequestType;

typedef enum {
    RESP_ACK_EXEC = 1,
    RESP_REJECT = 2,
    RESP_LIST = 3,
    RESP_SHUTDOWN_DONE = 4
} ResponseType;

typedef struct {
    int type;
    int command_id;
    pid_t runner_pid;
    int exit_status;
    char user_id[MAX_USER_ID];
    char reply_fifo[MAX_FIFO_PATH];
    char command_line[MAX_COMMAND_LINE];
} RequestMessage;

typedef struct {
    int type;
    int command_id;
    int status;
    char payload[MAX_RESPONSE_PAYLOAD];
} ResponseMessage;

ssize_t write_full(int fd, const void *buf, size_t count);
ssize_t read_full(int fd, void *buf, size_t count);
int ensure_directory(const char *path, mode_t mode);
long timeval_diff_ms(struct timeval start, struct timeval end);
int safe_snprintf(char *dst, size_t dst_size, const char *fmt, ...);
int build_command_line(char *out, size_t out_size, char *const argv[], int start_idx, int argc);
const char *tp_runtime_dir(void);
const char *tp_controller_fifo(void);
const char *tp_log_path(void);
int send_response_to_fifo(const char *fifo_path, const ResponseMessage *resp);
int write_request_to_controller(const RequestMessage *req, int nonblock);
int generate_command_id(pid_t pid);

#endif
