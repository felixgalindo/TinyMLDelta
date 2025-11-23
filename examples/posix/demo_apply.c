/**
 * @file demo_apply.c
 * @brief TinyMLDelta POSIX demo: apply a .tmd patch to a simulated flash image.
 *
 * This is a small command-line utility used by the POSIX demo environment.
 *
 * It demonstrates how an embedded device would:
 *   1) Load a TinyMLDelta patch from disk (simulating OTA receive)
 *   2) Pass the patch to the TinyMLDelta core engine
 *   3) Allow the core to update an inactive flash slot safely
 *   4) Atomically flip the active slot if all checks pass
 *
 * The "flash" is just a file (flash.bin), and the active slot index is stored
 * in a small companion file (active_slot.txt). These are provided by the
 * POSIX port (tinymldelta_ports_posix.c).
 *
 * Usage:
 *      ./demo_apply flash.bin patch.tmd
 *
 * This mimics how a real MCU would consume a downloaded patch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>

#include "tinymldelta.h"
#include "tinymldelta_ports.h"

/*
 * These functions are implemented in tinymldelta_ports_posix.c.
 *
 * They configure:
 *   - Where the simulated flash storage is (flash.bin)
 *   - Where the POSIX port stores/reads the "active slot" index
 *
 * The TinyMLDelta runtime calls the port layer when it needs to:
 *   - read/erase/write flash
 *   - update active slot
 *   - use journaling for crash-safe updates
 *   - print log messages
 */
void tmd_posix_set_flash_path(const char* path);
void tmd_posix_set_active_slot_path(const char* path);

/**
 * @brief Load any file fully into memory.
 *
 * This helper is used to read *.tmd patch files completely into RAM so that
 * the TinyMLDelta core can parse and apply them. In a real MCU, patches
 * may arrive via OTA and be streamed instead—this is purely for demo purposes.
 *
 * @param path     Path to file
 * @param out_buf  Returned buffer (malloc'ed)
 * @param out_len  Returned length
 * @return 0 on success, -1 on failure
 */
static int load_file(const char* path, uint8_t** out_buf, size_t* out_len) {
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;

  FILE* f = fopen(path, "rb");
  if (!f)
    return -1;

  uint8_t* buf = (uint8_t*)malloc((size_t)st.st_size);
  if (!buf) {
    fclose(f);
    return -1;
  }

  if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
    fclose(f);
    free(buf);
    return -1;
  }

  fclose(f);
  *out_buf = buf;
  *out_len = (size_t)st.st_size;
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr,
            "Usage: %s flash.bin patch.tmd\n"
            "Example:\n"
            "    ./demo_apply flash.bin patch.tmd\n",
            argv[0]);
    return 1;
  }

  const char* flash_path = argv[1];
  const char* patch_path = argv[2];

  /*
   * Tell the POSIX port where the simulated flash file lives.
   * The TinyMLDelta core will read/write flash via the tmd_ports_t interface.
   */
  tmd_posix_set_flash_path(flash_path);

  /*
   * The POSIX demo stores the "active slot index" in a small text file.
   * This mimics how a bootloader might store slot state in NVM.
   */
  tmd_posix_set_active_slot_path("active_slot.txt");

  uint8_t* patch = NULL;
  size_t patch_len = 0;

  if (load_file(patch_path, &patch, &patch_len) != 0) {
    fprintf(stderr, "Failed to read patch file: %s\n", patch_path);
    return 1;
  }

  /*
   * Apply the patch from memory.
   *
   * Internally, TinyMLDelta will:
   *   - Parse the patch header + TLVs
   *   - Validate compatibility guardrails (arena, ABI, opset, IO schema)
   *   - Validate integrity digests (CRC32)
   *   - Write to the inactive slot
   *   - Update journaling for crash safety
   *   - Atomically flip active slot on success
   */
  tmd_status_t st = tmd_apply_patch_from_memory(patch, patch_len);
  free(patch);

  if (st != TMD_STATUS_OK) {
    fprintf(stderr, "Patch apply failed with status %d\n", (int)st);
    return 2;
  }

  fprintf(stdout, "Patch applied successfully.\n");
  return 0;
}
