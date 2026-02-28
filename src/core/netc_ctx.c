/**
 * netc_ctx.c — Context lifecycle management.
 *
 * Implements netc_ctx_create, netc_ctx_destroy, netc_ctx_reset, netc_ctx_stats,
 * netc_strerror, and netc_version.
 */

#include "netc_internal.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Default configuration values
 * ========================================================================= */

static const netc_cfg_t NETC_CFG_DEFAULT = {
    .flags             = NETC_CFG_FLAG_STATEFUL,
    .ring_buffer_size  = 0,  /* 0 → use NETC_DEFAULT_RING_SIZE */
    .compression_level = 5,
    .simd_level        = 0,  /* 0 → auto-detect */
    .arena_size        = 0,  /* 0 → use NETC_DEFAULT_ARENA_SIZE */
};

/* =========================================================================
 * netc_ctx_create
 * ========================================================================= */

netc_ctx_t *netc_ctx_create(const netc_dict_t *dict, const netc_cfg_t *cfg) {
    if (cfg == NULL) {
        cfg = &NETC_CFG_DEFAULT;
    }

    netc_ctx_t *ctx = (netc_ctx_t *)calloc(1, sizeof(netc_ctx_t));
    if (NETC_UNLIKELY(ctx == NULL)) {
        return NULL;
    }

    ctx->dict              = dict;
    ctx->flags             = cfg->flags;
    ctx->compression_level = cfg->compression_level;
    ctx->simd_level        = cfg->simd_level;
    ctx->context_seq       = 0;

    /* Initialize SIMD dispatch table (auto-detects best available path) */
    netc_simd_ops_init(&ctx->simd_ops, (uint8_t)cfg->simd_level);

    /* Allocate ring buffer for stateful mode (cross-packet history) */
    if (cfg->flags & NETC_CFG_FLAG_STATEFUL) {
        ctx->ring_size = (cfg->ring_buffer_size > 0)
            ? (uint32_t)cfg->ring_buffer_size
            : (uint32_t)NETC_DEFAULT_RING_SIZE;
        ctx->ring = (uint8_t *)calloc(1, ctx->ring_size);
        if (NETC_UNLIKELY(ctx->ring == NULL)) {
            free(ctx);
            return NULL;
        }
    }

    /* Allocate working memory arena */
    ctx->arena_size = (cfg->arena_size > 0)
        ? cfg->arena_size
        : NETC_DEFAULT_ARENA_SIZE;
    ctx->arena = (uint8_t *)malloc(ctx->arena_size);
    if (NETC_UNLIKELY(ctx->arena == NULL)) {
        free(ctx->ring);
        free(ctx);
        return NULL;
    }

    /* Allocate previous-packet buffer for delta prediction (stateful only) */
    if (cfg->flags & NETC_CFG_FLAG_STATEFUL) {
        ctx->prev_pkt = (uint8_t *)calloc(1, NETC_MAX_PACKET_SIZE);
        if (NETC_UNLIKELY(ctx->prev_pkt == NULL)) {
            free(ctx->arena);
            free(ctx->ring);
            free(ctx);
            return NULL;
        }
    }
    ctx->prev_pkt_size = 0;

    memset(&ctx->stats, 0, sizeof(ctx->stats));
    return ctx;
}

/* =========================================================================
 * netc_ctx_destroy
 * ========================================================================= */

void netc_ctx_destroy(netc_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    free(ctx->prev_pkt);
    free(ctx->ring);
    free(ctx->arena);
    /* dict is shared and not owned by the context */
    free(ctx);
}

/* =========================================================================
 * netc_ctx_reset
 * ========================================================================= */

void netc_ctx_reset(netc_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->ring != NULL) {
        memset(ctx->ring, 0, ctx->ring_size);
        ctx->ring_pos = 0;
    }
    if (ctx->prev_pkt != NULL) {
        memset(ctx->prev_pkt, 0, NETC_MAX_PACKET_SIZE);
    }
    ctx->prev_pkt_size = 0;
    ctx->context_seq = 0;
    memset(&ctx->stats, 0, sizeof(ctx->stats));
}

/* =========================================================================
 * netc_ctx_stats
 * ========================================================================= */

netc_result_t netc_ctx_stats(const netc_ctx_t *ctx, netc_stats_t *out) {
    if (NETC_UNLIKELY(ctx == NULL)) {
        return NETC_ERR_CTX_NULL;
    }
    if (NETC_UNLIKELY(out == NULL)) {
        return NETC_ERR_INVALID_ARG;
    }
    if (!(ctx->flags & NETC_CFG_FLAG_STATS)) {
        return NETC_ERR_UNSUPPORTED;
    }
    *out = ctx->stats;
    return NETC_OK;
}

/* =========================================================================
 * netc_ctx_simd_level
 * ========================================================================= */

uint8_t netc_ctx_simd_level(const netc_ctx_t *ctx) {
    if (ctx == NULL) return 0U;
    return ctx->simd_ops.level;
}

/* =========================================================================
 * netc_strerror
 * ========================================================================= */

const char *netc_strerror(netc_result_t result) {
    switch (result) {
        case NETC_OK:               return "success";
        case NETC_ERR_NOMEM:        return "memory allocation failure";
        case NETC_ERR_TOOBIG:       return "input exceeds NETC_MAX_PACKET_SIZE";
        case NETC_ERR_CORRUPT:      return "corrupt or truncated compressed data";
        case NETC_ERR_DICT_INVALID: return "dictionary checksum mismatch or bad format";
        case NETC_ERR_BUF_SMALL:    return "output buffer capacity insufficient";
        case NETC_ERR_CTX_NULL:     return "NULL context pointer";
        case NETC_ERR_UNSUPPORTED:  return "algorithm or feature not supported";
        case NETC_ERR_VERSION:      return "model_id or dictionary format version mismatch";
        case NETC_ERR_INVALID_ARG:  return "invalid argument";
        default:                    return "unknown error";
    }
}

/* =========================================================================
 * netc_version
 * ========================================================================= */

const char *netc_version(void) {
    return NETC_VERSION_STR;
}
