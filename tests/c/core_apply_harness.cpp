/**
 * @file core_apply_harness.cpp
 * @brief Applies a .tmd patch to an in-memory base via the real TinyMLDelta
 *        core, then writes the reconstructed target to a file.
 *
 *   core_apply <base.bin> <patch.tmd> <out.bin> <slot_size>
 *
 * Exit 0 on success; non-zero if the core rejects the patch. The driver script
 * compares <out.bin> against the expected target byte-for-byte.
 *
 * License: Apache-2.0
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "tinymldelta.h"
#include "tmd_port_memory.h"  /* provides tmd_ports()/tmd_layout() + mem helpers */

static std::vector<unsigned char> read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<unsigned char> buf(n > 0 ? n : 0);
  if (n > 0 && fread(buf.data(), 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(2); }
  fclose(f);
  return buf;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <base.bin> <patch.tmd> <out.bin> <slot_size>\n", argv[0]);
    return 2;
  }
  std::vector<unsigned char> base = read_file(argv[1]);
  std::vector<unsigned char> patch = read_file(argv[2]);
  size_t slot = strtoul(argv[4], nullptr, 10);

  /* target_len lives at byte offset 8 of the patch header (uint32 LE). */
  if (patch.size() < 12) { fprintf(stderr, "patch too small\n"); return 2; }
  uint32_t target_len;
  memcpy(&target_len, patch.data() + 8, 4);

  tmd_mem_setup(base.data(), base.size(), slot);
  tmd_status_t st = tmd_apply_patch_from_memory(patch.data(), patch.size());
  if (st != TMD_STATUS_OK) {
    fprintf(stderr, "apply failed: status=%d\n", (int)st);
    return 1;
  }

  size_t rlen = 0;
  const uint8_t *res = tmd_mem_result(&rlen);
  FILE *o = fopen(argv[3], "wb");
  if (!o) { perror(argv[3]); return 2; }
  fwrite(res, 1, target_len, o);  /* only the real model, not slot padding */
  fclose(o);
  tmd_mem_cleanup();
  return 0;
}
