// lz4_store.c — Thin C wrappers around LZ4 HC.
// Include vendored LZ4 source directly — compiled as a single translation unit.

#include "vendored/lz4/lz4.c"   // unity build: vendored source
#include "vendored/lz4/lz4hc.c" // unity build: vendored source

#include "lz4_store.h"

int cbm_lz4_compress_hc(const char *src, int srcLen, char *dst, int dstCap) {
    return LZ4_compress_HC(src, dst, srcLen, dstCap, 9);
}

int cbm_lz4_decompress(const char *src, int srcLen, char *dst, int originalLen) {
    return LZ4_decompress_safe(src, dst, srcLen, originalLen);
}

int cbm_lz4_bound(int inputSize) {
    return LZ4_compressBound(inputSize);
}
