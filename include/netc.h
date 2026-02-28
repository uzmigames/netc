/**
 * netc.h — Public API for the netc network packet compression library
 *
 * netc is a C11 library for compressing low-entropy binary network packets
 * at wire speed. It operates as a buffer-to-buffer compression layer between
 * the application and whatever transport or buffering mechanism the caller uses.
 *
 * Transport agnosticism: netc has no knowledge of TCP, UDP, sockets, or any
 * transport protocol. NETC_CFG_FLAG_STATEFUL and NETC_CFG_FLAG_STATELESS
 * describe the calling pattern (ordered vs. independent payloads), not the
 * underlying transport.
 *
 * RFC-001: docs/rfc/RFC-001-netc-compression-protocol.md
 */

#ifndef NETC_H
#define NETC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Version
 * ========================================================================= */

#define NETC_VERSION_MAJOR 0
#define NETC_VERSION_MINOR 2
#define NETC_VERSION_PATCH 0
#define NETC_VERSION_STR   "0.2.0"

/* =========================================================================
 * Limits and constants
 * ========================================================================= */

/** Maximum input packet size (bytes). Inputs larger than this return NETC_ERR_TOOBIG. */
#define NETC_MAX_PACKET_SIZE  65535U

/**
 * Maximum bytes added to the output vs. the input (header only).
 * Callers may always allocate src_size + NETC_MAX_OVERHEAD for the dst buffer.
 */
#define NETC_MAX_OVERHEAD     8U

/** Compressed packet header size in bytes (RFC-001 §9.1, legacy format). */
#define NETC_HEADER_SIZE      8U

/** Compact header size: 2 bytes when original_size <= 127, 4 bytes otherwise. */
#define NETC_COMPACT_HDR_MIN  2U
#define NETC_COMPACT_HDR_MAX  4U

/* =========================================================================
 * Return codes (netc_result_t)
 * ========================================================================= */

typedef enum netc_result {
    NETC_OK               =  0,  /**< Success */
    NETC_ERR_NOMEM        = -1,  /**< Memory allocation failure */
    NETC_ERR_TOOBIG       = -2,  /**< Input exceeds NETC_MAX_PACKET_SIZE */
    NETC_ERR_CORRUPT      = -3,  /**< Corrupt or truncated compressed data */
    NETC_ERR_DICT_INVALID = -4,  /**< Dictionary checksum mismatch or bad format */
    NETC_ERR_BUF_SMALL    = -5,  /**< Output buffer capacity insufficient */
    NETC_ERR_CTX_NULL     = -6,  /**< NULL context pointer passed */
    NETC_ERR_UNSUPPORTED  = -7,  /**< Algorithm or feature not supported */
    NETC_ERR_VERSION      = -8,  /**< model_id or dictionary format version mismatch */
    NETC_ERR_INVALID_ARG  = -9,  /**< Invalid argument (NULL pointer, zero size, etc.) */
} netc_result_t;

/* =========================================================================
 * Packet header flags (NETC_PKT_FLAG_*) — RFC-001 §9.4
 * ========================================================================= */

/** Payload was delta-encoded from the previous packet. */
#define NETC_PKT_FLAG_DELTA    0x01U

/** Bigram context model was active during compression. */
#define NETC_PKT_FLAG_BIGRAM   0x02U

/** Uncompressed passthrough — payload is the original bytes verbatim. */
#define NETC_PKT_FLAG_PASSTHRU 0x04U

/** Dictionary model ID is present in the header (always set in v0.2). */
#define NETC_PKT_FLAG_DICT_ID  0x08U

/** Multi-region tANS: payload uses per-bucket streams (v0.2+). */
#define NETC_PKT_FLAG_MREG     0x10U

/** RLE pre-pass was applied before tANS (v0.2+). */
#define NETC_PKT_FLAG_RLE      0x20U

/** Dual-interleaved ANS (x2): payload has two initial states for ILP decode (v0.3+).
 *  Single-region wire format: [4B state0][4B state1][bitstream].
 *  MREG wire format: each region descriptor has 8B {state0 u32, state1 u32}
 *  followed by 4B bitstream_bytes (descriptor expands from 8B to 12B when X2). */
