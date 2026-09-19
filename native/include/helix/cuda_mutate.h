#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif
int helix_cuda_havoc(uint8_t *host, int seed_len, int nseeds, uint64_t tick);
#ifdef __cplusplus
}
#endif
