# ESP32 Port (ESP-IDF)

Reference platform port of the TinyMLDelta core for ESP32 / ESP-IDF. It
implements the `tmd_ports_t` / `tmd_layout_t` HAL against the ESP-IDF partition
API, with A/B model slots + journal in a custom flash partition and the
active-slot index in NVS.

> **Reference port — validate on hardware.** This is written against the
> documented ESP-IDF API and structured like the tested POSIX port, but has not
> yet been run on a device. Verify `esp_partition` write/erase alignment and NVS
> behaviour on real hardware before production use.

## Files

| File | Purpose |
|------|---------|
| `tinymldelta_ports_esp32.c` | the `tmd_ports_t` implementation (flash/CRC/slots/journal/log) |
| `partitions.csv` | example partition table with a `tmd_models` data partition |

## Layout

The `tmd_models` partition holds (offsets are partition-relative):

```
[ slot A : 256 KiB ][ slot B : 256 KiB ][ journal : 4 KiB ]
```

Override `TMD_ESP32_SLOT_BYTES` / `TMD_ESP32_META_BYTES` at build time to resize.
Slots and journal must be 4 KiB-sector aligned (the defaults are).

## Use

```c
#include "tinymldelta.h"

extern bool tmd_esp32_init(void);   // from tinymldelta_ports_esp32.c

void app_main(void) {
    if (!tmd_esp32_init()) { /* partition/NVS error */ return; }

    // ... receive a .tmd patch over BLE / Wi-Fi / MQTT into `patch` ...
    tmd_status_t st = tmd_apply_patch_from_memory(patch, patch_len);
    // on TMD_STATUS_OK the inactive slot holds the new model and is now active.
}
```

## Build (ESP-IDF component)

Add the core + this port to a component's `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "tinymldelta_ports_esp32.c"
         "${TINYMLDELTA}/runtime/src/tinymldelta_core.c"
    INCLUDE_DIRS "${TINYMLDELTA}/runtime/include"
    REQUIRES spi_flash nvs_flash esp_partition log)
```

Set the partition table in `menuconfig` → *Partition Table* → *Custom* →
`partitions.csv`.

## Optional features

Enable via build flags (e.g. in `CMakeLists.txt` `target_compile_definitions`):

- `TMD_FEAT_LZ4TINY=1` / `TMD_FEAT_COPYADD=1` — LZ4 / structure-aware patches
- `TMD_FEAT_VERIFY_SIG=1` — require patch authenticity; provide a `verify_patch`
  in the ports table (mbedTLS Ed25519/ECDSA, a secure element, or SUIT/COSE).
  See [../../docs/security.md](../../docs/security.md).