#define NETC_PKT_FLAG_X2       0x40U

/** LZ77 back-reference compression (v0.3+).
 *  Set on NETC_ALG_PASSTHRU packets when the payload is an LZ77 stream
 *  rather than raw bytes or RLE.  NETC_PKT_FLAG_PASSTHRU is always set
 *  alongside this flag.
 *
 *  Wire format (payload, no external dictionary):
 *    token stream of variable-length records:
 *      Literal run:  [0xxxxxxx] len=bits[6:0]+1; followed by len raw bytes
 *      Back-ref:     [1lllllll][ooooooooo] len=bits[6:0]+3, offset=byte+1
 *    A literal-run token with len=0 (byte=0x00) encodes 1 literal byte.
 *    Back-ref offset is 1-based (1–256 bytes back into decoded output). */
#define NETC_PKT_FLAG_LZ77     0x80U

/* =========================================================================
 * Algorithm identifiers — RFC-001 §9.3
 * ========================================================================= */

/** tANS (FSE) — primary codec, v0.1+. */
#define NETC_ALG_TANS     0x01U

/** rANS — secondary codec, deferred to v0.2. */
#define NETC_ALG_RANS     0x02U

/** Cross-packet LZ77 with ring-buffer history (v0.3+).
 *  Requires stateful mode. Token stream (NETC_ALG_LZ77X payload):
 *    [0lllllll]                  literal run: bits[6:0]+1 raw bytes (1–128)
 *    [10llllll][oooooooo]        short back-ref: len=bits[5:0]+3, offset=byte+1 (within-packet, 1–256)
 *    [11llllll][oo oooooo oooooooo] long back-ref: len=bits[5:0]+3, offset=u16le+1 (ring+dst, 1–65536)
 *  Encoder appends decoded bytes to ring buffer after each packet.
 *  Decoder reads from ring[ring_pos - offset .. ring_pos - 1] for long refs. */
#define NETC_ALG_LZ77X    0x03U

/** Per-position context-adaptive tANS (PCTX, v0.4+).
 *  Encodes all bytes in a SINGLE ANS stream but switches the probability
 *  table per byte offset: table = dict->tables[netc_ctx_bucket(offset)].
 *  This gives per-position entropy specialization (like MREG) with ZERO
 *  descriptor overhead — wire format is just [4B initial_state][bitstream].
 *  Preferred over MREG for packets < 512B where MREG descriptor overhead
 *  exceeds the benefit of separate per-region streams. */
#define NETC_ALG_TANS_PCTX 0x04U

/** LZP (Lempel-Ziv Prediction) — hash-context byte prediction (v0.5+).
 *  Predicts each byte by hashing the 3 previous bytes and looking up a
 *  trained hash table.  Matches cost ~1 bit; misses cost ~9 bits.
 *  Wire format: [2B n_literals LE][flag_bits][literal_bytes].
 *  Requires a v4+ dictionary with an LZP table trained via netc_dict_train(). */
#define NETC_ALG_LZP      0x05U

/** Uncompressed passthrough (incompressible data, AD-006). */
#define NETC_ALG_PASSTHRU 0xFFU

/* =========================================================================
 * Configuration flags (NETC_CFG_FLAG_*) — RFC-001 §10.4
 * ========================================================================= */

/**
 * Stateful mode: context accumulates history across sequential compress calls.
 * Use when payloads arrive in order on a reliable ordered channel.
 * Compatible with any ordered reliable medium (TCP, QUIC streams, ring buffers…).
 */
#define NETC_CFG_FLAG_STATEFUL  0x01U

/**
 * Stateless mode: each compress_stateless call is fully independent.
 * Use when payloads may arrive out of order or be lost.
 * ring_buffer_size is ignored when this flag is set.
 * Compatible with any medium (UDP datagrams, QUIC unreliable, shared memory…).
 */
