# TinyMLDelta – posix-only Makefile (local to this folder)
# Builds: demo_apply using POSIX/macOS flash backend

CC      := clang
CFLAGS  := -Wall -Wextra -Werror -std=c11 -O2
INCLUDES:= -I../../runtime/include -I.

CORE_SRCS := \
    ../../runtime/src/tinymldelta_core.c

PORT_SRCS := \
    tinymldelta_ports_posix.c

DEMO_SRCS := \
    demo_apply.c

OBJS := $(CORE_SRCS:.c=.o) $(PORT_SRCS:.c=.o) $(DEMO_SRCS:.c=.o)

TARGET := demo_apply

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

../../runtime/src/%.o: ../../runtime/src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
