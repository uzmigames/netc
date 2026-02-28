/**
 * netc_crc32.h — CRC32 (IEEE 802.3) interface.
 *
 * INTERNAL HEADER — not part of the public API.
 *
 * Canonical IEEE CRC32 implementation (polynomial 0xEDB88320 reflected).
 * All SIMD dispatch paths (generic, SSE4.2, NEON) produce identical IEEE
 * CRC32 output, ensuring portable dictionary checksums across platforms.
 *
 * The SIMD dispatch table (netc_simd_ops_t.crc32_update) routes through
 * this implementation. SSE4.2's _mm_crc32_u* computes CRC32C (Castagnoli),
 * a DIFFERENT polynomial, so the SSE42 slot delegates to generic software.
 * ARM NEON (ARMv8.1+ __crc32d) natively computes IEEE CRC32.
 */

#ifndef NETC_CRC32_H
#define NETC_CRC32_H

#include "netc_platform.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Update a running CRC32 checksum with new data.
 *
 * crc:  initial value (pass 0 to start a new checksum)
 * data: input bytes
 * len:  number of bytes
 *
 * Returns the updated CRC32. Finalize by XOR-ing with 0xFFFFFFFF:
 *   uint32_t result = netc_crc32_update(0, data, len);
 *   (the function applies the initial and final ~crc internally)
 *
 * Usage:
 *   // Single buffer:
 *   uint32_t crc = netc_crc32(data, len);
 *
 *   // Incremental (multiple buffers):
 *   uint32_t crc = netc_crc32_update(0, buf1, len1);
 *   crc = netc_crc32_continue(crc, buf2, len2);
 */
uint32_t netc_crc32(const void *data, size_t len);
uint32_t netc_crc32_continue(uint32_t crc, const void *data, size_t len);

#endif /* NETC_CRC32_H */
