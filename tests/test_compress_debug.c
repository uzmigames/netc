/**
 * test_compress_debug.c — Trace decode failure step-by-step.
 */
#include "netc.h"
#include "../src/algo/netc_tans.h"
#include "../src/util/netc_bitstream.h"
#include "../src/core/netc_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Replicate freq_normalize from netc_dict.c */
static void freq_normalize_local(
    const uint64_t raw[256], uint64_t total, uint16_t out[256])
{
    if (total == 0) {
        uint16_t base = (uint16_t)(4096 / 256);
        for (int s = 0; s < 256; s++) out[s] = base;
        return;
    }
    uint32_t tsum = 0;
    int max_sym = 0; uint32_t max_val = 0;
    for (int s = 0; s < 256; s++) {
        if (raw[s] == 0) { out[s] = 0; continue; }
        uint64_t scaled = (raw[s] * 4096) / total;
        if (scaled == 0) scaled = 1;
        out[s] = (uint16_t)scaled;
        tsum += (uint32_t)scaled;
        if (out[s] > max_val) { max_val = out[s]; max_sym = s; }
    }
    if (tsum < 4096) out[max_sym] += (uint16_t)(4096 - tsum);
    else if (tsum > 4096) {
        uint32_t excess = tsum - 4096;
        if (out[max_sym] > (uint16_t)excess + 1) out[max_sym] -= (uint16_t)excess;
        else for (int s = 0; s < 256 && excess > 0; s++) if (out[s]>1){out[s]--;excess--;}
    }
}

static int decode_trace(
    const netc_tans_table_t *tbl, netc_bsr_t *bsr,
    uint8_t *dst, size_t dst_size, uint32_t initial_state)
{
    uint32_t X = initial_state;
    for (size_t i = 0; i < dst_size; i++) {
        if (X < 4096 || X >= 8192) {
            printf("  FAIL at i=%zu: X=%u out of range\n", i, X);
            return -1;
        }
        uint32_t slot = X - 4096;
        const netc_tans_decode_entry_t *d = &tbl->decode[slot];
        dst[i] = d->symbol;
        int nb = d->nb_bits;
        uint32_t bits_val = 0;
        if (nb > 0) {
            int rc = netc_bsr_read(bsr, nb, &bits_val);
            if (i < 25 || (i >= 507 && i < 512)) {
                printf("  i=%zu sym=0x%02X X=%u slot=%u nb=%d bits=%u bsr.bits=%d -> X_new=%u\n",
                       i, d->symbol, X, slot, nb, bits_val, bsr->bits,
                       (unsigned)d->next_state_base + bits_val);
            }
            if (rc != 0) {
                printf("  UNDERFLOW at i=%zu: bsr.bits=%d\n", i, bsr->bits);
                return -1;
            }
        } else {
            if (i < 25 || (i >= 507 && i < 512)) {
                printf("  i=%zu sym=0x%02X X=%u slot=%u nb=0 -> X_new=%u\n",
                       i, d->symbol, X, slot, (unsigned)d->next_state_base);
            }
        }
        X = (uint32_t)d->next_state_base + bits_val;
    }
    return 0;
}

static int encode_trace(
    const netc_tans_table_t *tbl, const uint8_t *src, size_t src_size,
    netc_bsw_t *bsw, uint32_t init_state)
{
    uint32_t X = init_state;
    if (X < 4096) X = 4096;
    int total_bits = 0;
    for (size_t i = src_size; i-- > 0;) {
        uint8_t sym = src[i];
        uint32_t f = tbl->freq.freq[sym];
        if (f == 0) { printf("  sym absent at i=%zu\n", i); return 0; }
        int nb_hi = tbl->encode[sym].nb_hi;
        uint32_t lower = f << (uint32_t)nb_hi;
        int nb;
        uint32_t j;
        if (nb_hi == 0 || X >= lower) { nb = nb_hi; j = (X >> (uint32_t)nb) - f; }
        else { nb = nb_hi - 1; j = (X >> (uint32_t)nb) - f; }
        uint32_t bits = X & ((1u << (uint32_t)nb) - 1u);
        if (nb > 0) {
            netc_bsw_write(bsw, bits, nb);
            total_bits += nb;
        }
        uint32_t new_X = 4096u + (uint32_t)tbl->encode_state[(uint32_t)tbl->encode[sym].cumul + j];
        if (nb > 0 || i < 25 || (i >= 507 && i < 512)) {
            printf("  enc i=%zu sym=0x%02X X=%u nb=%d bits=%u j=%u new_X=%u\n",
                   i, sym, X, nb, bits, j, new_X);
        }
        X = new_X;
    }
    printf("  Final state: %u, total_bits=%d\n", X, total_bits);
    return (int)X;
}

