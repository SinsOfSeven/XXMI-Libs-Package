/*
 * Single-file unity build of the Zstandard decoder (decode-only).
 *
 * Zstandard is distributed under a BSD 3-Clause license:
 *   Copyright (c) Yann Collet, Facebook, Inc.
 *   https://github.com/facebook/zstd/blob/dev/LICENSE
 *
 * This amalgamation pulls in only the files required for decompression
 * (no encoder, no dictionary builder, no legacy frame formats).
 */

#define ZSTD_DISABLE_DEPRECATE_WARNINGS 1

#include "lib/common/error_private.c"
#include "lib/common/entropy_common.c"
#include "lib/common/fse_decompress.c"
#include "lib/common/xxhash.c"
#include "lib/common/zstd_common.c"

#include "lib/decompress/huf_decompress.c"
#include "lib/decompress/zstd_ddict.c"
#include "lib/decompress/zstd_decompress.c"
#include "lib/decompress/zstd_decompress_block.c"