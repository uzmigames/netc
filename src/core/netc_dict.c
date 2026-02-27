/**
 * netc_dict.c — Dictionary training, serialization, and management.
 *
 * Phase 1: Minimal implementation that supports passthrough mode.
 * Phase 2 will add frequency table training and ANS probability normalization.
 */

#include "netc_internal.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * CRC32 — simple table-free implementation for dictionary validation.
 * Phase 1: software CRC32. Phase 2 replaces with hardware or table-based.
 * ========================================================================= */

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));  /* 0 or 0xFFFFFFFF */
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t dict_checksum(const netc_dict_t *d) {
    /* Checksum covers all fields except the checksum field itself */
    uint32_t crc = 0;
    crc = crc32_update(crc, &d->magic,    sizeof(d->magic));
    crc = crc32_update(crc, &d->version,  sizeof(d->version));
    crc = crc32_update(crc, &d->model_id, sizeof(d->model_id));
    crc = crc32_update(crc, &d->_pad,     sizeof(d->_pad));
    return crc;
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
        return NETC_ERR_INVALID_ARG;  /* 0 and 255 are reserved */
    }
    if (NETC_UNLIKELY(count > 0 && (packets == NULL || sizes == NULL))) {
        return NETC_ERR_INVALID_ARG;
    }

    netc_dict_t *d = (netc_dict_t *)calloc(1, sizeof(netc_dict_t));
    if (NETC_UNLIKELY(d == NULL)) {
        return NETC_ERR_NOMEM;
    }

    d->magic    = NETC_DICT_MAGIC;
    d->version  = NETC_DICT_VERSION;
    d->model_id = model_id;
    d->_pad     = 0;

    /*
     * Phase 1: dictionary holds only metadata.
     * Phase 2: iterate packets[], build byte frequency tables per context bucket.
     *
     * Suppress unused parameter warnings until Phase 2.
     */
    (void)packets;
    (void)sizes;
    (void)count;

    d->checksum = dict_checksum(d);
    *out_dict   = d;
    return NETC_OK;
}

/* =========================================================================
 * Serialized dictionary blob layout:
 *   [0..3]  magic    (uint32 LE)
 *   [4]     version  (uint8)
 *   [5]     model_id (uint8)
 *   [6..7]  _pad     (uint16 LE)
 *   [8..11] checksum (uint32 LE, CRC32 of fields 0..7)
 *
 * Total: 12 bytes for Phase 1.
 * Phase 2 will append frequency table data after the header.
 * ========================================================================= */

#define DICT_BLOB_HEADER_SIZE 12U

/* =========================================================================
 * netc_dict_save
 * ========================================================================= */

netc_result_t netc_dict_save(const netc_dict_t *dict, void **out, size_t *out_size) {
    if (NETC_UNLIKELY(dict == NULL || out == NULL || out_size == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }

    uint8_t *blob = (uint8_t *)malloc(DICT_BLOB_HEADER_SIZE);
    if (NETC_UNLIKELY(blob == NULL)) {
        return NETC_ERR_NOMEM;
    }

    netc_write_u32_le(blob + 0, dict->magic);
    blob[4] = dict->version;
    blob[5] = dict->model_id;
    netc_write_u16_le(blob + 6, dict->_pad);
    netc_write_u32_le(blob + 8, dict->checksum);

    *out      = blob;
    *out_size = DICT_BLOB_HEADER_SIZE;
    return NETC_OK;
}

/* =========================================================================
 * netc_dict_load
 * ========================================================================= */

netc_result_t netc_dict_load(const void *data, size_t size, netc_dict_t **out) {
    if (NETC_UNLIKELY(data == NULL || out == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (NETC_UNLIKELY(size < DICT_BLOB_HEADER_SIZE)) {
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

    netc_dict_t *d = (netc_dict_t *)calloc(1, sizeof(netc_dict_t));
    if (NETC_UNLIKELY(d == NULL)) {
        return NETC_ERR_NOMEM;
    }

    d->magic    = magic;
    d->version  = version;
    d->model_id = b[5];
    d->_pad     = netc_read_u16_le(b + 6);
    d->checksum = netc_read_u32_le(b + 8);

    /* Validate checksum */
    uint32_t expected = dict_checksum(d);
    if (NETC_UNLIKELY(d->checksum != expected)) {
        free(d);
        return NETC_ERR_DICT_INVALID;
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
