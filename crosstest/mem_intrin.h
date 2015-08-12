/*
 * Simple sanity test of memcpy, memmove, and memset intrinsics.
 * (fixed length buffers, variable length buffers, etc.).
 * There is no include guard since this will be included multiple times,
 * under different namespaces.
 */
#include "xdefs.h"

int memcpy_test(uint8_t *buf, uint8_t *buf2, uint8_t init, SizeT length);
int memmove_test(uint8_t *buf, uint8_t *buf2, uint8_t init, SizeT length);
int memset_test(uint8_t *buf, uint8_t *buf2, uint8_t init, SizeT length);

int memcpy_test_fixed_len(uint8_t init);
int memmove_test_fixed_len(uint8_t init);
int memset_test_fixed_len(uint8_t init);
