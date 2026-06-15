#include "runner_exec.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TOKENS 256
#define MAX_STAGES 32
#define MAX_ARGS_PER_STAGE 128
#define MAX_WORD_LEN 1024

typedef enum {
    TOK_WORD = 1,
    TOK_PIPE = 2,
    TOK_REDIR_IN = 3,
    TOK_REDIR_OUT = 4,
    TOK_REDIR_ERR = 5
} TokenType;

typedef struct {
    int type;
    char *text;
} Token;

typedef struct {
    char *argv[MAX_ARGS_PER_STAGE + 1];
    int argc;
    char *in_file;
    char *out_file;
    char *err_file;
} Stage;

typedef struct {
    Stage stages[MAX_STAGES];
    int count;
} ParsedCommand;

static int is_operator_start(const char *p) {
    return (*p == '|' || *p == '<' || *p == '>' || (*p == '2' && *(p + 1) == '>'));
}

static int add_token(Token tokens[], int *count, int type, const char *text) {
    if (*count >= MAX_TOKENS) {
        return -1;
    }

    tokens[*count].type = type;
    tokens[*count].text = NULL;

    if (text != NULL) {
        tokens[*count].text = strdup(text);
        if (tokens[*count].text == NULL) {
            return -1;
        }
    }

    (*count)++;
    return 0;
}

static int read_word(const char **cursor, char out[MAX_WORD_LEN]) {
    const char *p = *cursor;
    size_t idx = 0;

    while (*p != '\0' && !isspace((unsigned char)*p) && !is_operator_start(p)) {
        if (*p == '\\' && *(p + 1) != '\0') {
            p++;
            if (idx + 1 >= MAX_WORD_LEN) {
                return -1;
            }
            out[idx++] = *p;
            p++;
            continue;
        }

        if (*p == '\'' || *p == '"') {
            char quote = *p;
            p++;
            while (*p != '\0' && *p != quote) {
                if (*p == '\\' && quote == '"' && *(p + 1) != '\0') {
                    p++;
                }
                if (idx + 1 >= MAX_WORD_LEN) {
                    return -1;
                }
                out[idx++] = *p;
                p++;
            }
            if (*p == quote) {
                p++;
            }
            continue;
        }

        if (idx + 1 >= MAX_WORD_LEN) {
            return -1;
        }
        out[idx++] = *p;
        p++;
    }

    out[idx] = '\0';
    *cursor = p;
    return (idx == 0) ? -1 : 0;
}

static int tokenize_command(const char *line, Token tokens[], int *token_count) {
    const char *p = line;
    char word[MAX_WORD_LEN];
    int count = 0;

    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (*p == '|') {
            if (add_token(tokens, &count, TOK_PIPE, NULL) < 0) {
                return -1;
            }
            p++;
            continue;
        }

        if (*p == '<') {
            if (add_token(tokens, &count, TOK_REDIR_IN, NULL) < 0) {
                return -1;
            }
            p++;
            continue;
        }

        if (*p == '2' && *(p + 1) == '>') {
            if (add_token(tokens, &count, TOK_REDIR_ERR, NULL) < 0) {
                return -1;
            }
            p += 2;
            continue;
        }

        if (*p == '>') {
            if (add_token(tokens, &count, TOK_REDIR_OUT, NULL) < 0) {
                return -1;
            }
            p++;
            continue;
        }

        if (read_word(&p, word) < 0) {
            return -1;
        }

        if (add_token(tokens, &count, TOK_WORD, word) < 0) {
            return -1;
        }
    }

    *token_count = count;
    return 0;
}

static void free_tokens(Token tokens[], int token_count) {
    for (int i = 0; i < token_count; ++i) {
        free(tokens[i].text);
        tokens[i].text = NULL;
    }
}

