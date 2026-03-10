# Makefile — Build demo_app_ei for the Arduino UNO Q Linux co-processor.
#
# demo_app_ei integrates:
#   TinyMLDelta C library — tmd_apply_patch_from_memory() for OTA model updates
#   Edge Impulse SDK      — run_classifier() with external model loading
#
# The EI SDK bundles its own TFLite Micro — no libtensorflowlite_c.so needed.
#
# Build (on board):
#   make EI_SDK=./edge-impulse-sdk-pack
#
# The EI_SDK path must point to the edge-impulse-sdk-pack directory that
# contains edgeimpulse/edge-impulse-sdk/ (with external model loading patch).
#
# Cross-compile:  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
#                       EI_SDK=./edge-impulse-sdk-pack
# Clean:          make clean

RUNTIME     := ../../../runtime
COMMON      := ../common
CXX         ?= g++
CC          ?= gcc

# EI_SDK is required.
ifndef EI_SDK
$(error EI_SDK is not set. Usage: make EI_SDK=/path/to/edge-impulse-sdk-pack)
endif

EI_SDK_ROOT := $(EI_SDK)/edgeimpulse/edge-impulse-sdk

# ── Include paths ────────────────────────────────────────────────────────────

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-sign-compare -Wno-unused-variable \
            -Wno-maybe-uninitialized -Wno-uninitialized \
            -I$(RUNTIME)/include \
            -I$(COMMON) \
            -I. \
            -I$(EI_SDK)/edgeimpulse \
            -I$(EI_SDK_ROOT)/CMSIS/DSP/Include \
            -I$(EI_SDK_ROOT)/CMSIS/Core/Include \
            -I$(EI_SDK_ROOT)/CMSIS/NN/Include \
            -DEI_CLASSIFIER_EXTERNAL_MODEL_LOADING \
            -DEI_PORTING_POSIX=1 \
            -DEIDSP_USE_CMSIS_DSP=0 \
            -DEI_C_LINKAGE=1 \
            -D__LINUX_ERRNO_EXTENSIONS__

CFLAGS   := -std=c11 -O2 -Wall \
            -I$(RUNTIME)/include

LDFLAGS  := -lpthread -lm -lstdc++

# ── Source files ─────────────────────────────────────────────────────────────

# App + TinyMLDelta
APP_SRC   := demo_app_ei.cpp
CORE_SRC  := $(RUNTIME)/src/tinymldelta_core.c

# Compiled model from EI export
MODEL_SRC := $(wildcard tflite-model/*.cpp)

# EI SDK TFLite Micro sources
TFLM_SRC := $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/micro/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/micro/kernels/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/micro/memory_planner/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/micro/arena_allocator/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/core/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/core/c/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/core/api/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/kernels/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/kernels/internal/*.cc) \
            $(wildcard $(EI_SDK_ROOT)/tensorflow/lite/schema/*.cc)

# EI SDK DSP sources
DSP_SRC := $(wildcard $(EI_SDK_ROOT)/dsp/kissfft/*.cpp) \
           $(wildcard $(EI_SDK_ROOT)/dsp/dct/*.cpp)

# EI SDK porting layer
PORTING_SRC := $(wildcard $(EI_SDK_ROOT)/porting/posix/*.c) \
               $(wildcard $(EI_SDK_ROOT)/porting/posix/*.cpp)

# Collect all objects
APP_OBJ   := $(APP_SRC:.cpp=.o)
CORE_OBJ  := $(CORE_SRC:.c=.o)
MODEL_OBJ := $(MODEL_SRC:.cpp=.o)
TFLM_OBJ  := $(TFLM_SRC:.cc=.o)
DSP_OBJ   := $(filter %.o, $(DSP_SRC:.cpp=.o) $(DSP_SRC:.c=.o))
PORT_OBJ  := $(filter %.o, $(PORTING_SRC:.c=.o) $(PORTING_SRC:.cpp=.o))

ALL_OBJ   := $(APP_OBJ) $(CORE_OBJ) $(MODEL_OBJ) $(TFLM_OBJ) $(DSP_OBJ) $(PORT_OBJ)
TARGET    := demo_app_ei

# ── Rules ────────────────────────────────────────────────────────────────────

$(TARGET): $(ALL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(APP_OBJ) $(CORE_OBJ) $(MODEL_OBJ)
	find $(EI_SDK_ROOT) -name "*.o" -delete 2>/dev/null; true
	find tflite-model -name "*.o" -delete 2>/dev/null; true

.PHONY: clean
