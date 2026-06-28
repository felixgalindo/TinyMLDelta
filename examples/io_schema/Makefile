# I/O schema reference example — License: Apache-2.0
CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
BIN      = schema_demo

$(BIN): main.c schema.c schema.h
	$(CC) $(CFLAGS) -o $(BIN) main.c schema.c

.PHONY: run clean test
run: $(BIN)
	$(RUNNER) ./$(BIN)

# Same as run; non-zero exit on any unexpected decision (CI-friendly).
test: run

clean:
	rm -f $(BIN)
