/*
 * JuiceFS, Copyright 2026 Juicedata, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef QAT_LZ4_H
#define QAT_LZ4_H

#include <stdint.h>

struct qlz4_stats {
    uint64_t comp_ok, comp_fallback, decomp_ok, decomp_fallback;
};

/* Returns the number of usable QAT dc instances (>0) or a negative error. */
int qlz4_init(int level);
/* Raw LZ4 block (liblz4 LZ4_compress_default compatible); <0 means use software. */
int qlz4_compress(const void *src, int src_len, void *dst, int dst_cap);
/* Decompress a raw LZ4 block; returns the decompressed size or <0 (use software). */
int qlz4_decompress(const void *src, int src_len, void *dst, int dst_cap);
void qlz4_get_stats(struct qlz4_stats *s);

#endif