int main(void) {
    uint8_t s_repetitive[512];
    uint8_t s_skewed[512];
    memset(s_repetitive, 0x41, sizeof(s_repetitive));
    for (size_t i = 0; i < sizeof(s_skewed); i++) {
        s_skewed[i] = (i % 5 == 0) ? (uint8_t)(i & 0x7F) : (uint8_t)0x41;
    }

    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    uint64_t raw[256] = {0};
    uint64_t total = 0;
    for (size_t i = 256; i < 512; i++) { raw[s_repetitive[i]]++; total++; }
    for (size_t i = 256; i < 512; i++) { raw[s_skewed[i]]++; total++; }
    freq_normalize_local(raw, total, ft.freq);
    printf("freq[0x41]=%u, cumul[0x41]=208 (expected)\n", ft.freq[0x41]);

    netc_tans_table_t tbl;
    netc_tans_build(&tbl, &ft);

    /* Show freq table */
    printf("Symbols with freq > 0:\n");
    uint16_t cumul2[257] = {0};
    for (int s = 0; s < 256; s++) cumul2[s+1] = cumul2[s] + ft.freq[s];
    int nsyms = 0;
    for (int s = 0; s < 256; s++) {
        if (ft.freq[s] > 0) {
            printf("  sym=0x%02X freq=%u cumul=%u\n", s, ft.freq[s], cumul2[s]);
            nsyms++;
        }
    }
    printf("Total symbols: %d\n", nsyms);

    /* Show encode table entry for 0x41 */
    printf("encode[0x41]: nb_hi=%u, cumul=%u\n",
           tbl.encode[0x41].nb_hi, tbl.encode[0x41].cumul);

    /* Check consistency for slot 3522 */
    {
        uint16_t slot = 3522;
        printf("decode[3522]: sym=0x%02X nb=%u nsb=%u\n",
               tbl.decode[slot].symbol, tbl.decode[slot].nb_bits,
               tbl.decode[slot].next_state_base);
        /* Find which encode_state entry maps to 3522 */
        for (int i = 0; i < 4096; i++) {
            if (tbl.encode_state[i] == slot) {
                printf("encode_state[%d]=3522 -> sym=? cumul range: check all syms\n", i);
                for (int s = 0; s < 256; s++) {
                    if (ft.freq[s] > 0 && i >= cumul2[s] && i < cumul2[s+1]) {
                        printf("  => sym=0x%02X cumul=%u k=%d\n", s, cumul2[s], i-cumul2[s]);
                    }
                }
            }
        }
    }

    /* Verify consistency: for each sym s and occurrence k, decode[encode_state[cumul[s]+k]].symbol == s */
    int inconsistencies = 0;
    for (int s = 0; s < 256; s++) {
        if (ft.freq[s] == 0) continue;
        for (uint32_t k = 0; k < ft.freq[s] && inconsistencies < 5; k++) {
            uint32_t idx = (uint32_t)cumul2[s] + k;
            uint16_t pos = tbl.encode_state[idx];
            uint8_t dsym = tbl.decode[pos].symbol;
            if (dsym != (uint8_t)s) {
                printf("INCONSISTENCY: encode_state[%u+%u=%u]=%u decode[%u].sym=0x%02X expected=0x%02X\n",
                       cumul2[s], k, idx, pos, pos, dsym, (uint8_t)s);
                inconsistencies++;
            }
        }
    }
    if (inconsistencies == 0) printf("Table consistency: OK\n");
    else printf("Table has %d+ inconsistencies\n", inconsistencies);

    /* Trace encode */
    uint8_t bits[65536];
    netc_bsw_t bsw;
    netc_bsw_init(&bsw, bits, sizeof(bits));
    printf("Encode trace (first/last 20):\n");
    int fs = encode_trace(&tbl, s_repetitive, sizeof(s_repetitive), &bsw, 4096);
    size_t bsz = netc_bsw_flush(&bsw);
    printf("Encode: fs=%d bsz=%zu\n", fs, bsz);

    if (bsz > 0) {
        printf("Bitstream (%zu bytes): ", bsz);
        for (size_t i = 0; i < bsz && i < 16; i++) printf("%02X ", bits[i]);
        printf("\n");
    }

    if (fs == 0 || fs < 4096) { printf("ENCODE FAILED\n"); return 1; }

    printf("\nDecode trace:\n");
    netc_bsr_t bsr;
    netc_bsr_init(&bsr, bits, bsz);
    printf("  After BSR init: bits=%d ptr_offset=%td\n",
           bsr.bits, bsr.ptr - (const uint8_t*)bits);
    uint8_t dst[512] = {0};
    int dr = decode_trace(&tbl, &bsr, dst, sizeof(s_repetitive), (uint32_t)fs);
    printf("Decode result: %d\n", dr);
    if (dr == 0) {
        printf("Match: %s\n", memcmp(s_repetitive, dst, sizeof(s_repetitive))==0 ? "YES" : "NO");
    }
    return 0;
}
