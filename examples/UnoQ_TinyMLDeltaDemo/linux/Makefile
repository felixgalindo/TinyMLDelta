# Makefile — Build demo_app for the Arduino UNO Q Linux co-processor.
#
# demo_app directly integrates:
#   TinyMLDelta C library — tmd_apply_patch_from_memory() for OTA model updates
#   TFLite C API          — autoencoder inference (libtensorflowlite_c.so v2.17)
#
# Build on board:    make
# Cross-compile:     make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc
# Clean:             make clean

RUNTIME     := ../../../runtime
CXX         := g++
CC          := gcc

# TFLite C shared library location (classic TFLite interpreter API).
# demo_app.cpp forward-declares the C API, so no include path is needed.
TFLITE_LIB  ?= /home/arduino/tinymldelta

CXXFLAGS := -std=c++17 -O2 -Wall \
            -I$(RUNTIME)/include

CFLAGS   := -std=c11 -O2 -Wall \
            -I$(RUNTIME)/include

LDFLAGS  := -L$(TFLITE_LIB) -ltensorflowlite_c -lpthread \
            -Wl,-rpath,$(TFLITE_LIB)

TARGET   := demo_app
APP_OBJ  := demo_app.o
CORE_OBJ := $(RUNTIME)/src/tinymldelta_core.o

$(TARGET): $(APP_OBJ) $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

demo_app.o: demo_app.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(RUNTIME)/src/tinymldelta_core.o: $(RUNTIME)/src/tinymldelta_core.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(APP_OBJ) $(CORE_OBJ) $(TARGET)

.PHONY: clean
