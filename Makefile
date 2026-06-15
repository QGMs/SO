CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -Iinclude
LDFLAGS =

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
TMP_DIR = tmp
SCRIPT_DIR = scripts

COMMON_OBJ = $(OBJ_DIR)/common.o
CONTROLLER_OBJ = $(OBJ_DIR)/controller.o $(COMMON_OBJ)
RUNNER_OBJ = $(OBJ_DIR)/runner.o $(OBJ_DIR)/runner_exec.o $(COMMON_OBJ)

all: folders controller runner

controller: $(BIN_DIR)/controller

runner: $(BIN_DIR)/runner

folders:
	@mkdir -p $(SRC_DIR) include $(OBJ_DIR) $(BIN_DIR) $(TMP_DIR) $(SCRIPT_DIR)

$(BIN_DIR)/controller: $(CONTROLLER_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN_DIR)/runner: $(RUNNER_OBJ)
	$(CC) $(LDFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_DIR)/*.o $(BIN_DIR)/controller $(BIN_DIR)/runner $(TMP_DIR)/*
