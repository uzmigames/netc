/**
 * netc_dict.c — Dictionary training, serialization, and management.
 *
 * Phase 2: Real byte frequency counting per context bucket, ANS probability
 * normalization, tANS table construction, and full blob serialization.
 *
 * Serialized blob layout (version 5):
 *   [0..3]     magic           (uint32 LE)  = NETC_DICT_MAGIC
 *   [4]        version         (uint8)      = 5
 *   [5]        model_id        (uint8)
 *   [6]        ctx_count       (uint8)      = NETC_CTX_COUNT (16)
 *   [7]        dict_flags      (uint8)      = NETC_DICT_FLAG_* bitmask
 *   [8..263]   bigram_class_map[256]         (NEW in v5)
 *   [264..]    unigram freq tables: 16 × 256 × uint16 LE = 8192 bytes
 *   [8456..]   bigram freq tables: 16 × 8 × 256 × uint16 LE = 65536 bytes
 *   IF NETC_DICT_FLAG_LZP set:
 *     [73992..73995] lzp_ht_size (uint32 LE) = NETC_LZP_HT_SIZE (131072)
 *     [73996..]      LZP entries (2B each) × lzp_ht_size
 *   [last 4]   checksum (uint32 LE, CRC32 of all preceding bytes)
 *
 * v5 base (no LZP): 8 + 256 + 8192 + 65536 + 4 = 73996 bytes.
 * v5 with LZP: 73992 + 4 + 131072*2 + 4 = 336144 bytes.
 *
 * v4 blob layout (backward-compat loading):
 *   [0..7]     header (same as v5, version=4)
 *   [8..]      unigram freq: 16 × 256 × 2 = 8192B
 *   [8200..]   bigram freq: 16 × 4 × 256 × 2 = 32768B (4 classes, not 8)
 *   [40968..]  LZP section (if present)
 *   [last 4]   checksum
 */

#include "netc_internal.h"
#include "../util/netc_crc32.h"
#include "../simd/netc_simd.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Blob layout constants
 * ========================================================================= */

/* Blob header: magic(4) + version(1) + model_id(1) + ctx_count(1) + dict_flags(1) = 8 bytes */
#define DICT_HEADER_SIZE      8U
/* Unigram frequency table section: NETC_CTX_COUNT buckets × 256 × sizeof(uint16) */
#define DICT_FREQ_BYTES       (NETC_CTX_COUNT * NETC_TANS_SYMBOLS * 2U)  /* 16*512=8192 */
/* LZP section: 4B lzp_ht_size + entries (2 bytes each) */
#define DICT_LZP_ENTRY_BYTES  2U
#define DICT_LZP_SECTION_SIZE (4U + NETC_LZP_HT_SIZE * DICT_LZP_ENTRY_BYTES)  /* 262148 */

/* ----- v4 layout constants (backward-compat) ----- */
/* Bigram freq: 16 × 4 × 256 × 2 = 32768 */
#define DICT_V4_BIGRAM_BYTES  (NETC_CTX_COUNT * NETC_BIGRAM_CTX_COUNT_V4 * NETC_TANS_SYMBOLS * 2U)
#define DICT_V4_BASE_SIZE     (DICT_HEADER_SIZE + DICT_FREQ_BYTES + DICT_V4_BIGRAM_BYTES)  /* 40968 */
/* v3 (no LZP) minimum blob: base + checksum = 40972 */
#define DICT_V3_BLOB_SIZE     (DICT_V4_BASE_SIZE + 4U)

/* ----- v5 layout constants ----- */
/* Class map: 256 bytes immediately after header */
#define DICT_V5_CLASSMAP_SIZE 256U
/* Bigram freq: 16 × 8 × 256 × 2 = 65536 */
#define DICT_V5_BIGRAM_BYTES  (NETC_CTX_COUNT * NETC_BIGRAM_CTX_COUNT * NETC_TANS_SYMBOLS * 2U)
/* v5 base: header(8) + class_map(256) + unigram(8192) + bigram(65536) = 73992 */
#define DICT_V5_BASE_SIZE     (DICT_HEADER_SIZE + DICT_V5_CLASSMAP_SIZE + DICT_FREQ_BYTES + DICT_V5_BIGRAM_BYTES)

