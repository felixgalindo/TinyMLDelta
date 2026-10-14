# STM32L4 port for the SensorTile

The `stm32l4_sensortile` example runs the TinyMLDelta core on an STM32L476
microcontroller, and it patches a model that is stored in the chip's own
flash. The core in `runtime/` is used without changes, and the example adds
the port and a small test firmware.

We tested it on a SensorTile (STEVAL-STLCS01V1, which has an STM32L476JG) in
the cradle expansion board. The ST-LINK/V2-1 programmer on a Nucleo board
connects to the SensorTile over SWD, with the Nucleo's CN2 jumpers removed.
Any STM32L476xG board should work with the same code, because the flash
layout only uses the first 512 KiB.

## How a patch is applied

The host computer writes the base model and the patch into flash over SWD.
Then it resets the board, and the firmware applies the patch.

```
   host (PC)                                  STM32L476 flash, bank 1
   ---------                                  -----------------------
                                              0x08000000  application  (384 KiB, limited by the linker)
   base.tflite ---- provision (SWD) --------> 0x08060000  slot A       (32 KiB)
                                              0x08068000  slot B       (32 KiB)
                                              0x08070000  journal      (2 KiB)
                                              0x08070800  slot marker  (2 KiB)
   patch.tmd ------ stage (SWD) ------------> 0x08078000  staging      (32 KiB)

                       reset
                         |
                         v
           firmware boots, finds the staged patch, and calls
           tmd_apply_patch_from_memory(), which does these steps:
             1. check the digest of the active slot
             2. copy the active slot to the other slot
             3. apply the chunks, checking the CRC of each one
             4. switch the slot marker to the new slot
           then the firmware erases the staging area
                         |
   read back <-----------+
   compare with target.tflite, byte for byte
```

## Why the port keeps a page of flash in RAM

The core writes chunks at any offset and with any length, and it sometimes
writes the same bytes twice. STM32L4 flash can't do either, because of three
limits:

- It erases in pages of 2 KiB.
- It programs 8 bytes at a time, at an address that is a multiple of 8.
- It can only program bytes that are erased, and programming them a second
  time can cause an ECC error (an error in the flash's own error-correcting
  code).

So `src/tmd_port_stm32l4.c` keeps one page of flash in RAM:

```
flash_write()  ->  load the page into RAM if needed, then change bytes in RAM
flash_read()   ->  read from RAM if that page has unsaved changes, else from flash
page change    ->  erase the page, then program it from RAM
```

With the page in RAM, the core always reads back the bytes it wrote, and each
flash page is erased and then programmed once.

The port also handles three other jobs:

- The slot marker is one 8-byte record in its own page, and an erased marker
  means slot A. The port writes the marker only after the new slot is fully in
  flash. If the power fails while the marker is written, the device boots
  either the old model or the new one, and both of them are complete.
- The journal is a list of 16-byte records in one page. The port adds records
  to the end, so it doesn't have to erase the page for every chunk, and it
  ignores a record that was only half written.
- The port refuses any erase or write outside the inactive slot. The core only
  needs to write the inactive slot, so the check costs nothing in normal use,
  and a faulty patch can't change the running model or the application.

## What you need

You need the following tools on the host computer:

- `arm-none-eabi-gcc`, plus `st-flash` (from the stlink tools) and `openocd`
- Python 3, for `tools/tmd_host.py`
- An ST package with the STM32L4 HAL and CMSIS, e.g. STM32CubeL4 or a
  SensorTile function pack

The build uses ST's startup and system files from the ST package without
changes, so they are not copied into the TinyMLDelta repo.

## Build and run

```bash
cd examples/stm32l4_sensortile
make STM32CUBE=/path/to/STM32Cube_FW_L4      # builds build/tmd-stm32l4.bin
make STM32CUBE=/path/to/STM32Cube_FW_L4 flash

# make a patch with the normal CLI
python ../../cli/tinymldelta_patchgen.py ../posix/base.tflite ../posix/target.tflite patch.tmd

python3 tools/tmd_host.py provision ../posix/base.tflite
python3 tools/tmd_host.py run patch.tmd --expect ../posix/target.tflite
```

