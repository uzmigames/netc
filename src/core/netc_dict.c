/**
 * netc_dict.c — Dictionary training, serialization, and management.
 *
 * Phase 2: Real byte frequency counting per context bucket, ANS probability
 * normalization, tANS table construction, and full blob serialization.
 *
 * Serialized blob layout (version 3):
 *   [0..3]   magic       (uint32 LE)  = NETC_DICT_MAGIC
 *   [4]      version     (uint8)      = NETC_DICT_VERSION (3)
 *   [5]      model_id    (uint8)
 *   [6]      ctx_count   (uint8)      = NETC_CTX_COUNT (16)
 *   [7]      _pad        (uint8)      = 0
 *   [8..]    unigram freq tables: NETC_CTX_COUNT × 256 × uint16 LE
 *            = 16 × 512 = 8192 bytes
 *   [8200..] bigram freq tables: NETC_CTX_COUNT × NETC_BIGRAM_CTX_COUNT × 256 × uint16 LE
 *            = 16 × 4 × 512 = 32768 bytes
 *   [last 4] checksum (uint32 LE, CRC32 of all preceding bytes)
 *
 * Total blob size: 8 + 8192 + 32768 + 4 = 40972 bytes.
 */

#include "netc_internal.h"
#include "../util/netc_crc32.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Blob layout constants
 * ========================================================================= */

/* Blob header: magic(4) + version(1) + model_id(1) + ctx_count(1) + pad(1) = 8 bytes */
#define DICT_HEADER_SIZE      8U
/* Unigram frequency table section: NETC_CTX_COUNT buckets × 256 × sizeof(uint16) */
#define DICT_FREQ_BYTES       (NETC_CTX_COUNT * NETC_TANS_SYMBOLS * 2U)  /* 16*512=8192 */
/* Bigram frequency table section: NETC_CTX_COUNT × NETC_BIGRAM_CTX_COUNT × 256 × sizeof(uint16) */
#define DICT_BIGRAM_BYTES     (NETC_CTX_COUNT * NETC_BIGRAM_CTX_COUNT * NETC_TANS_SYMBOLS * 2U) /* 16*4*512=32768 */
#define DICT_BLOB_SIZE        (DICT_HEADER_SIZE + DICT_FREQ_BYTES + DICT_BIGRAM_BYTES + 4U)  /* 40972 */
#define DICT_BIGRAM_OFF       (DICT_HEADER_SIZE + DICT_FREQ_BYTES)  /* 8200 */
#define DICT_CHECKSUM_OFF     (DICT_BLOB_SIZE - 4U)

/* =========================================================================
 * dict_checksum — CRC32 of the blob excluding the trailing checksum field
 * ========================================================================= */

static uint32_t dict_blob_checksum(const uint8_t *blob) {
    return netc_crc32(blob, DICT_CHECKSUM_OFF);
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

    d->magic     = NETC_DICT_MAGIC;
    d->version   = NETC_DICT_VERSION;
    d->model_id  = model_id;
    d->ctx_count = (uint8_t)NETC_CTX_COUNT;
    d->_pad      = 0;

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

    /* --- Compute checksum over the serialized blob --- */
    /* We compute the checksum from the blob representation for consistency
     * between train/save and load. Build the blob, compute, store. */
    uint8_t *tmp_blob = (uint8_t *)malloc(DICT_BLOB_SIZE);
    if (NETC_UNLIKELY(tmp_blob == NULL)) {
        free(d);
        return NETC_ERR_NOMEM;
    }

    /* Serialize header */
    netc_write_u32_le(tmp_blob + 0, d->magic);
    tmp_blob[4] = d->version;
    tmp_blob[5] = d->model_id;
    tmp_blob[6] = d->ctx_count;
    tmp_blob[7] = d->_pad;

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

    /* Compute and store checksum */
    d->checksum = netc_crc32(tmp_blob, DICT_CHECKSUM_OFF);
    netc_write_u32_le(tmp_blob + DICT_CHECKSUM_OFF, d->checksum);
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

    uint8_t *blob = (uint8_t *)malloc(DICT_BLOB_SIZE);
    if (NETC_UNLIKELY(blob == NULL)) {
        return NETC_ERR_NOMEM;
    }

    netc_write_u32_le(blob + 0, dict->magic);
    blob[4] = dict->version;
    blob[5] = dict->model_id;
    blob[6] = dict->ctx_count;
    blob[7] = dict->_pad;

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

    netc_write_u32_le(blob + DICT_CHECKSUM_OFF, dict->checksum);

    *out      = blob;
    *out_size = DICT_BLOB_SIZE;
    return NETC_OK;
}

/* =========================================================================
 * netc_dict_load — deserialize and validate blob
 * ========================================================================= */

netc_result_t netc_dict_load(const void *data, size_t size, netc_dict_t **out) {
    if (NETC_UNLIKELY(data == NULL || out == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(size < DICT_BLOB_SIZE)) {
        return NETC_ERR_DICT_INVALID;
    }

    const uint8_t *b = (const uint8_t *)data;

    uint32_t magic = netc_read_u32_le(b + 0);
    if (NETC_UNLIKELY(magic != NETC_DICT_MAGIC)) {
        return NETC_ERR_DICT_INVALID;
    }

    uint8_t version = b[4];
    if (NETC_UNLIKELY(version != NETC_DICT_VERSION)) {
        return NETC_ERR_VERSION;
    }
    uint8_t stored_ctx_count = b[6];
    if (NETC_UNLIKELY(stored_ctx_count != (uint8_t)NETC_CTX_COUNT)) {
        return NETC_ERR_VERSION;
    }

    /* Validate checksum */
    uint32_t stored   = netc_read_u32_le(b + DICT_CHECKSUM_OFF);
    uint32_t expected = dict_blob_checksum(b);
    if (NETC_UNLIKELY(stored != expected)) {
        return NETC_ERR_DICT_INVALID;
    }

    netc_dict_t *d = (netc_dict_t *)calloc(1, sizeof(netc_dict_t));
    if (NETC_UNLIKELY(d == NULL)) {
        return NETC_ERR_NOMEM;
    }

    d->magic     = magic;
    d->version   = version;
    d->model_id  = b[5];
    d->ctx_count = b[6];
    d->_pad      = b[7];
    d->checksum  = stored;

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

    *out = d;
    return NETC_OK;
}

/* =========================================================================
 * netc_dict_free / netc_dict_free_blob / netc_dict_model_id
 * ========================================================================= */

void netc_dict_free(netc_dict_t *dict) {
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