/* Compute blob size from version + dict_flags */
static size_t dict_blob_size_v(uint8_t version, uint8_t dict_flags) {
    size_t sz;
    if (version >= 5U) {
        sz = DICT_V5_BASE_SIZE;
    } else {
        sz = DICT_V4_BASE_SIZE;
    }
    if (dict_flags & NETC_DICT_FLAG_LZP) sz += DICT_LZP_SECTION_SIZE;
    sz += 4U; /* checksum */
    return sz;
}

/* =========================================================================
 * dict_checksum — CRC32 of the blob excluding the trailing checksum field
 * ========================================================================= */

static uint32_t dict_blob_checksum(const uint8_t *blob, size_t blob_size) {
    return netc_crc32(blob, blob_size - 4U);
}

/* =========================================================================
 * freq_normalize — scale raw counts to sum exactly to TABLE_SIZE (4096).
 *
 * Algorithm (from Zstd/FSE):
 *   1. If all counts are 0, assign uniform distribution.
 *   2. Scale each count proportionally: scaled[s] = max(1, raw[s] * TABLE_SIZE / total).
 *   3. Correct rounding error by adjusting the largest symbol.
 * ========================================================================= */

static void freq_normalize(
    const uint64_t raw[NETC_TANS_SYMBOLS],
    uint64_t        total,
    uint16_t        out[NETC_TANS_SYMBOLS])
{
    /* Laplace smoothing: add 1 to every symbol's count.
     * This ensures all 256 symbols have freq >= 1 after normalization,
     * allowing any byte to be entropy-coded regardless of training data.
     * The smoothed total is total + NETC_TANS_SYMBOLS. */
    uint64_t smoothed_total = total + (uint64_t)NETC_TANS_SYMBOLS;

    uint32_t table_sum = 0;
    int      max_sym   = 0;
    uint32_t max_val   = 0;

    for (int s = 0; s < (int)NETC_TANS_SYMBOLS; s++) {
        uint64_t smoothed = raw[s] + 1u;
        uint64_t scaled   = (smoothed * (uint64_t)NETC_TANS_TABLE_SIZE) / smoothed_total;
        if (scaled == 0) scaled = 1;
        if (scaled > 0xFFFFU) scaled = 0xFFFFU;
        out[s]     = (uint16_t)scaled;
        table_sum += (uint32_t)scaled;
        if (out[s] > max_val) {
            max_val = out[s];
            max_sym = s;
        }
    }

    /* Adjust largest symbol to correct rounding drift */
    if (table_sum < NETC_TANS_TABLE_SIZE) {
        out[max_sym] = (uint16_t)(out[max_sym] + (uint16_t)(NETC_TANS_TABLE_SIZE - table_sum));
    } else if (table_sum > NETC_TANS_TABLE_SIZE) {
        uint32_t excess = table_sum - NETC_TANS_TABLE_SIZE;
        if (out[max_sym] > (uint16_t)excess + 1U) {
            out[max_sym] = (uint16_t)(out[max_sym] - (uint16_t)excess);
        } else {
            for (int s = 0; s < (int)NETC_TANS_SYMBOLS && excess > 0; s++) {
                if (out[s] > 1) { out[s]--; excess--; }
            }
        }
    }
}

/* =========================================================================
 * netc_dict_train
 * ========================================================================= */

