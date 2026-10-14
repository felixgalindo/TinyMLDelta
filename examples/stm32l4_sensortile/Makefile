# TinyMLDelta on the SensorTile (STM32L476JG)
#
# Builds the unmodified TinyMLDelta core plus this port with arm-none-eabi-gcc,
# straight from ST's HAL sources. ST's own startup and system files are taken
# from the package as-is, so nothing from ST is copied into this repo except
# the HAL config header (inc/stm32l4xx_hal_conf.h, BSD-3-Clause).
#
#   make STM32CUBE=/path/to/STM32Cube_FW_L4     build build/tmd-stm32l4.bin
#   make flash                                  write it to 0x08000000
#   make size                                   section sizes
#   make clean
#
# STM32CUBE is any ST package that has Drivers/STM32L4xx_HAL_Driver and
# Drivers/CMSIS: STM32CubeL4, or a SensorTile function pack such as
# STM32CubeFunctionPack_SENSING1.
#
# Author: Harshdeep Parmar
# License: Apache-2.0

TARGET  := tmd-stm32l4
BUILD   := build

STM32CUBE ?=
ifeq ($(STM32CUBE),)
ifneq ($(MAKECMDGOALS),clean)
$(error set STM32CUBE=/path/to/an/STM32L4 Cube package)
endif
endif
TMD       ?= ../..

HAL     := $(STM32CUBE)/Drivers/STM32L4xx_HAL_Driver
CMSIS   := $(STM32CUBE)/Drivers/CMSIS
DEVICE  := $(CMSIS)/Device/ST/STM32L4xx

PREFIX  := arm-none-eabi-
CC      := $(PREFIX)gcc
AS      := $(PREFIX)gcc -x assembler-with-cpp
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

# ---------------------------------------------------------------- sources
C_SRC := \
  src/main.c \
  src/board.c \
  src/tmd_port_stm32l4.c \
  $(DEVICE)/Source/Templates/system_stm32l4xx.c \
  $(TMD)/runtime/src/tinymldelta_core.c \
  $(HAL)/Src/stm32l4xx_hal.c \
  $(HAL)/Src/stm32l4xx_hal_cortex.c \
  $(HAL)/Src/stm32l4xx_hal_flash.c \
  $(HAL)/Src/stm32l4xx_hal_flash_ex.c \
  $(HAL)/Src/stm32l4xx_hal_gpio.c \
  $(HAL)/Src/stm32l4xx_hal_pwr.c \
  $(HAL)/Src/stm32l4xx_hal_pwr_ex.c \
  $(HAL)/Src/stm32l4xx_hal_rcc.c \
  $(HAL)/Src/stm32l4xx_hal_rcc_ex.c \
  $(HAL)/Src/stm32l4xx_hal_dma.c

ASM_SRC := $(DEVICE)/Source/Templates/gcc/startup_stm32l476xx.s

INCLUDES := \
  -Iinc \
  -I$(TMD)/runtime/include \
  -I$(HAL)/Inc \
  -I$(DEVICE)/Include \
  -I$(CMSIS)/Include

# ---------------------------------------------------------------- flags
CPU   := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

SEMIHOSTING ?= 0

DEFS  := -DSTM32L476xx -DUSE_HAL_DRIVER \
         -DBOARD_SEMIHOSTING=$(SEMIHOSTING) \
         -DTMD_FEAT_LOG=1 \
         -DTMD_FEAT_JOURNAL=1 \
         -DTMD_FEAT_VERIFY_BASE=1

# -ffreestanding: no libc is linked in; board.c supplies mem*/strlen itself.
# -fno-builtin-* stops GCC from "optimising" those definitions into calls to
# themselves, which would recurse forever.
CFLAGS := $(CPU) $(DEFS) $(INCLUDES) -Os -g3 \
          -ffreestanding -fno-builtin-memcpy -fno-builtin-memset \
          -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-strlen \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -std=gnu11 -MMD -MP

ASFLAGS := $(CPU) -g3 -Wall

LDSCRIPT := stm32l476xg.ld
LDFLAGS  := $(CPU) -T$(LDSCRIPT) -nostdlib -nostartfiles \
            -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
            -Wl,--print-memory-usage -lgcc

# ---------------------------------------------------------------- objects
OBJS := $(addprefix $(BUILD)/,$(notdir $(C_SRC:.c=.o)))
vpath %.c $(sort $(dir $(C_SRC)))
OBJS += $(addprefix $(BUILD)/,$(notdir $(ASM_SRC:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SRC)))

.PHONY: all clean flash size monitor

all: $(BUILD)/$(TARGET).bin

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(AS) -c $(ASFLAGS) $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo "--- $@ ---"
	@ls -l $@

$(BUILD):
	mkdir -p $@

size: $(BUILD)/$(TARGET).elf
	$(SIZE) -A $<

# The SensorTile's factory firmware sleeps in STOP 2 with the debug port
# unclocked, so the first flash needs --connect-under-reset.
flash: $(BUILD)/$(TARGET).bin
	st-flash --connect-under-reset --reset write $< 0x08000000

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)

# Live semihosting console. Only meaningful for a build made with
# SEMIHOSTING=1; otherwise the firmware never traps and this just sits there.
# Ctrl-C to quit.
monitor:
	openocd -f interface/stlink.cfg -c "transport select hla_swd" \
	  -c "adapter speed 480" -f target/stm32l4x.cfg \
	  -c "reset_config srst_only srst_nogate connect_assert_srst" \
	  -c "init" -c "arm semihosting enable" -c "reset run"
