// Kraken_GetBlockSize from kraken.cpp, the only part of the ooz decoder that the compressor calls.
#include "stdafx.h"
#include "compr_util.h"

int Kraken_GetBlockSize(const uint8 *src, const uint8 *src_end, int *dest_size, int dest_capacity) {
  const byte *src_org = src;
  int src_size, dst_size;

  if (src_end - src < 2)
    return -1; // too few bytes

  int chunk_type = (src[0] >> 4) & 0x7;
  if (chunk_type == 0) {
    if (src[0] >= 0x80) {
      // In this mode, memcopy stores the length in the bottom 12 bits.
      src_size = ((src[0] << 8) | src[1]) & 0xFFF;
      src += 2;
    } else {
      if (src_end - src < 3)
        return -1; // too few bytes
      src_size = ((src[0] << 16) | (src[1] << 8) | src[2]);
      if (src_size & ~0x3ffff)
        return -1; // reserved bits must not be set
      src += 3;
    }
    if (src_size > dest_capacity || src_end - src < src_size)
      return -1;
    *dest_size = src_size;
    return src + src_size - src_org;
  }

  if (chunk_type >= 6)
    return -1;

  // In all the other modes, the initial bytes encode
  // the src_size and the dst_size
  if (src[0] >= 0x80) {
    if (src_end - src < 3)
      return -1; // too few bytes

    // short mode, 10 bit sizes
    uint32 bits = ((src[0] << 16) | (src[1] << 8) | src[2]);
    src_size = bits & 0x3ff;
    dst_size = src_size + ((bits >> 10) & 0x3ff) + 1;
    src += 3;
  } else {
    // long mode, 18 bit sizes
    if (src_end - src < 5)
      return -1; // too few bytes
    uint32 bits = ((src[1] << 24) | (src[2] << 16) | (src[3] << 8) | src[4]);
    src_size = bits & 0x3ffff;
    dst_size = (((bits >> 18) | (src[0] << 14)) & 0x3FFFF) + 1;
    if (src_size >= dst_size)
      return -1;
    src += 5;
  }
  if (src_end - src < src_size || dst_size > dest_capacity)
    return -1;
  *dest_size = dst_size;
  return src_size;
}