#define NETC_CFG_FLAG_STATELESS 0x02U

/** Enable inter-payload delta prediction (field-class aware, AD-002). */
#define NETC_CFG_FLAG_DELTA     0x04U

/** Enable bigram context model (4 coarse buckets, RFC-001 §6.2). */
#define NETC_CFG_FLAG_BIGRAM    0x08U

/** Collect compression statistics (accessible via netc_ctx_stats). */
#define NETC_CFG_FLAG_STATS     0x10U

/** Use compact packet headers (2-4 bytes instead of 8).
 *  Eliminates compressed_size, model_id, and context_seq from the wire —
 *  they are derived from src_size and the context state.
 *  Both compressor and decompressor contexts MUST agree on this flag. */
#define NETC_CFG_FLAG_COMPACT_HDR 0x20U

/* =========================================================================
 * Opaque types
 * ========================================================================= */

/** Opaque compression context. One per logical connection per thread. */
typedef struct netc_ctx netc_ctx_t;

/** Opaque trained dictionary. Thread-safe for concurrent reads. */
typedef struct netc_dict netc_dict_t;

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct netc_stats {
    uint64_t packets_compressed;    /**< Total packets through netc_compress */
    uint64_t packets_decompressed;  /**< Total packets through netc_decompress */
    uint64_t bytes_in;              /**< Total input bytes */
    uint64_t bytes_out;             /**< Total output bytes (compressed) */
    uint64_t passthrough_count;     /**< Packets emitted as passthrough */
} netc_stats_t;

/* =========================================================================
 * Configuration — RFC-001 §10.4
 * ========================================================================= */

typedef struct netc_cfg {
    uint32_t flags;             /**< NETC_CFG_FLAG_* bitmask */
    size_t   ring_buffer_size;  /**< Stateful history ring buffer (0 = default 64KB) */
    uint8_t  compression_level; /**< 0=fastest … 9=best ratio (default: 5) */
    uint8_t  simd_level;        /**< 0=auto, 1=generic, 2=SSE4.2, 3=AVX2, 4=NEON */
    size_t   arena_size;        /**< Working memory arena (0 = default 3000 bytes) */
} netc_cfg_t;

/* =========================================================================
 * Context lifecycle — RFC-001 §10.1
 * ========================================================================= */

/**
 * Create a compression context.
 *
 * dict may be NULL for passthrough-only operation (no compression).
 * cfg may be NULL to use defaults (stateful, level 5, SIMD auto).
 *
 * Returns NULL on allocation failure.
 */
netc_ctx_t *netc_ctx_create(const netc_dict_t *dict, const netc_cfg_t *cfg);

/**
 * Destroy context and free all associated memory.
 * Passing NULL is safe (no-op).
 */
void netc_ctx_destroy(netc_ctx_t *ctx);

/**
 * Reset per-connection state (ring buffer, sequence counter) without
 * releasing memory or changing the dictionary. Call on connection reset
 * or reconnect. Safe to call from the same thread that owns the context.
 */
void netc_ctx_reset(netc_ctx_t *ctx);

/**
 * Retrieve accumulated statistics (only valid if NETC_CFG_FLAG_STATS set).
 * Returns NETC_ERR_CTX_NULL if ctx is NULL.
 * Returns NETC_ERR_UNSUPPORTED if NETC_CFG_FLAG_STATS was not set at creation.
 */
netc_result_t netc_ctx_stats(const netc_ctx_t *ctx, netc_stats_t *out);

/* =========================================================================
 * Dictionary management — RFC-001 §10.2
 * ========================================================================= */

/**
 * Train a dictionary from a corpus of representative packets.
 *
 * packets: array of pointers to packet data.
 * sizes:   array of packet sizes (one per packet).
 * count:   number of packets in the corpus.
 * model_id: 1–254 (0 reserved for passthrough, 255 reserved).
 * out_dict: receives the newly allocated dictionary on success.
 *
 * Returns NETC_OK on success. The caller owns the returned dictionary
 * and must free it with netc_dict_free().
 */
