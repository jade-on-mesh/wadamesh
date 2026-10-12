/*
 * wadamesh: the largest DSP scratch buffers live in one heap arena (PSRAM on the
 * device) instead of on the stack, so a decode fits in the small task stacks an
 * ESP32 can spare from internal RAM. dav1d runs single-threaded here and these
 * functions never call one another, so each call carves its arrays from the
 * start of the arena. The embedding code allocates the arena before dav1d_open
 * (dav1d_ext_alloc_scratch, in wedge.c).
 */
#ifndef DAV1D_SRC_EXT_SCRATCH_H
#define DAV1D_SRC_EXT_SCRATCH_H

#include <stddef.h>
#include <stdint.h>

#if DAV1D_EXTERNAL_ALLOC
/* Largest user: put_8tap_c / prep_8tap_c, 128 * 135 int16_t = 34,560 B. */
#define DAV1D_EXT_SCRATCH_SIZE (40 * 1024)
extern uint8_t *dav1d_ext_scratch;
int dav1d_ext_alloc_scratch(void);
#define SCRATCH_BEGIN() size_t scratch_off_ = 0
#define SCRATCH_ARRAY(type, var, n) \
    type *const var = (type *)(dav1d_ext_scratch + \
        (scratch_off_ = (scratch_off_ + 63) & ~(size_t)63)); \
    scratch_off_ += (size_t)(n) * sizeof(type)
#else
#define SCRATCH_BEGIN() do {} while (0)
#define SCRATCH_ARRAY(type, var, n) ALIGN(type var[n], 64)
#endif

#endif /* DAV1D_SRC_EXT_SCRATCH_H */
