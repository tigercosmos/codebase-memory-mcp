/* UniXcoder (microsoft/unixcoder-base) token embeddings.
 * 51416 tokens × 768d int8-quantized unit vectors.
 * 
 * Vector blob embedded via code_vectors_blob.S (assembler .incbin).
 * Token strings are in this header as a static array.
 *
 * Source: https://huggingface.co/microsoft/unixcoder-base
 * License: MIT
 */
#ifndef CBM_UNIXCODER_VECTORS_H
#define CBM_UNIXCODER_VECTORS_H

#include <stdint.h>

#define PRETRAINED_TOKEN_COUNT 51416
#define PRETRAINED_DIM 768

/* Raw vector blob: first 8 bytes = [int32 count][int32 dim],
 * then count × dim int8 values (unit-normalized, ×127 scaled). */
extern const unsigned char PRETRAINED_VECTOR_BLOB[];
extern const unsigned int PRETRAINED_VECTOR_BLOB_LEN;

/* Access the int8 vector for token index i. */
static inline const int8_t *pretrained_vec_at(int i) {
    return (const int8_t *)(PRETRAINED_VECTOR_BLOB + 8 + (size_t)i * PRETRAINED_DIM);
}

/* Token strings (separate header to keep this file clean). */
#include "code_tokens.h"

#endif /* CBM_UNIXCODER_VECTORS_H */