netc_result_t netc_dict_train(
    const uint8_t * const *packets,
    const size_t          *sizes,
    size_t                 count,
    uint8_t                model_id,
    netc_dict_t          **out_dict)
{
    if (NETC_UNLIKELY(out_dict == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(model_id == 0 || model_id == 255)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(count > 0 && (packets == NULL || sizes == NULL))) {
        return NETC_ERR_INVALID_ARG;
    }

    netc_dict_t *d = (netc_dict_t *)calloc(1, sizeof(netc_dict_t));
    if (NETC_UNLIKELY(d == NULL)) {
        return NETC_ERR_NOMEM;
    }

    d->magic      = NETC_DICT_MAGIC;
    d->version    = NETC_DICT_VERSION;
    d->model_id   = model_id;
    d->ctx_count  = (uint8_t)NETC_CTX_COUNT;
    d->dict_flags = 0;
    d->lzp_table  = NULL;
    d->bigram_class_count = NETC_BIGRAM_CTX_COUNT;  /* 8 */

    /* --- Phase 2a: accumulate unigram byte frequencies per context bucket --- */
    uint64_t raw[NETC_CTX_COUNT][NETC_TANS_SYMBOLS];
    uint64_t totals[NETC_CTX_COUNT];
    memset(raw,    0, sizeof(raw));
    memset(totals, 0, sizeof(totals));

    /* Bucket boundary table: bucket b covers byte positions [bucket_start[b], bucket_start[b+1]).
     * Mirrors the netc_ctx_bucket() LUT thresholds exactly. */
    static const uint32_t bucket_end[NETC_CTX_COUNT] = {
           8,   16,   24,   32,   48,   64,   96,  128,
         192,  256,  384,  512, 1024, 4096, 16384, 65536
    };

    /* Use SIMD-dispatched freq_count for each contiguous bucket range within the packet.
     * tmp_freq accumulates into a uint32_t[256] scratch buffer (fits in L1 cache);
     * results are promoted to the uint64_t raw table after each bucket. */
    netc_simd_ops_t simd_ops;
    netc_simd_ops_init(&simd_ops, NETC_SIMD_LEVEL_AUTO);

    uint32_t tmp_freq[NETC_TANS_SYMBOLS];

    for (size_t p = 0; p < count; p++) {
        if (packets[p] == NULL || sizes[p] == 0) continue;
        size_t pkt_size = sizes[p];
        if (pkt_size > NETC_MAX_PACKET_SIZE) pkt_size = NETC_MAX_PACKET_SIZE;
        const uint8_t *pkt = packets[p];

        uint32_t seg_start = 0;
        for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
            if (seg_start >= (uint32_t)pkt_size) break;
            uint32_t seg_end = bucket_end[b];
            if (seg_end > (uint32_t)pkt_size) seg_end = (uint32_t)pkt_size;
            uint32_t seg_len = seg_end - seg_start;

            memset(tmp_freq, 0, sizeof(tmp_freq));
            simd_ops.freq_count(pkt + seg_start, (size_t)seg_len, tmp_freq);
            for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
                raw[b][s] += tmp_freq[s];
            }
            totals[b] += seg_len;
            seg_start = seg_end;
        }
    }

    /* --- Phase 2b: build bigram class_map via frequency-based clustering --- */
    /* For each prev_byte value (0-255), compute the most-frequent next-symbol
     * (aggregated across all buckets). Sort prev_bytes by their most-frequent
     * symbol index, then divide into 8 equal groups of 32. */
    {
        /* cond_peak[prev] = the symbol that occurs most often after prev (all buckets) */
        uint16_t cond_peak[256];
        /* Per prev_byte, accumulate total next-symbol counts across all buckets */
        uint64_t *cond_counts = (uint64_t *)calloc(256, 256 * sizeof(uint64_t));
        if (NETC_UNLIKELY(cond_counts == NULL)) {
            free(d);
            return NETC_ERR_NOMEM;
        }

        for (size_t p = 0; p < count; p++) {
            if (packets[p] == NULL || sizes[p] == 0) continue;
            size_t pkt_size = sizes[p];
            if (pkt_size > NETC_MAX_PACKET_SIZE) pkt_size = NETC_MAX_PACKET_SIZE;
            const uint8_t *pkt = packets[p];
            for (size_t i = 0; i < pkt_size; i++) {
                uint8_t prev = (i > 0) ? pkt[i - 1] : 0x00u;
                cond_counts[(size_t)prev * 256 + pkt[i]]++;
            }
        }

        /* Find the peak symbol for each prev_byte */
        for (uint32_t prev = 0; prev < 256; prev++) {
            uint64_t best = 0;
            uint16_t best_sym = 0;
            for (uint32_t s = 0; s < 256; s++) {
                uint64_t c = cond_counts[(size_t)prev * 256 + s];
                if (c > best) { best = c; best_sym = (uint16_t)s; }
            }
            cond_peak[prev] = best_sym;
        }
        free(cond_counts);

        /* Sort prev_byte indices by their peak symbol to cluster similar contexts.
         * Simple insertion sort on 256 elements (indices 0-255). */
        uint16_t sorted_prev[256];
        for (uint32_t i = 0; i < 256; i++) sorted_prev[i] = (uint16_t)i;
        for (uint32_t i = 1; i < 256; i++) {
            uint16_t key = sorted_prev[i];
            uint16_t key_peak = cond_peak[key];
            int j = (int)i - 1;
            while (j >= 0 && cond_peak[sorted_prev[j]] > key_peak) {
                sorted_prev[j + 1] = sorted_prev[j];
                j--;
            }
            sorted_prev[j + 1] = key;
        }

        /* Divide sorted prev_bytes into 8 equal groups of 32 */
        for (uint32_t g = 0; g < NETC_BIGRAM_CTX_COUNT; g++) {
            for (uint32_t k = 0; k < 32; k++) {
                uint16_t prev_idx = sorted_prev[g * 32 + k];
                d->bigram_class_map[prev_idx] = (uint8_t)g;
            }
        }
    }

    /* --- Phase 2c: accumulate bigram frequencies using trained class_map --- */
    /* Bigram raw counts: [bucket][bigram_class][symbol] — now 8 classes */
    uint64_t (*bgram_raw)[NETC_BIGRAM_CTX_COUNT][NETC_TANS_SYMBOLS] =
        (uint64_t (*)[NETC_BIGRAM_CTX_COUNT][NETC_TANS_SYMBOLS])
        calloc(NETC_CTX_COUNT, sizeof((*bgram_raw)));
    uint64_t bgram_totals[NETC_CTX_COUNT][NETC_BIGRAM_CTX_COUNT];
    if (NETC_UNLIKELY(bgram_raw == NULL)) {
        free(d);
        return NETC_ERR_NOMEM;
    }
    memset(bgram_totals, 0, sizeof(bgram_totals));

    for (size_t p = 0; p < count; p++) {
        if (packets[p] == NULL || sizes[p] == 0) continue;
        size_t pkt_size = sizes[p];
        if (pkt_size > NETC_MAX_PACKET_SIZE) pkt_size = NETC_MAX_PACKET_SIZE;
        const uint8_t *pkt = packets[p];
        for (size_t i = 0; i < pkt_size; i++) {
            uint32_t bucket = netc_ctx_bucket((uint32_t)i);
            uint8_t sym = pkt[i];
            /* Use trained class_map for bigram context */
            uint8_t prev = (i > 0) ? pkt[i - 1] : 0x00u;
            uint32_t bclass = netc_bigram_class(prev, d->bigram_class_map);
            bgram_raw[bucket][bclass][sym]++;
            bgram_totals[bucket][bclass]++;
        }
    }

    /* --- Normalize unigram frequencies and build tANS tables --- */
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        netc_freq_table_t ft;
        freq_normalize(raw[b], totals[b], ft.freq);
        if (netc_tans_build(&d->tables[b], &ft) != 0) {
            free(bgram_raw);
            free(d);
            return NETC_ERR_NOMEM; /* table build failure (should not happen) */
        }
    }

    /* --- Normalize bigram frequencies and build bigram tANS sub-tables --- */
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t c = 0; c < NETC_BIGRAM_CTX_COUNT; c++) {
            netc_freq_table_t ft;
            freq_normalize(bgram_raw[b][c], bgram_totals[b][c], ft.freq);
            if (netc_tans_build(&d->bigram_tables[b][c], &ft) != 0) {
                free(bgram_raw);
                free(d);
                return NETC_ERR_NOMEM;
            }
        }
    }
    free(bgram_raw);

    /* --- Phase 4: LZP hash table training (Boyer-Moore majority vote) --- */
    /* For each (prev_byte, position) context, find the most common byte.
     * Uses position-aware order-1 hashing: hash(prev_byte, byte_offset).
     * Boyer-Moore majority element algorithm: O(1) space per slot.
     *   - If current candidate matches: increment vote count
     *   - If vote count is zero: replace candidate, count = 1
     *   - Otherwise: decrement vote count (cancel one opposite vote)
     * After all training data, the candidate is the majority element if
     * one exists (>50% frequency at this hash slot). */
    {
        /* Training state: candidate + vote count per hash slot */
        typedef struct { uint8_t candidate; int16_t count; } lzp_vote_t;
        lzp_vote_t *votes = (lzp_vote_t *)calloc(NETC_LZP_HT_SIZE, sizeof(lzp_vote_t));
        /* Total hit count per slot (to compute validity threshold) */
        uint16_t *slot_total = (uint16_t *)calloc(NETC_LZP_HT_SIZE, sizeof(uint16_t));
        if (NETC_UNLIKELY(votes == NULL || slot_total == NULL)) {
            free(votes);
            free(slot_total);
            free(d);
            return NETC_ERR_NOMEM;
        }

        /* Pass 1: Boyer-Moore majority vote across all training packets */
        for (size_t p = 0; p < count; p++) {
            if (packets[p] == NULL || sizes[p] == 0) continue;
            size_t pkt_size = sizes[p];
            if (pkt_size > NETC_MAX_PACKET_SIZE) pkt_size = NETC_MAX_PACKET_SIZE;
            const uint8_t *pkt = packets[p];
            for (size_t i = 0; i < pkt_size; i++) {
                uint8_t  prev = (i > 0) ? pkt[i - 1] : 0x00u;
                uint32_t h = netc_lzp_hash(prev, (uint32_t)i);
                uint8_t  byte_val = pkt[i];
                if (slot_total[h] < 0xFFFFU) slot_total[h]++;
                if (votes[h].count == 0) {
                    votes[h].candidate = byte_val;
                    votes[h].count     = 1;
                } else if (votes[h].candidate == byte_val) {
                    if (votes[h].count < INT16_MAX) votes[h].count++;
                } else {
                    votes[h].count--;
                }
            }
        }

        /* Pass 2: verify candidates — count actual frequency of the majority candidate.
         * Boyer-Moore only guarantees majority if >50%; we verify and set valid
         * only when the candidate appears in >= 40% of slot occurrences (generous
         * threshold since even 40% hit rate saves significant bytes). */
        /* Reset counts for verification pass */
        uint16_t *hit_count = slot_total; /* reuse for hit counting */
        uint16_t *total_count = (uint16_t *)calloc(NETC_LZP_HT_SIZE, sizeof(uint16_t));
        if (NETC_UNLIKELY(total_count == NULL)) {
            free(votes);
            free(slot_total);
            free(d);
            return NETC_ERR_NOMEM;
        }
        memset(hit_count, 0, NETC_LZP_HT_SIZE * sizeof(uint16_t));

        for (size_t p = 0; p < count; p++) {
            if (packets[p] == NULL || sizes[p] == 0) continue;
            size_t pkt_size = sizes[p];
            if (pkt_size > NETC_MAX_PACKET_SIZE) pkt_size = NETC_MAX_PACKET_SIZE;
            const uint8_t *pkt = packets[p];
            for (size_t i = 0; i < pkt_size; i++) {
                uint8_t  prev = (i > 0) ? pkt[i - 1] : 0x00u;
                uint32_t h = netc_lzp_hash(prev, (uint32_t)i);
                if (total_count[h] < 0xFFFFU) total_count[h]++;
                if (pkt[i] == votes[h].candidate) {
                    if (hit_count[h] < 0xFFFFU) hit_count[h]++;
                }
            }
        }

        /* Allocate LZP table */
        d->lzp_table = (netc_lzp_entry_t *)calloc(NETC_LZP_HT_SIZE, sizeof(netc_lzp_entry_t));
        if (NETC_UNLIKELY(d->lzp_table == NULL)) {
            free(total_count);
            free(votes);
            free(d);
            return NETC_ERR_NOMEM;
        }

        /* Populate LZP table: valid only if hit_rate >= 40% and total >= 2 */
        for (uint32_t h = 0; h < NETC_LZP_HT_SIZE; h++) {
            if (total_count[h] >= 2 &&
                (uint32_t)hit_count[h] * 10u >= (uint32_t)total_count[h] * 4u) {
                d->lzp_table[h].value = votes[h].candidate;
                d->lzp_table[h].valid = 1;
            }
        }

        d->dict_flags |= NETC_DICT_FLAG_LZP;
        free(total_count);
        free(votes);
    }

    /* --- Phase 5: Retrain tANS tables on LZP-filtered data --- */
    /* When LZP is trained, the compressor will XOR each byte with its LZP
     * prediction before tANS encoding.  Correctly-predicted bytes become 0x00.
     * The tANS tables must match this post-filter distribution for optimal
     * compression.  Re-accumulate frequencies on LZP-filtered packets and
     * rebuild all unigram + bigram tANS tables. */
    if (d->dict_flags & NETC_DICT_FLAG_LZP) {
        /* Reset accumulators */
        memset(raw,    0, sizeof(raw));
        memset(totals, 0, sizeof(totals));

        uint64_t (*bgram_raw2)[NETC_BIGRAM_CTX_COUNT][NETC_TANS_SYMBOLS] =
            (uint64_t (*)[NETC_BIGRAM_CTX_COUNT][NETC_TANS_SYMBOLS])
            calloc(NETC_CTX_COUNT, sizeof(*bgram_raw2));
        uint64_t bgram_totals2[NETC_CTX_COUNT][NETC_BIGRAM_CTX_COUNT];
        if (NETC_UNLIKELY(bgram_raw2 == NULL)) {
            free(d->lzp_table);
            free(d);
            return NETC_ERR_NOMEM;
        }
        memset(bgram_totals2, 0, sizeof(bgram_totals2));

        /* Temp buffer for one filtered packet */
        uint8_t *filt_buf = (uint8_t *)malloc(NETC_MAX_PACKET_SIZE);
        if (NETC_UNLIKELY(filt_buf == NULL)) {
            free(bgram_raw2);
            free(d->lzp_table);
            free(d);
            return NETC_ERR_NOMEM;
        }

        for (size_t p = 0; p < count; p++) {
            if (packets[p] == NULL || sizes[p] == 0) continue;
            size_t pkt_size = sizes[p];
            if (pkt_size > NETC_MAX_PACKET_SIZE) pkt_size = NETC_MAX_PACKET_SIZE;

            /* Apply LZP XOR filter to this packet */
            netc_lzp_xor_filter(packets[p], pkt_size, d->lzp_table, filt_buf);

            /* Accumulate frequencies on filtered data */
            for (size_t i = 0; i < pkt_size; i++) {
                uint32_t bucket = netc_ctx_bucket((uint32_t)i);
                uint8_t sym = filt_buf[i];
                raw[bucket][sym]++;
                totals[bucket]++;
                uint8_t prev = (i > 0) ? filt_buf[i - 1] : 0x00u;
                uint32_t bclass = netc_bigram_class(prev, d->bigram_class_map);
                bgram_raw2[bucket][bclass][sym]++;
                bgram_totals2[bucket][bclass]++;
            }
        }
        free(filt_buf);

        /* Rebuild unigram tANS tables from filtered frequencies */
        for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
            netc_freq_table_t ft;
            freq_normalize(raw[b], totals[b], ft.freq);
            netc_tans_build(&d->tables[b], &ft);
        }

        /* Rebuild bigram tANS tables from filtered frequencies */
        for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
            for (uint32_t c = 0; c < NETC_BIGRAM_CTX_COUNT; c++) {
                netc_freq_table_t ft;
                freq_normalize(bgram_raw2[b][c], bgram_totals2[b][c], ft.freq);
                netc_tans_build(&d->bigram_tables[b][c], &ft);
            }
        }
        free(bgram_raw2);
    }

    /* --- Compute checksum over the serialized blob --- */
    /* We compute the checksum from the blob representation for consistency
     * between train/save and load. Build the blob, compute, store. */
    size_t blob_sz = dict_blob_size_v(d->version, d->dict_flags);
    uint8_t *tmp_blob = (uint8_t *)malloc(blob_sz);
    if (NETC_UNLIKELY(tmp_blob == NULL)) {
        free(d->lzp_table);
        free(d);
        return NETC_ERR_NOMEM;
    }

    /* Serialize header */
    netc_write_u32_le(tmp_blob + 0, d->magic);
    tmp_blob[4] = d->version;
    tmp_blob[5] = d->model_id;
    tmp_blob[6] = d->ctx_count;
    tmp_blob[7] = d->dict_flags;

    size_t off = 8;

    /* v5: serialize bigram class map (256 bytes) */
    memcpy(tmp_blob + off, d->bigram_class_map, 256);
    off += 256;

    /* Serialize unigram frequency tables */
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
            netc_write_u16_le(tmp_blob + off, d->tables[b].freq.freq[s]);
            off += 2;
        }
    }

    /* Serialize bigram frequency sub-tables (8 classes in v5) */
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t c = 0; c < NETC_BIGRAM_CTX_COUNT; c++) {
            for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
                netc_write_u16_le(tmp_blob + off, d->bigram_tables[b][c].freq.freq[s]);
                off += 2;
            }
        }
    }

    /* Serialize LZP table (if present) */
    if (d->dict_flags & NETC_DICT_FLAG_LZP) {
        netc_write_u32_le(tmp_blob + off, NETC_LZP_HT_SIZE);
        off += 4;
        for (uint32_t h = 0; h < NETC_LZP_HT_SIZE; h++) {
            tmp_blob[off++] = d->lzp_table[h].value;
            tmp_blob[off++] = d->lzp_table[h].valid;
        }
    }

    /* Compute and store checksum */
    d->checksum = netc_crc32(tmp_blob, blob_sz - 4U);
    netc_write_u32_le(tmp_blob + off, d->checksum);
    free(tmp_blob);

    *out_dict = d;
    return NETC_OK;
}

