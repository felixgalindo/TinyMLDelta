/**
 * @file demo_apply.c
 * @brief Small CLI that applies a TinyMLDelta patch to flash.bin (POSIX).
 *
 * Usage:
 *   ./demo_apply flash.bin patch.tmd
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h>

#include "tinymldelta.h"
#include "tinymldelta_ports.h"

/* Provided by tinymldelta_ports_posix.c */
void tmd_posix_set_flash_path(const char* path);
void tmd_posix_set_active_slot_path(const char* path);

static int load_file(const char* path, uint8_t** out_buf, size_t* out_len) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  uint8_t* buf = (uint8_t*)malloc((size_t)st.st_size);
  if (!buf) { fclose(f); return -1; }
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
    fprintf(stderr, "Usage: %s flash.bin patch.tmd\n", argv[0]);
    return 1;
  }
  const char* flash_path = argv[1];
  const char* patch_path = argv[2];

  tmd_posix_set_flash_path(flash_path);
  tmd_posix_set_active_slot_path("active_slot.txt");

  uint8_t* patch = NULL;
  size_t patch_len = 0;
  if (load_file(patch_path, &patch, &patch_len) != 0) {
    fprintf(stderr, "Failed to read patch file %s\n", patch_path);
    return 1;
  }

  tmd_status_t st = tmd_apply_patch_from_memory(patch, patch_len);
  free(patch);

  if (st != TMD_STATUS_OK) {
    fprintf(stderr, "Patch apply failed with status %d\n", (int)st);
    return 2;
  }

  fprintf(stdout, "Patch applied successfully.\n");
  return 0;
}
