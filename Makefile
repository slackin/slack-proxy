CC       = gcc
CFLAGS   = -O2 -Wall -Wextra -Wno-unused-parameter -pedantic -std=c11
CPPFLAGS = -I include
LDFLAGS  =

SRC_DIR  = src
INC_DIR  = include
BUILD_DIR = build

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/relay.c \
       $(SRC_DIR)/hashmap.c \
       $(SRC_DIR)/q3proto.c \
       $(SRC_DIR)/log.c

OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
BIN  = $(BUILD_DIR)/urt-proxy

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