/* =========================================================================
 * netc_dict_save — serialize to blob
 * ========================================================================= */

netc_result_t netc_dict_save(const netc_dict_t *dict, void **out, size_t *out_size) {
    if (NETC_UNLIKELY(dict == NULL || out == NULL || out_size == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }

    size_t blob_sz = dict_blob_size_v(dict->version, dict->dict_flags);
    uint8_t *blob = (uint8_t *)malloc(blob_sz);
    if (NETC_UNLIKELY(blob == NULL)) {
        return NETC_ERR_NOMEM;
    }

    netc_write_u32_le(blob + 0, dict->magic);
    blob[4] = dict->version;
    blob[5] = dict->model_id;
    blob[6] = dict->ctx_count;
    blob[7] = dict->dict_flags;

    size_t off = 8;

    /* v5: serialize bigram class map (256 bytes) */
    if (dict->version >= 5) {
        memcpy(blob + off, dict->bigram_class_map, 256);
        off += 256;
    }

    /* Serialize unigram frequency tables */
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
            netc_write_u16_le(blob + off, dict->tables[b].freq.freq[s]);
            off += 2;
        }
    }

    /* Serialize bigram sub-tables */
    {
        uint32_t n_classes = (dict->version >= 5) ? NETC_BIGRAM_CTX_COUNT : NETC_BIGRAM_CTX_COUNT_V4;
        for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
            for (uint32_t c = 0; c < n_classes; c++) {
                for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
                    netc_write_u16_le(blob + off, dict->bigram_tables[b][c].freq.freq[s]);
                    off += 2;
                }
            }
        }
    }

    /* Serialize LZP table (if present) */
    if (dict->dict_flags & NETC_DICT_FLAG_LZP) {
        netc_write_u32_le(blob + off, NETC_LZP_HT_SIZE);
        off += 4;
        for (uint32_t h = 0; h < NETC_LZP_HT_SIZE; h++) {
            blob[off++] = dict->lzp_table[h].value;
            blob[off++] = dict->lzp_table[h].valid;
        }
    }

    netc_write_u32_le(blob + off, dict->checksum);

    *out      = blob;
    *out_size = blob_sz;
    return NETC_OK;
}

