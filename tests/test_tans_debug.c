/**
 * test_tans_debug.c — Direct tANS encode/decode round-trip tests.
 * Verifies that the tANS codec correctly encodes and decodes data.
 */

#include "unity.h"
#include "../src/algo/netc_tans.h"
#include "../src/util/netc_bitstream.h"
#include <string.h>
#include <stdio.h>

void setUp(void)    {}
void tearDown(void) {}

static void do_tans_roundtrip(const char *name,
    uint16_t freq_a, uint16_t freq_b,
    const uint8_t *src, size_t src_size)
{
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x41] = freq_a;
    if (freq_b > 0) ft.freq[0x42] = freq_b;

    netc_tans_table_t tbl;
    int r = netc_tans_build(&tbl, &ft);
    printf("[%s] build=%d valid=%d\n", name, r, tbl.valid);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r, name);

    uint8_t buf[65536];
    netc_bsw_t bsw;
    netc_bsw_init(&bsw, buf, sizeof(buf));

    uint32_t fs = netc_tans_encode(&tbl, src, src_size, &bsw, 4096);
    size_t bs = netc_bsw_flush(&bsw);
    printf("[%s] encode: final_state=%u bits_sz=%zu\n", name, fs, bs);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0, fs, name);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(4096, fs, name);
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(8192, fs, name);

    uint8_t dst[65536] = {0};
    netc_bsr_t bsr;
    netc_bsr_init(&bsr, buf, bs);

    int dr = netc_tans_decode(&tbl, &bsr, dst, src_size, fs);
    printf("[%s] decode=%d\n", name, dr);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dr, name);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(src, dst, src_size, name);
}

void test_tans_uniform_2sym(void) {
    uint8_t src[8] = {0x41,0x41,0x42,0x41,0x41,0x42,0x41,0x41};
    do_tans_roundtrip("2sym_2048_2048", 2048, 2048, src, 8);
}

void test_tans_skewed_2sym(void) {
    uint8_t src[8] = {0x41,0x41,0x41,0x42,0x41,0x41,0x41,0x41};
    do_tans_roundtrip("2sym_3072_1024", 3072, 1024, src, 8);
}

void test_tans_heavy_skew(void) {
    uint8_t src[16];
    memset(src, 0x41, 15);
    src[15] = 0x42;
    do_tans_roundtrip("heavy_3840_256", 3840, 256, src, 16);
}

void test_tans_all_same_4096(void) {
    uint8_t src[32];
    memset(src, 0x41, sizeof(src));
    do_tans_roundtrip("all_same_4096", 4096, 0, src, sizeof(src));
}

void test_tans_repetitive_512(void) {
    uint8_t src[512];
    memset(src, 0x41, sizeof(src));
    /* Use a realistic training distribution */
    netc_freq_table_t ft;
    memset(&ft, 0, sizeof(ft));
    ft.freq[0x41] = 3688;
    /* Spread remaining 408 slots over 51 symbols 0x00..0x7E at i%5==0 */
    uint16_t rem = (uint16_t)(4096 - 3688);
    int nsyms = 0;
    for (int i = 0; i < 128 && rem > 0; i += 5) {
        if (i == 0x41) continue;
        ft.freq[i] = 8;
        rem = (uint16_t)(rem > 8 ? rem - 8 : 0);
        nsyms++;
    }
    /* Check all src bytes are in table */
    int ok = 1;
    for (size_t j = 0; j < sizeof(src); j++) {
        if (ft.freq[src[j]] == 0) { ok = 0; break; }
    }
    printf("All src bytes in table: %s\n", ok ? "YES" : "NO");
    TEST_ASSERT_EQUAL_INT(1, ok);

    /* Ensure sum = TABLE_SIZE */
    uint32_t total = 0;
    for (int i = 0; i < 256; i++) total += ft.freq[i];
    /* Adjust to exact TABLE_SIZE */
    if (total < 4096) ft.freq[0x41] = (uint16_t)(ft.freq[0x41] + (4096 - total));
    if (total > 4096) ft.freq[0x41] = (uint16_t)(ft.freq[0x41] - (total - 4096));

    netc_tans_table_t tbl;
    int r = netc_tans_build(&tbl, &ft);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_INT(1, tbl.valid);

    uint8_t buf[65536];
    netc_bsw_t bsw;
    netc_bsw_init(&bsw, buf, sizeof(buf));

    uint32_t fs = netc_tans_encode(&tbl, src, sizeof(src), &bsw, 4096);
    size_t bs = netc_bsw_flush(&bsw);
    printf("repetitive_512: final_state=%u bits_sz=%zu\n", fs, bs);
    TEST_ASSERT_NOT_EQUAL_UINT32(0, fs);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4096, fs);
    TEST_ASSERT_LESS_THAN_UINT32(8192, fs);

    uint8_t dst[512] = {0};
    netc_bsr_t bsr;
    netc_bsr_init(&bsr, buf, bs);
    int dr = netc_tans_decode(&tbl, &bsr, dst, sizeof(src), fs);
    printf("repetitive_512: decode=%d\n", dr);
    TEST_ASSERT_EQUAL_INT(0, dr);
    TEST_ASSERT_EQUAL_MEMORY(src, dst, sizeof(src));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_tans_uniform_2sym);
    RUN_TEST(test_tans_skewed_2sym);
    RUN_TEST(test_tans_heavy_skew);
    RUN_TEST(test_tans_all_same_4096);
    RUN_TEST(test_tans_repetitive_512);
    return UNITY_END();
}
