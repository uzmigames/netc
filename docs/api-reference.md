# netc API Reference

Version: **0.1.0** — All public functions declared in `include/netc.h`.

---

## Contents

1. [Constants and Limits](#1-constants-and-limits)
2. [Return Codes](#2-return-codes)
3. [Flags](#3-flags)
4. [Types](#4-types)
5. [Context Lifecycle](#5-context-lifecycle)
6. [Dictionary Management](#6-dictionary-management)
7. [Compression](#7-compression)
8. [Utility](#8-utility)
9. [Error Handling](#9-error-handling)
10. [Thread Safety](#10-thread-safety)

---

## 1. Constants and Limits

```c
#define NETC_VERSION_MAJOR  0
#define NETC_VERSION_MINOR  1
#define NETC_VERSION_PATCH  0
#define NETC_VERSION_STR    "0.1.0"

#define NETC_MAX_PACKET_SIZE  65535U   // max input size (bytes)
#define NETC_MAX_OVERHEAD     8U       // max bytes added by header (conservative bound)
#define NETC_HEADER_SIZE      8U       // legacy compressed packet header size

// Compact header constants (used when NETC_CFG_FLAG_COMPACT_HDR is set)
#define NETC_COMPACT_HDR_MIN  2U       // compact header size for packets <= 127B
#define NETC_COMPACT_HDR_MAX  4U       // compact header size for packets > 127B
```

- Input packets exceeding `NETC_MAX_PACKET_SIZE` return `NETC_ERR_TOOBIG`.
- The output buffer must be at least `src_size + NETC_MAX_OVERHEAD` bytes. Use `netc_compress_bound()` to compute this safely.
- `NETC_MAX_OVERHEAD` remains 8 for backward compatibility; compact mode actual overhead is 2-4 bytes.

---

## 2. Return Codes

```c
typedef enum netc_result {
    NETC_OK               =  0,
    NETC_ERR_NOMEM        = -1,   // allocation failure
    NETC_ERR_TOOBIG       = -2,   // input > NETC_MAX_PACKET_SIZE
    NETC_ERR_CORRUPT      = -3,   // corrupt or truncated data
    NETC_ERR_DICT_INVALID = -4,   // bad dictionary format or checksum
    NETC_ERR_BUF_SMALL    = -5,   // output buffer too small
    NETC_ERR_CTX_NULL     = -6,   // NULL context
    NETC_ERR_UNSUPPORTED  = -7,   // algorithm or feature not available
    NETC_ERR_VERSION      = -8,   // model_id or dict format mismatch
    NETC_ERR_INVALID_ARG  = -9,   // NULL pointer, zero size, etc.
} netc_result_t;
```

Use `netc_strerror(result)` to get a human-readable description.

---

## 3. Flags

### Packet header flags (`NETC_PKT_FLAG_*`)

Set by the compressor in the packet header; read by the decompressor.

| Flag | Value | Meaning |
|------|------:|---------|
| `NETC_PKT_FLAG_DELTA`    | `0x01` | Payload was delta-encoded |
| `NETC_PKT_FLAG_BIGRAM`   | `0x02` | Bigram context model active |
| `NETC_PKT_FLAG_PASSTHRU` | `0x04` | Uncompressed passthrough payload |
| `NETC_PKT_FLAG_DICT_ID`  | `0x08` | Dictionary model_id verified |
| `NETC_PKT_FLAG_LZ77`     | `0x10` | LZ77 within-packet compression |
| `NETC_PKT_FLAG_MREG`     | `0x20` | Multi-region tANS (multiple buckets per packet) |
| `NETC_PKT_FLAG_X2`       | `0x40` | Dual-interleaved tANS streams |
| `NETC_PKT_FLAG_RLE`      | `0x80` | RLE pre-pass applied |

### Algorithm identifiers (`NETC_ALG_*`)

| Constant | Value | Description |
|----------|------:|-------------|
| `NETC_ALG_TANS`      | `0x01` | tANS/FSE — primary codec. Upper 4 bits encode bucket index. |
| `NETC_ALG_RANS`      | `0x02` | rANS — planned for v0.2 |
| `NETC_ALG_TANS_PCTX` | `0x03` | Per-position context-adaptive tANS |
| `NETC_ALG_LZP`       | `0x04` | LZP XOR pre-filter + tANS. Upper 4 bits encode bucket index. |
| `NETC_ALG_LZ77X`     | `0x05` | Cross-packet LZ77 (ring buffer history) |
| `NETC_ALG_PASSTHRU`  | `0xFF` | Uncompressed passthrough |

### Configuration flags (`NETC_CFG_FLAG_*`)

Passed to `netc_ctx_create` via `netc_cfg_t.flags`.

| Flag | Value | Description |
|------|------:|-------------|
| `NETC_CFG_FLAG_STATEFUL`    | `0x01` | Ordered sequential payloads (accumulates ring buffer history) |
| `NETC_CFG_FLAG_STATELESS`   | `0x02` | Independent payloads (no shared history) |
| `NETC_CFG_FLAG_DELTA`       | `0x04` | Enable inter-packet delta prediction |
| `NETC_CFG_FLAG_BIGRAM`      | `0x08` | Enable bigram context model |
| `NETC_CFG_FLAG_STATS`       | `0x10` | Enable statistics collection |
| `NETC_CFG_FLAG_COMPACT_HDR` | `0x20` | Use compact 2-4B packet header (see RFC-001 §9.1a). Must be set on both compressor and decompressor contexts. Also enables ANS state compaction (2B instead of 4B). |
| `NETC_CFG_FLAG_FAST_COMPRESS` | `0x100` | Speed mode: skip trial passes for ~2-5% ratio cost, 8-62% throughput gain. Decompressor does not need this flag. |
| `NETC_CFG_FLAG_ADAPTIVE` | `0x200` | Enable adaptive cross-packet learning. Requires `STATEFUL`. Adapts tANS frequency tables (rebuilt every 128 packets), LZP hash predictions, and delta prediction order (order-2 when beneficial) to the live data stream. Both encoder and decoder must set this flag. Context memory ~1 MB with all features enabled. |

---

## 4. Types

### `netc_cfg_t`

```c
typedef struct netc_cfg {
    uint32_t flags;             // NETC_CFG_FLAG_* bitmask
    size_t   ring_buffer_size;  // stateful ring buffer size (0 = 64 KB default)
    uint8_t  compression_level; // 0..9 (0 = fastest; 9 = best ratio; default 5)
    uint8_t  simd_level;        // 0=auto, 1=generic, 2=SSE4.2, 3=AVX2, 4=NEON
    size_t   arena_size;        // working memory arena (0 = default ~131 KB)
} netc_cfg_t;
```

Zero-initialize and then set only the fields you care about:

```c
netc_cfg_t cfg;
memset(&cfg, 0, sizeof(cfg));
cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;
```

### `netc_stats_t`

```c
typedef struct netc_stats {
    uint64_t packets_compressed;
    uint64_t packets_decompressed;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t passthrough_count;
} netc_stats_t;
```

Only populated when `NETC_CFG_FLAG_STATS` is set.

---

## 5. Context Lifecycle

### `netc_ctx_create`

```c
netc_ctx_t *netc_ctx_create(const netc_dict_t *dict, const netc_cfg_t *cfg);
```

Allocate and initialize a compression context.

**Parameters:**
- `dict` — Shared trained dictionary. May be `NULL` for passthrough-only mode.
- `cfg` — Configuration. May be `NULL` to use defaults (stateful, level 5, SIMD auto).

**Returns:** Pointer to the new context, or `NULL` on allocation failure.

**Thread safety:** Not thread-safe. Create one context per connection per thread.

**Notes:**
- The context does not take ownership of `dict` — the dictionary must remain valid and unmodified for the lifetime of the context.
- One context per logical stream direction. For bidirectional stateful streams, create two contexts (one for encode, one for decode).

---

### `netc_ctx_destroy`

```c
void netc_ctx_destroy(netc_ctx_t *ctx);
```

Free all resources associated with the context. Safe to call with `NULL`.

---

### `netc_ctx_reset`

```c
void netc_ctx_reset(netc_ctx_t *ctx);
```

Reset per-connection state (ring buffer, sequence counter) without releasing memory or changing the dictionary.

Call on connection reset or reconnect to re-synchronize state with the remote peer.

---

### `netc_ctx_stats`

```c
netc_result_t netc_ctx_stats(const netc_ctx_t *ctx, netc_stats_t *out);
```

Retrieve accumulated statistics. Requires `NETC_CFG_FLAG_STATS` at context creation.

**Returns:**
- `NETC_OK` — `*out` filled with current counters.
- `NETC_ERR_CTX_NULL` — `ctx` is `NULL`.
- `NETC_ERR_UNSUPPORTED` — stats not enabled.

---

## 6. Dictionary Management

### `netc_dict_train`

```c
netc_result_t netc_dict_train(
    const uint8_t * const *packets,
    const size_t          *sizes,
    size_t                 count,
    uint8_t                model_id,
    netc_dict_t          **out_dict
);
```

Train a probability model from a corpus of representative packets.

**Parameters:**
- `packets` — Array of `count` pointers to packet data.
- `sizes` — Corresponding packet sizes.
- `count` — Number of packets (recommended: ≥ 50,000 for good coverage).
- `model_id` — Identifier embedded in the dictionary (1–254). Used to detect model mismatch at decompress time.
- `out_dict` — Receives the newly allocated dictionary on success.

**Returns:** `NETC_OK` on success. The caller must free with `netc_dict_free()`.

**Example:**

```c
const uint8_t *pkts[50000];
size_t         lens[50000];
// ... fill from network capture ...

netc_dict_t *dict = NULL;
netc_result_t r = netc_dict_train(pkts, lens, 50000, 1, &dict);
if (r != NETC_OK) { /* handle error */ }
```

---

### `netc_dict_save`

```c
netc_result_t netc_dict_save(const netc_dict_t *dict, void **out, size_t *out_size);
```

Serialize the dictionary to a binary blob. Includes CRC32 checksum for integrity.

**Returns:** `NETC_OK` on success. Caller must free `*out` with `netc_dict_free_blob()`.

---

### `netc_dict_load`

```c
netc_result_t netc_dict_load(const void *data, size_t size, netc_dict_t **out);
```

Deserialize a dictionary from a blob previously produced by `netc_dict_save`. Validates the embedded CRC32.

**Returns:**
- `NETC_OK` — dict loaded; `*out` is non-NULL, caller owns it.
- `NETC_ERR_DICT_INVALID` — bad magic or corrupt data.
- `NETC_ERR_VERSION` — format version mismatch.
- `NETC_ERR_CORRUPT` — CRC32 mismatch.
- `NETC_ERR_NOMEM` — allocation failure.

---

### `netc_dict_free`

```c
void netc_dict_free(netc_dict_t *dict);
```

Free a dictionary returned by `netc_dict_train` or `netc_dict_load`. Safe to call with `NULL`.

---

### `netc_dict_free_blob`

```c
void netc_dict_free_blob(void *blob);
```

Free a binary blob returned by `netc_dict_save`. Safe to call with `NULL`.

---

### `netc_dict_model_id`

```c
uint8_t netc_dict_model_id(const netc_dict_t *dict);
```

Return the `model_id` (1–254) embedded in the dictionary. Returns 0 if `dict` is `NULL`.

---

## 7. Compression

### `netc_compress`

```c
netc_result_t netc_compress(
    netc_ctx_t *ctx,
    const void *src,
    size_t      src_size,
    void       *dst,
    size_t      dst_cap,
    size_t     *dst_size
);
```

Compress one packet using the stateful context.

**Parameters:**
- `ctx` — Compression context (must not be `NULL`).
- `src` — Input packet bytes.
- `src_size` — Input size. Must be ≤ `NETC_MAX_PACKET_SIZE`.
- `dst` — Output buffer.
- `dst_cap` — Output buffer capacity. Must be ≥ `netc_compress_bound(src_size)`.
- `dst_size` — On success, receives the number of output bytes.

**Returns:**
- `NETC_OK` — `dst[0..*dst_size)` contains the compressed packet.
- `NETC_ERR_CTX_NULL` — `ctx` is `NULL`.
- `NETC_ERR_TOOBIG` — `src_size > NETC_MAX_PACKET_SIZE`.
- `NETC_ERR_BUF_SMALL` — `dst_cap` too small.
- `NETC_ERR_NOMEM` — allocation failure.

**Passthrough guarantee:** If compression would expand the data, the original bytes are emitted verbatim with `NETC_PKT_FLAG_PASSTHRU`. Output is always ≤ `src_size + NETC_HEADER_SIZE`.

**On error:** `dst` is not modified; context state is unchanged.

---

### `netc_decompress`

```c
netc_result_t netc_decompress(
    netc_ctx_t *ctx,
    const void *src,
    size_t      src_size,
    void       *dst,
    size_t      dst_cap,
    size_t     *dst_size
);
```

Decompress one packet using the stateful context.

**Parameters:**
- `ctx` — Decompression context (separate instance from the compressor context).
- `src` — Compressed input (as produced by `netc_compress`).
- `src_size` — Compressed input size (including header).
- `dst` — Output buffer.
- `dst_cap` — Must be ≥ `NETC_MAX_PACKET_SIZE` to handle any valid packet.
- `dst_size` — On success, receives the decompressed size.

**Returns:**
- `NETC_OK` — decompression succeeded.
- `NETC_ERR_CORRUPT` — malformed or truncated data.
- `NETC_ERR_BUF_SMALL` — `dst_cap` < `original_size` in header.
- `NETC_ERR_VERSION` — `model_id` in header does not match dictionary.

**Security:** The decompressor enforces all bounds strictly. It never writes beyond `dst_cap`. Invalid ANS states and truncated bitstreams return `NETC_ERR_CORRUPT`.

---

### `netc_compress_stateless`

```c
netc_result_t netc_compress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size
);
```

Compress one independent packet. No context state is accumulated. Each call is self-contained; suitable for UDP or any out-of-order / lossy medium.

**Parameters:** Same as `netc_compress`, but `dict` replaces `ctx`. `dict` must not be `NULL`.

---

### `netc_decompress_stateless`

```c
netc_result_t netc_decompress_stateless(
    const netc_dict_t *dict,
    const void        *src,
    size_t             src_size,
    void              *dst,
    size_t             dst_cap,
    size_t            *dst_size
);
```

Decompress one independent packet. `dict` must not be `NULL`.

---

## 8. Utility

### `netc_strerror`

```c
const char *netc_strerror(netc_result_t result);
```

Return a human-readable string for a result code (e.g. `"NETC_ERR_CORRUPT"`). Always returns a non-NULL string.

---

### `netc_version`

```c
const char *netc_version(void);
```

Return the library version string (e.g. `"0.1.0"`). Always non-NULL.

---

### `netc_compress_bound`

```c
static inline size_t netc_compress_bound(size_t src_size);
```

Return the minimum output buffer size for compressing `src_size` bytes. Equivalent to `src_size + NETC_MAX_OVERHEAD`. Use this to size your `dst` buffer.

---

## 9. Error Handling

All functions that return `netc_result_t` follow these conventions:

- `NETC_OK (0)` — success.
- Negative values — error. The specific code identifies the failure mode.
- On error, output buffers are **never modified**.
- On error, context state is **never modified** (except resource exhaustion causing `NETC_ERR_NOMEM`).

**Recommended pattern:**

```c
netc_result_t r = netc_compress(ctx, src, src_size, dst, dst_cap, &dst_size);
if (r != NETC_OK) {
    fprintf(stderr, "compress failed: %s\n", netc_strerror(r));
    // handle error — ctx state is still valid
}
```

---

## 10. Thread Safety

| Object | Thread Safety |
|--------|--------------|
| `netc_dict_t *` | **Thread-safe for concurrent reads.** Multiple `netc_ctx_t` instances may share the same dict from different threads without synchronization. |
| `netc_ctx_t *` | **NOT thread-safe.** One context per connection per thread. Do not share a context across threads. |
| `netc_compress_stateless` | **Re-entrant** — may be called concurrently from multiple threads with different dict/src/dst arguments. |
| `netc_dict_train` | **Not thread-safe** — do not call concurrently for the same `out_dict`. |

---

## Complete Example — TCP Stateful Mode

```c
#include "netc.h"
#include <assert.h>
#include <string.h>

void example_tcp(void) {
    /* --- One-time setup --- */
    const uint8_t *pkts[50000];
    size_t         lens[50000];
    /* ... fill from your network capture ... */

    netc_dict_t  *dict = NULL;
    netc_result_t r    = netc_dict_train(pkts, lens, 50000, 1, &dict);
    assert(r == NETC_OK);

    /* Save to disk for reuse */
    void  *blob      = NULL;
    size_t blob_size = 0;
    netc_dict_save(dict, &blob, &blob_size);
    /* write blob to file ... */
    netc_dict_free_blob(blob);

    /* --- Per-connection setup --- */
    netc_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.flags = NETC_CFG_FLAG_STATEFUL | NETC_CFG_FLAG_DELTA;

    netc_ctx_t *enc = netc_ctx_create(dict, &cfg);
    netc_ctx_t *dec = netc_ctx_create(dict, &cfg);
    assert(enc && dec);

    /* --- Hot path: compress each outgoing packet --- */
    uint8_t src[512] = { /* ... your packet ... */ };
    size_t  src_len  = 256;

    uint8_t dst[512 + NETC_MAX_OVERHEAD];
    size_t  dst_len = 0;

    r = netc_compress(enc, src, src_len, dst, sizeof(dst), &dst_len);
    assert(r == NETC_OK);
    /* send dst[0..dst_len) over the wire */

    /* --- Hot path: decompress each incoming packet --- */
    uint8_t out[NETC_MAX_PACKET_SIZE];
    size_t  out_len = 0;

    r = netc_decompress(dec, dst, dst_len, out, sizeof(out), &out_len);
    assert(r == NETC_OK);
    assert(out_len == src_len);
    assert(memcmp(src, out, src_len) == 0);

    /* --- Teardown --- */
    netc_ctx_destroy(enc);
    netc_ctx_destroy(dec);
    netc_dict_free(dict);
}
```

## Complete Example — UDP Stateless Mode

```c
#include "netc.h"
#include <assert.h>

void example_udp(netc_dict_t *dict) {
    uint8_t src[128] = { /* ... */ };
    size_t  src_len  = 128;

    uint8_t comp[128 + NETC_MAX_OVERHEAD];
    size_t  comp_len = 0;

    /* Compress — no context needed */
    netc_result_t r = netc_compress_stateless(dict, src, src_len,
                                               comp, sizeof(comp), &comp_len);
    assert(r == NETC_OK);

    /* Decompress — also no context */
    uint8_t out[NETC_MAX_PACKET_SIZE];
    size_t  out_len = 0;
    r = netc_decompress_stateless(dict, comp, comp_len,
                                   out, sizeof(out), &out_len);
    assert(r == NETC_OK);
    assert(out_len == src_len);
}
```
