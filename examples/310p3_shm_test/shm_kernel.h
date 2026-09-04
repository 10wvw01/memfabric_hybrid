#ifndef __SHM_KERNEL_H__
#define __SHM_KERNEL_H__

#include <cstdint>

void shm_shift_kernel_do(void *stream, uint8_t *gva, int64_t *localInput, int64_t *localOutput);

#endif