/* =========================================================================
 * netc_dict_load — deserialize and validate blob
 * ========================================================================= */

netc_result_t netc_dict_load(const void *data, size_t size, netc_dict_t **out) {
    if (NETC_UNLIKELY(data == NULL || out == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    /* Minimum blob size is DICT_V3_BLOB_SIZE (v3/v4 without LZP) */
    if (NETC_UNLIKELY(size < DICT_V3_BLOB_SIZE)) {
        return NETC_ERR_DICT_INVALID;
    }

    const uint8_t *b = (const uint8_t *)data;

    uint32_t magic = netc_read_u32_le(b + 0);
    if (NETC_UNLIKELY(magic != NETC_DICT_MAGIC)) {
        return NETC_ERR_DICT_INVALID;
    }

    uint8_t version = b[4];
    /* Accept v3, v4, and v5 dictionaries */
    if (NETC_UNLIKELY(version < 3 || version > NETC_DICT_VERSION)) {
        return NETC_ERR_VERSION;
    }
    uint8_t stored_ctx_count = b[6];
    if (NETC_UNLIKELY(stored_ctx_count != (uint8_t)NETC_CTX_COUNT)) {
        return NETC_ERR_VERSION;
    }

    /* Read dict_flags (byte 7: _pad in v3, dict_flags in v4+) */
    uint8_t dflags = b[7];

    /* Compute expected blob size and validate */
    size_t expected_sz = dict_blob_size_v(version, dflags);
    if (NETC_UNLIKELY(size < expected_sz)) {
        return NETC_ERR_DICT_INVALID;
    }

    /* Validate checksum */
    uint32_t stored_cksum = netc_read_u32_le(b + expected_sz - 4U);
    uint32_t expected_cksum = dict_blob_checksum(b, expected_sz);
    if (NETC_UNLIKELY(stored_cksum != expected_cksum)) {
        return NETC_ERR_DICT_INVALID;
    }

    netc_dict_t *d = (netc_dict_t *)calloc(1, sizeof(netc_dict_t));
    if (NETC_UNLIKELY(d == NULL)) {
        return NETC_ERR_NOMEM;
    }

    d->magic      = magic;
    d->version    = version;
    d->model_id   = b[5];
    d->ctx_count  = b[6];
    d->dict_flags = dflags;
    d->lzp_table  = NULL;
    d->checksum   = stored_cksum;

    size_t off = 8;

    /* v5: read trained bigram class map (256 bytes) */
    if (version >= 5) {
        memcpy(d->bigram_class_map, b + off, 256);
        off += 256;
        d->bigram_class_count = NETC_BIGRAM_CTX_COUNT;  /* 8 */
    } else {
        /* v3/v4: build default class_map from prev_byte >> 6.
         * Maps to classes 0-3 only; classes 4-7 unused. */
        for (uint32_t i = 0; i < 256; i++) {
            d->bigram_class_map[i] = (uint8_t)(i >> 6);
        }
        d->bigram_class_count = NETC_BIGRAM_CTX_COUNT_V4;  /* 4 */
    }

    /* Deserialize unigram frequency tables and rebuild tANS decode/encode tables */
    for (uint32_t bucket = 0; bucket < NETC_CTX_COUNT; bucket++) {
        netc_freq_table_t ft;
        for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
            ft.freq[s] = netc_read_u16_le(b + off);
            off += 2;
        }
        if (netc_tans_build(&d->tables[bucket], &ft) != 0) {
            free(d);
            return NETC_ERR_DICT_INVALID;
        }
    }

    /* Deserialize bigram sub-tables */
    {
        uint32_t n_classes = (version >= 5) ? NETC_BIGRAM_CTX_COUNT : NETC_BIGRAM_CTX_COUNT_V4;
        for (uint32_t bucket = 0; bucket < NETC_CTX_COUNT; bucket++) {
            for (uint32_t c = 0; c < n_classes; c++) {
                netc_freq_table_t ft;
                for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
                    ft.freq[s] = netc_read_u16_le(b + off);
                    off += 2;
                }
                if (netc_tans_build(&d->bigram_tables[bucket][c], &ft) != 0) {
                    free(d);
                    return NETC_ERR_DICT_INVALID;
                }
            }
        }
    }

    /* Deserialize LZP table (v4+ with DICT_FLAG_LZP) */
    if (dflags & NETC_DICT_FLAG_LZP) {
        uint32_t lzp_ht_size = netc_read_u32_le(b + off);
        off += 4;
        if (NETC_UNLIKELY(lzp_ht_size > NETC_LZP_HT_SIZE)) {
            free(d);
            return NETC_ERR_DICT_INVALID;
        }
        d->lzp_table = (netc_lzp_entry_t *)calloc(NETC_LZP_HT_SIZE, sizeof(netc_lzp_entry_t));
        if (NETC_UNLIKELY(d->lzp_table == NULL)) {
            free(d);
            return NETC_ERR_NOMEM;
        }
        for (uint32_t h = 0; h < lzp_ht_size; h++) {
            d->lzp_table[h].value = b[off++];
            d->lzp_table[h].valid = b[off++];
        }
    }

    *out = d;
    return NETC_OK;
}

/* =========================================================================
 * netc_dict_free / netc_dict_free_blob / netc_dict_model_id
 * ========================================================================= */

void netc_dict_free(netc_dict_t *dict) {
    if (dict != NULL) {
        free(dict->lzp_table);
    }
    free(dict);
}

void netc_dict_free_blob(void *blob) {
    free(blob);
}

uint8_t netc_dict_model_id(const netc_dict_t *dict) {
    if (dict == NULL) {
        return 0;
    }
    return dict->model_id;
}