static int parse_tokens(Token tokens[], int token_count, ParsedCommand *cmd) {
    int stage_idx = 0;

    memset(cmd, 0, sizeof(*cmd));
    cmd->count = 1;

    for (int i = 0; i < token_count; ++i) {
        Stage *stage = &cmd->stages[stage_idx];
        Token *tok = &tokens[i];

        if (tok->type == TOK_WORD) {
            if (stage->argc >= MAX_ARGS_PER_STAGE) {
                return -1;
            }
            stage->argv[stage->argc++] = tok->text;
            stage->argv[stage->argc] = NULL;
            continue;
        }

        if (tok->type == TOK_PIPE) {
            if (stage->argc == 0) {
                return -1;
            }
            stage_idx++;
            if (stage_idx >= MAX_STAGES) {
                return -1;
            }
            cmd->count = stage_idx + 1;
            continue;
        }

        if (tok->type == TOK_REDIR_IN || tok->type == TOK_REDIR_OUT || tok->type == TOK_REDIR_ERR) {
            if (i + 1 >= token_count || tokens[i + 1].type != TOK_WORD) {
                return -1;
            }

            if (tok->type == TOK_REDIR_IN) {
                stage->in_file = tokens[i + 1].text;
            } else if (tok->type == TOK_REDIR_OUT) {
                stage->out_file = tokens[i + 1].text;
            } else {
                stage->err_file = tokens[i + 1].text;
            }
            i++;
            continue;
        }

        return -1;
    }

    for (int i = 0; i < cmd->count; ++i) {
        if (cmd->stages[i].argc == 0) {
            return -1;
        }
    }

    return 0;
}

static int apply_redirections(const Stage *stage) {
    int fd;

    if (stage->in_file != NULL) {
        fd = open(stage->in_file, O_RDONLY);
        if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) {
            if (fd >= 0) {
                close(fd);
            }
            return -1;
        }
        close(fd);
    }

    if (stage->out_file != NULL) {
        fd = open(stage->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
            if (fd >= 0) {
                close(fd);
            }
            return -1;
        }
        close(fd);
    }

    if (stage->err_file != NULL) {
        fd = open(stage->err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || dup2(fd, STDERR_FILENO) < 0) {
            if (fd >= 0) {
                close(fd);
            }
            return -1;
        }
        close(fd);
    }

    return 0;
}

static int execute_parsed_command(const ParsedCommand *cmd) {
    int pipes[MAX_STAGES - 1][2];
    pid_t pids[MAX_STAGES];
    int stage_count = cmd->count;
    int last_status = 0;

    for (int i = 0; i < stage_count - 1; ++i) {
        if (pipe(pipes[i]) < 0) {
            return -1;
        }
    }

    for (int i = 0; i < stage_count; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            for (int j = 0; j < stage_count - 1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            return -1;
        }

        if (pid == 0) {
            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                _exit(126);
            }
            if (i < stage_count - 1 && dup2(pipes[i][1], STDOUT_FILENO) < 0) {
                _exit(126);
            }

            for (int j = 0; j < stage_count - 1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (apply_redirections(&cmd->stages[i]) < 0) {
                dprintf(STDERR_FILENO, "[runner] redirection failed\n");
                _exit(126);
            }

            execvp(cmd->stages[i].argv[0], cmd->stages[i].argv);
            dprintf(STDERR_FILENO, "[runner] exec failed for '%s': %s\n",
                    cmd->stages[i].argv[0], strerror(errno));
            _exit(127);
        }

        pids[i] = pid;
    }

    for (int i = 0; i < stage_count - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (int i = 0; i < stage_count; ++i) {
        int status = 0;
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = 1 << 8;
            break;
        }
        if (i == stage_count - 1) {
            last_status = status;
        }
    }

    if (WIFEXITED(last_status)) {
        return WEXITSTATUS(last_status);
    }

    if (WIFSIGNALED(last_status)) {
        return 128 + WTERMSIG(last_status);
    }

    return 1;
}

int execute_user_command(const char *command_line) {
    Token tokens[MAX_TOKENS];
    ParsedCommand parsed;
    int token_count = 0;
    int rc;

    memset(tokens, 0, sizeof(tokens));

    if (command_line == NULL || *command_line == '\0') {
        return 1;
    }

    if (tokenize_command(command_line, tokens, &token_count) < 0 || token_count == 0) {
        free_tokens(tokens, token_count);
        dprintf(STDERR_FILENO, "[runner] invalid command syntax\n");
        return 1;
    }

    if (parse_tokens(tokens, token_count, &parsed) < 0) {
        free_tokens(tokens, token_count);
        dprintf(STDERR_FILENO, "[runner] invalid command syntax\n");
        return 1;
    }

    rc = execute_parsed_command(&parsed);

    free_tokens(tokens, token_count);

    if (rc < 0) {
        return 1;
    }
    return rc;
}
