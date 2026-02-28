/**
 * netc_dict.c — Dictionary training, serialization, and management.
 *
 * Phase 2: Real byte frequency counting per context bucket, ANS probability
 * normalization, tANS table construction, and full blob serialization.
 *
 * Serialized blob layout (version 4):
 *   [0..3]   magic       (uint32 LE)  = NETC_DICT_MAGIC
 *   [4]      version     (uint8)      = NETC_DICT_VERSION (4)
 *   [5]      model_id    (uint8)
 *   [6]      ctx_count   (uint8)      = NETC_CTX_COUNT (16)
 *   [7]      dict_flags  (uint8)      = NETC_DICT_FLAG_* bitmask
 *   [8..]    unigram freq tables: NETC_CTX_COUNT × 256 × uint16 LE
 *            = 16 × 512 = 8192 bytes
 *   [8200..] bigram freq tables: NETC_CTX_COUNT × NETC_BIGRAM_CTX_COUNT × 256 × uint16 LE
 *            = 16 × 4 × 512 = 32768 bytes
 *   IF NETC_DICT_FLAG_LZP set:
 *     [40968..40971] lzp_ht_size  (uint32 LE) = NETC_LZP_HT_SIZE (131072)
 *     [40972..]      LZP entries  (value + valid, 2B each) × lzp_ht_size
 *   [last 4] checksum (uint32 LE, CRC32 of all preceding bytes)
 *
 * Base blob size (no LZP): 8 + 8192 + 32768 + 4 = 40972 bytes.
 * With LZP: 40968 + 4 + 131072*2 + 4 = 303120 bytes.
 */

#include "netc_internal.h"
#include "../util/netc_crc32.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Blob layout constants
 * ========================================================================= */

/* Blob header: magic(4) + version(1) + model_id(1) + ctx_count(1) + dict_flags(1) = 8 bytes */
#define DICT_HEADER_SIZE      8U
/* Unigram frequency table section: NETC_CTX_COUNT buckets × 256 × sizeof(uint16) */
#define DICT_FREQ_BYTES       (NETC_CTX_COUNT * NETC_TANS_SYMBOLS * 2U)  /* 16*512=8192 */
/* Bigram frequency table section: NETC_CTX_COUNT × NETC_BIGRAM_CTX_COUNT × 256 × sizeof(uint16) */
#define DICT_BIGRAM_BYTES     (NETC_CTX_COUNT * NETC_BIGRAM_CTX_COUNT * NETC_TANS_SYMBOLS * 2U) /* 16*4*512=32768 */
/* Base size before optional LZP section and checksum */
#define DICT_BASE_SIZE        (DICT_HEADER_SIZE + DICT_FREQ_BYTES + DICT_BIGRAM_BYTES) /* 40968 */
#define DICT_BIGRAM_OFF       (DICT_HEADER_SIZE + DICT_FREQ_BYTES)  /* 8200 */
/* v3 (no LZP) blob size: base + checksum */
#define DICT_V3_BLOB_SIZE     (DICT_BASE_SIZE + 4U)  /* 40972 */
/* LZP section: 4B lzp_ht_size + entries (2 bytes each) */
#define DICT_LZP_ENTRY_BYTES  2U
#define DICT_LZP_SECTION_SIZE (4U + NETC_LZP_HT_SIZE * DICT_LZP_ENTRY_BYTES)  /* 262148 */
/* v4 with LZP blob size: base + lzp_section + checksum */
#define DICT_V4_LZP_BLOB_SIZE (DICT_BASE_SIZE + DICT_LZP_SECTION_SIZE + 4U)  /* 303120 */

/* Compute blob size from dict_flags */
static size_t dict_blob_size(uint8_t dict_flags) {
    size_t sz = DICT_BASE_SIZE;
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

    /* --- Phase 2: accumulate byte frequencies per context bucket --- */
    uint64_t raw[NETC_CTX_COUNT][NETC_TANS_SYMBOLS];
    uint64_t totals[NETC_CTX_COUNT];
    memset(raw,    0, sizeof(raw));
    memset(totals, 0, sizeof(totals));

    /* Bigram raw counts: [bucket][bigram_class][symbol] */
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
            raw[bucket][sym]++;
            totals[bucket]++;
            /* Bigram: accumulate (prev_byte, curr_byte) counts.
             * For i==0 use 0x00 as the implicit "start of packet" previous byte. */
            uint8_t prev = (i > 0) ? pkt[i - 1] : 0x00u;
            uint32_t bclass = netc_bigram_class(prev);
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
                uint32_t bclass = netc_bigram_class(prev);
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
    size_t blob_sz = dict_blob_size(d->dict_flags);
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

    /* Serialize unigram frequency tables */
    size_t off = 8;
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
            netc_write_u16_le(tmp_blob + off, d->tables[b].freq.freq[s]);
            off += 2;
        }
    }

    /* Serialize bigram frequency sub-tables */
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

    size_t blob_sz = dict_blob_size(dict->dict_flags);
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
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
            netc_write_u16_le(blob + off, dict->tables[b].freq.freq[s]);
            off += 2;
        }
    }

    /* Serialize bigram sub-tables */
    for (uint32_t b = 0; b < NETC_CTX_COUNT; b++) {
        for (uint32_t c = 0; c < NETC_BIGRAM_CTX_COUNT; c++) {
            for (uint32_t s = 0; s < NETC_TANS_SYMBOLS; s++) {
                netc_write_u16_le(blob + off, dict->bigram_tables[b][c].freq.freq[s]);
                off += 2;
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
    /* Minimum blob size is DICT_V3_BLOB_SIZE (v3 without LZP) */
    if (NETC_UNLIKELY(size < DICT_V3_BLOB_SIZE)) {
        return NETC_ERR_DICT_INVALID;
    }

    const uint8_t *b = (const uint8_t *)data;

    uint32_t magic = netc_read_u32_le(b + 0);
    if (NETC_UNLIKELY(magic != NETC_DICT_MAGIC)) {
        return NETC_ERR_DICT_INVALID;
    }

    uint8_t version = b[4];
    /* Accept v3 and v4 dictionaries */
    if (NETC_UNLIKELY(version < 3 || version > NETC_DICT_VERSION)) {
        return NETC_ERR_VERSION;
    }
    uint8_t stored_ctx_count = b[6];
    if (NETC_UNLIKELY(stored_ctx_count != (uint8_t)NETC_CTX_COUNT)) {
        return NETC_ERR_VERSION;
    }

    /* Read dict_flags (byte 7: _pad in v3, dict_flags in v4) */
    uint8_t dflags = b[7];

    /* Compute expected blob size and validate */
    size_t expected_sz = dict_blob_size(dflags);
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

    /* Deserialize unigram frequency tables and rebuild tANS decode/encode tables */
    size_t off = 8;
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

    /* Deserialize bigram sub-tables (v0.3+) */
    for (uint32_t bucket = 0; bucket < NETC_CTX_COUNT; bucket++) {
        for (uint32_t c = 0; c < NETC_BIGRAM_CTX_COUNT; c++) {
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
