#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace fs {
struct Node { bool dir; uint8_t* data; size_t size; };
extern Node g_fw; extern const char* g_fw_path;
inline Node* lookup(const char* p) { return g_fw_path && strcmp(p, "/lib/firmware/intel/vpu/vpu_37xx_v1.bin") == 0 ? &g_fw : nullptr; }
inline bool ensure_data(Node*) { return true; }
inline void* put_file(const char* p, const void* d, size_t n) { printf("[put_file %s, %zu bytes]\n", p, n);
  if (strstr(p, "verbose")) { fwrite(d, 1, n, stdout); printf("\n[end]\n"); } return nullptr; }
}