netc_result_t netc_dict_train(
    const uint8_t * const *packets,
    const size_t          *sizes,
    size_t                 count,
    uint8_t                model_id,
    netc_dict_t          **out_dict
);

/**
 * Load a dictionary from a binary blob (previously produced by netc_dict_save).
 * Validates the embedded CRC32 checksum before accepting.
 *
 * Returns NETC_OK on success. Caller owns the returned dictionary.
 */
netc_result_t netc_dict_load(const void *data, size_t size, netc_dict_t **out);

/**
 * Serialize a dictionary to a newly allocated binary blob.
 * The blob includes a CRC32 checksum for integrity validation.
 *
 * out:      receives a pointer to the allocated buffer.
 * out_size: receives the buffer size in bytes.
 *
 * The caller must free *out with netc_dict_free_blob().
 */
netc_result_t netc_dict_save(const netc_dict_t *dict, void **out, size_t *out_size);

/**
 * Free a binary blob returned by netc_dict_save.
 * Passing NULL is safe (no-op).
 */
void netc_dict_free_blob(void *blob);

/**
 * Free a dictionary returned by netc_dict_train or netc_dict_load.
 * Passing NULL is safe (no-op).
 */
void netc_dict_free(netc_dict_t *dict);

/**
 * Return the model_id embedded in this dictionary (1–254).
 * Returns 0 if dict is NULL.
 */
uint8_t netc_dict_model_id(const netc_dict_t *dict);

/* =========================================================================
 * Compression — RFC-001 §10.3
 * ========================================================================= */

/**
 * Compress a single packet (stateful context).
 *
 * ctx:      compression context (must not be NULL).
 * src:      input packet bytes.
 * src_size: input size (must be ≤ NETC_MAX_PACKET_SIZE).
 * dst:      output buffer (must have capacity ≥ src_size + NETC_MAX_OVERHEAD).
 * dst_cap:  output buffer capacity in bytes.
 * dst_size: receives the actual compressed output size on success.
 *
 * Passthrough guarantee (AD-006): if compression would expand the payload,
 * the original bytes are emitted with NETC_PKT_FLAG_PASSTHRU set.
 * Output is always ≤ src_size + NETC_HEADER_SIZE.
 *
 * On error: dst is not written, context state is unchanged.
 */
netc_result_t netc_compress(
    netc_ctx_t *ctx,
    const void *src,
    size_t      src_size,
    void       *dst,
    size_t      dst_cap,
    size_t     *dst_size
);

/**
 * Decompress a single packet (stateful context).
 *
 * dst_cap must be ≥ the original_size encoded in the packet header.
 * On error: dst is not written, context state is unchanged.
 */
netc_result_t netc_decompress(
    netc_ctx_t *ctx,
    const void *src,
    size_t      src_size,
    void       *dst,
    size_t      dst_cap,
    size_t     *dst_size
);

/**
 * Compress a single packet without a stateful context (stateless / independent).
 *
 * Each call is fully independent. dict must not be NULL.
 * Equivalent to creating a throw-away context with NETC_CFG_FLAG_STATELESS,
 * compressing, then destroying — but with zero allocation overhead.
 */
netc_result_t netc_compress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size
);

/**
 * Decompress a single packet without a stateful context.
 * dict must not be NULL.
 */
netc_result_t netc_decompress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size
);

/* =========================================================================
 * Utility
 * ========================================================================= */

/**
 * Return a human-readable description of a netc_result_t error code.
 * Always returns a non-NULL string (unknown codes return "unknown error").
 */
const char *netc_strerror(netc_result_t result);

/**
 * Return the netc library version string (e.g. "0.1.0").
 * Always returns a non-NULL, null-terminated string.
 */
const char *netc_version(void);

/**
 * Return the minimum output buffer size for compressing src_size bytes.
 * Equivalent to src_size + NETC_MAX_OVERHEAD. Safe to call with any value.
 */
static inline size_t netc_compress_bound(size_t src_size) {
    return src_size + NETC_MAX_OVERHEAD;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NETC_H */
