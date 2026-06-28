# Capability-envelope reference example — License: Apache-2.0
CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
BIN      = envelope_demo

$(BIN): main.c envelope.c envelope.h
	$(CC) $(CFLAGS) -o $(BIN) main.c envelope.c

.PHONY: run clean test
run: $(BIN)
	./$(BIN)

# Same as run; non-zero exit on any unexpected decision (CI-friendly).
test: run

clean:
	rm -f $(BIN)