The `run` command writes the patch to the staging area, resets the board, and
waits for the firmware to finish. Then it prints the device log, reads the
active slot back, and compares it with the expected model.

The firmware writes its log to a buffer in RAM, and the host reads the buffer
over SWD, so you don't need a UART. For a live log, build with
`make SEMIHOSTING=1` and run `make monitor`. Semihosting is off by default,
because the CPU stops when it runs a semihosting call with no debugger
attached.

Before you flash the board for the first time, save a full copy of its flash
with `st-flash read backup.bin 0x08000000 0x100000`. Nothing in the example
writes the option bytes.

## LED codes

| Pattern | Meaning |
|---|---|
| slow, 1 Hz | no patch was staged |
| fast, 5 Hz | the patch was applied |
| double blink, then a pause | the patch was rejected or failed |
| rapid flicker | hard fault |

## Test results on hardware

We ran all tests on one SensorTile, with the final build of the port. In
each test, the host read the active slot back over SWD and compared it byte
for byte with the expected model file. For the tests where the patch should
fail, the expected file is the original model, so a pass means the running
model did not change.

Most tests used a 21,560-byte int8 audio classifier that is not part of the
TinyMLDelta repo. Test 7 uses the models in `examples/posix`, so you can repeat it with
the commands above.

| # | Test | Result |
|---|---|---|
| 1 | 21,560-byte int8 model, 34-chunk patch (RAW and RLE chunks) | applied, new model matches |
| 2 | The same patch applied a second time | rejected (base digest), model unchanged |
| 3 | Patch back to the old model, from slot B to slot A | applied, model matches |
| 4 | One byte changed inside a chunk payload | rejected (chunk CRC), model unchanged |
| 5 | Small patch of 328 bytes and 4 chunks | applied, model matches |
| 6 | Patch made for a different base model | rejected (base digest), model unchanged |
| 7 | `examples/posix` base to target, patch from the current PatchGen | applied, model matches |
| 8 | Chunk aimed outside the inactive slot, into the running slot | refused by the port, model unchanged |
| 9 | Chunk aimed at the application's flash | refused by the port, application unchanged |
| 10 | Half-written journal record left in flash before the patch | record ignored, model matches |
| 11 | 200-chunk patch, so the journal page fills and is erased | applied, model matches |

The repo's own test suite (`SAN=1 tests/run_all.sh`) and the POSIX demo
(`examples/posix/run_demo.sh`) still pass. The port doesn't change any
existing file except the top-level README.

## Size and speed

The firmware uses 11,072 bytes of flash and 22,640 bytes of RAM, and 16 KiB of
the RAM is the log buffer. A full run (write the patch, reset, apply, read
back) took 5 to 20 seconds. The CPU runs at the 4 MHz clock it starts with,
because the example has no work that needs a faster clock.

## Known limits

- Each `journal_write()` call first saves the cached page to flash, so the
  journal never records a chunk before the chunk is in flash. The cost is
  about one extra page erase per chunk, e.g. about 34 erases for a 34-chunk
  patch, and each flash page can be erased about 10,000 times. Build with
  `TMD_FEAT_JOURNAL=0` if the extra erases cost too much for your device.
- The core saves its progress in the journal, but after a reset it starts
  again from chunk 0. The restart is safe, because the core copies the slot
  again first, but it doesn't save time.
- The staging area and each slot hold 32 KiB. To make them larger, change
  `inc/flash_layout.h` and `tools/tmd_host.py` together.

## Files

```
inc/flash_layout.h        flash map, and the reasons for it
inc/tmd_port_stm32l4.h    what the port offers main.c: sync and a flash pointer
inc/board.h               LED, RAM log, status block
inc/stm32l4xx_it.h        exception handlers
inc/string.h              small <string.h>, because no C library is linked
inc/stm32l4xx_hal_conf.h  ST HAL configuration (BSD-3-Clause, from ST's template)
src/tmd_port_stm32l4.c    the port: page cache, address check, journal, marker
src/main.c                boot, find the patch, apply it, report, blink
src/board.c               LED, RAM log, memcpy and memset
stm32l476xg.ld            linker script; the application can use 384 KiB
tools/tmd_host.py         provision, stage, run, verify, status over SWD
```
