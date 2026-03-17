CC := gcc
LD := gcc

CFLAGS := -O0        \
		  -g         \
		  -Wextra    \
		  -Werror    \
		  -Iinclude/ \
		  -DUSE_JOBCONTROL \
		  -DUSE_LINEEDITOR

BUILD_DIR := build
SRC_DIR := src

SRCS := $(shell find $(SRC_DIR) -type f -name "*.c" -o -name "*.s")
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
OBJS := $(OBJS:$(SRC_DIR)/%.s=$(BUILD_DIR)/%.o)

TARGET := $(BUILD_DIR)/aurum

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin

.PHONY: all clean clean-tests test

all: $(TARGET)
$(TARGET): $(OBJS)
	gcc $(OBJS) -o $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	mkdir -p $(BINDIR)
	cp $(TARGET) $(BINDIR)/$(notdir $(TARGET))
	chmod 755 $(BINDIR)/$(notdir $(TARGET))

clean:
	rm -rf $(TARGET)
	rm -rf $(BUILD_DIR)

clean-tests:
	rm -rf test/*.out
	rm -rf test/test.txt

test: install
	aurum test.sh