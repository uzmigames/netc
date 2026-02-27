/**
 * test_bitstream.c — Unit tests for CRC32 and bitstream (writer + reader).
 *
 * Tests:
 *   CRC32:
 *     - Known vectors (empty string, "123456789" standard test, all-zeros)
 *     - netc_crc32_continue chaining correctness
 *   Bitstream writer:
 *     - Write and flush single bits
 *     - Write multi-bit values (0–32 bits)
 *     - Overflow detection
 *   Bitstream round-trip (writer → reader):
 *     - 1-bit values
 *     - 8-bit bytes
 *     - Mixed widths
 *     - Full 32-bit values
 *   Bitstream reader:
 *     - Peek does not advance
 *     - Empty detection
 */

#include "unity.h"
#include "../src/util/netc_crc32.h"
#include "../src/util/netc_bitstream.h"
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Unity lifecycle
 * ========================================================================= */

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * CRC32 tests
 * ========================================================================= */

void test_crc32_empty(void) {
    /* CRC32 of empty buffer is 0x00000000 */
    TEST_ASSERT_EQUAL_UINT32(0x00000000U, netc_crc32("", 0));
}

void test_crc32_standard_vector(void) {
    /* Standard CRC32 (IEEE 802.3) check value for "123456789" is 0xCBF43926 */
    const char *msg = "123456789";
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926U, netc_crc32(msg, 9));
}

void test_crc32_single_byte(void) {
    /* CRC32 of a single 0x00 byte */
    uint8_t b = 0x00;
    uint32_t crc = netc_crc32(&b, 1);
    /* Verify it's not the empty CRC */
    TEST_ASSERT_NOT_EQUAL(0x00000000U, crc);
    /* Deterministic: same byte produces same CRC */
    TEST_ASSERT_EQUAL_UINT32(crc, netc_crc32(&b, 1));
}

void test_crc32_all_zeros(void) {
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    uint32_t c1 = netc_crc32(buf, 8);
    uint32_t c2 = netc_crc32(buf, 16);
    TEST_ASSERT_NOT_EQUAL(c1, c2);  /* Different lengths → different CRC */
}

void test_crc32_all_ones(void) {
    uint8_t buf[8];
    memset(buf, 0xFF, sizeof(buf));
    uint32_t crc = netc_crc32(buf, 8);
    TEST_ASSERT_NOT_EQUAL(0x00000000U, crc);
    TEST_ASSERT_EQUAL_UINT32(crc, netc_crc32(buf, 8));
}

void test_crc32_different_data(void) {
    const char *a = "hello";
    const char *b = "world";
    TEST_ASSERT_NOT_EQUAL(netc_crc32(a, 5), netc_crc32(b, 5));
}

void test_crc32_continue_chaining(void) {
    /* CRC over "hello world" in one call should equal chaining */
    const char *msg = "hello world";
    uint32_t c_full = netc_crc32(msg, 11);
    uint32_t c_part = netc_crc32(msg, 5);
    uint32_t c_cont = netc_crc32_continue(c_part, msg + 5, 6);
    TEST_ASSERT_EQUAL_UINT32(c_full, c_cont);
}

void test_crc32_continue_single_byte_chunks(void) {
    const uint8_t data[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint32_t c_full  = netc_crc32(data, 4);
    uint32_t c_chain = netc_crc32(data, 1);
    c_chain = netc_crc32_continue(c_chain, data + 1, 1);
    c_chain = netc_crc32_continue(c_chain, data + 2, 1);
    c_chain = netc_crc32_continue(c_chain, data + 3, 1);
    TEST_ASSERT_EQUAL_UINT32(c_full, c_chain);
}

/* =========================================================================
 * Bitstream writer tests
 * ========================================================================= */

void test_bsw_init(void) {
    uint8_t buf[16];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(0, netc_bsw_size(&w));
}

void test_bsw_write_zero_bits(void) {
    uint8_t buf[8];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, netc_bsw_write(&w, 0, 0));
    TEST_ASSERT_EQUAL_UINT(0, netc_bsw_size(&w));
}

void test_bsw_flush_empty(void) {
    /* Flush with no data bits still writes a sentinel byte (0x01). */
    uint8_t buf[8];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(1, sz);   /* one sentinel byte */
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[0]); /* sentinel bit at position 0 */
}

void test_bsw_write_single_byte(void) {
    /* 8 data bits → data byte + sentinel byte = 2 bytes total. */
    uint8_t buf[4];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, netc_bsw_write(&w, 0xA5U, 8));
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(2, sz);           /* data + sentinel */
    TEST_ASSERT_EQUAL_UINT8(0xA5U, buf[0]); /* data byte unchanged */
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[1]); /* sentinel byte */
}

void test_bsw_write_two_bytes(void) {
    /* 16 data bits → 2 data bytes + sentinel byte = 3 bytes total. */
    uint8_t buf[4];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, netc_bsw_write(&w, 0x12U, 8));
    TEST_ASSERT_EQUAL_INT(0, netc_bsw_write(&w, 0x34U, 8));
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(3, sz);           /* 2 data + sentinel */
    TEST_ASSERT_EQUAL_UINT8(0x12U, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34U, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[2]); /* sentinel byte */
}

void test_bsw_lsb_first_packing(void) {
    /* Write 3 bits then 5 bits, verify LSB-first packing in the byte.
     * 8 data bits → data byte + sentinel byte = 2 total. */
    uint8_t buf[4];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    /* Write value 0x5 (=0b101) in 3 bits, then 0x1A (=0b11010) in 5 bits */
    netc_bsw_write(&w, 0x5U, 3);   /* bits 0-2: 101 */
    netc_bsw_write(&w, 0x1AU, 5);  /* bits 3-7: 11010 */
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(2, sz);           /* data + sentinel */
    /* data byte = 11010_101 (MSB..LSB) = 0xD5 */
    TEST_ASSERT_EQUAL_UINT8(0xD5U, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[1]); /* sentinel byte */
}

void test_bsw_write_32_bits(void) {
    /* 32 data bits → 4 data bytes + sentinel byte = 5 total. */
    uint8_t buf[8];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, netc_bsw_write(&w, 0x12345678U, 32));
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(5, sz);           /* 4 data + sentinel */
    /* LSB-first byte ordering: 0x78, 0x56, 0x34, 0x12 */
    TEST_ASSERT_EQUAL_UINT8(0x78U, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x56U, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x34U, buf[2]);
    TEST_ASSERT_EQUAL_UINT8(0x12U, buf[3]);
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[4]); /* sentinel byte */
}

void test_bsw_overflow_returns_error(void) {
    uint8_t buf[1];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, netc_bsw_write(&w, 0xFFU, 8));
    /* Buffer full — next write should fail */
    TEST_ASSERT_EQUAL_INT(-1, netc_bsw_write(&w, 0xFFU, 8));
}

void test_bsw_flush_overflow(void) {
    uint8_t buf[1];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    netc_bsw_write(&w, 0xFFU, 8); /* fill the byte */
    /* Manually set bits to simulate a pending partial byte */
    /* Write 1 bit after the buffer is full to force flush to overflow */
    netc_bsw_write(&w, 1U, 1);
    /* Now flush should return (size_t)-1 since ptr > end */
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT((size_t)-1, sz);
}

/* =========================================================================
 * Bitstream round-trip tests (writer → reader)
 * ========================================================================= */

void test_bitstream_roundtrip_bytes(void) {
    /* Write 8 bytes forward, read them back in reverse byte order.
     * With sentinel: 8 data bytes + 1 sentinel byte = 9 total.
     * Reader consumes sentinel, then reads 8-bit groups from MSB;
     * since each 8-bit write aligns to a byte and 8-bit reads reverse byte
     * order, the read sequence is values[7], values[6], ... values[0]. */
    uint8_t values[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    uint8_t buf[16];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    for (int i = 0; i < 8; i++) {
        netc_bsw_write(&w, values[i], 8);
    }
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(9, sz);  /* 8 data + 1 sentinel */

    netc_bsr_t r;
    netc_bsr_init(&r, buf, sz);
    uint32_t got;
    /* Reader reads bytes in reverse order (backward buffer traversal) */
    for (int i = 7; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(0, netc_bsr_read(&r, 8, &got));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)values[i], got);
    }
}

void test_bitstream_roundtrip_single_bits(void) {
    /*
     * Write 8 bits alternating: i&1 for i=0..7 → 0,1,0,1,0,1,0,1.
     * LSB-first packing: bit0=0, bit1=1, ... → 0b10101010 = 0xAA.
     * With sentinel: data byte 0xAA + sentinel byte 0x01 = 2 bytes.
     *
     * Reader skips sentinel byte (0x01), then reads the data byte 0xAA
     * from MSB to LSB: bit7=1,bit6=0,bit5=1,...,bit0=0 → reversed order.
     * Read sequence: 1,0,1,0,1,0,1,0 = (7-i)&1 for i=0..7.
     */
    uint8_t buf[4];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    for (int i = 0; i < 8; i++) {
        netc_bsw_write(&w, (uint32_t)(i & 1), 1);
    }
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(2, sz);            /* data byte + sentinel byte */
    TEST_ASSERT_EQUAL_UINT8(0xAAU, buf[0]);   /* verify LSB-first packing */
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[1]);   /* sentinel byte */

    netc_bsr_t r;
    netc_bsr_init(&r, buf, sz);
    uint32_t got;
    /* Reader reads from MSB of data byte: reverse of write order */
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(0, netc_bsr_read(&r, 1, &got));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)((7 - i) & 1), got);
    }
}

void test_bitstream_roundtrip_mixed_widths(void) {
    /*
     * Write 4 bits (0xA=0b1010) then 4 bits (0x5=0b0101) = 1 data byte.
     * Byte = 0b01011010 = 0x5A (bits 3-0 = 0xA, bits 7-4 = 0x5).
     * With sentinel: data byte 0x5A + sentinel byte 0x01 = 2 bytes.
     *
     * Reader skips sentinel byte, reads data byte from MSB:
     *   first 4 bits = bits 7-4 of 0x5A = 0101 = 0x5
     *   next  4 bits = bits 3-0 of 0x5A = 1010 = 0xA
     * Read order is reversed from write order (0x5 first, then 0xA).
     */
    uint8_t buf[4];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    netc_bsw_write(&w, 0xAU, 4);  /* bits 0-3: 1010 */
    netc_bsw_write(&w, 0x5U, 4);  /* bits 4-7: 0101 */
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(2, sz);            /* data byte + sentinel */
    TEST_ASSERT_EQUAL_UINT8(0x5AU, buf[0]);   /* 0b01011010 */
    TEST_ASSERT_EQUAL_UINT8(0x01U, buf[1]);   /* sentinel byte */

    netc_bsr_t r;
    netc_bsr_init(&r, buf, sz);
    uint32_t got;
    /* Reader reads from MSB: first read gives the high nibble (0x5) */
    TEST_ASSERT_EQUAL_INT(0, netc_bsr_read(&r, 4, &got));
    TEST_ASSERT_EQUAL_UINT32(0x5U, got);
    TEST_ASSERT_EQUAL_INT(0, netc_bsr_read(&r, 4, &got));
    TEST_ASSERT_EQUAL_UINT32(0xAU, got);
}

void test_bitstream_roundtrip_32bit(void) {
    /*
     * Write 4 bytes (32 data bits) as four 8-bit writes.
     * With sentinel: 4 data bytes + 1 sentinel byte = 5 total.
     * Reader skips sentinel, reads bytes in REVERSE order (MSB-first
     * accumulator reads the last-written byte first).
     */
    uint8_t vals[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    uint8_t buf[8];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    for (int i = 0; i < 4; i++) {
        netc_bsw_write(&w, vals[i], 8);
    }
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(5, sz);  /* 4 data + 1 sentinel */

    netc_bsr_t r;
    netc_bsr_init(&r, buf, sz);
    uint32_t got;
    /* Reader reads bytes in REVERSE write order */
    for (int i = 3; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(0, netc_bsr_read(&r, 8, &got));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)vals[i], got);
    }
}

/* =========================================================================
 * Bitstream reader — peek does not consume
 * ========================================================================= */

void test_bsr_peek_does_not_consume(void) {
    uint8_t buf[4];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    netc_bsw_write(&w, 0xA5U, 8);
    size_t sz = netc_bsw_flush(&w);

    netc_bsr_t r;
    netc_bsr_init(&r, buf, sz);
    uint32_t p1 = netc_bsr_peek(&r, 8);
    uint32_t p2 = netc_bsr_peek(&r, 8);
    TEST_ASSERT_EQUAL_UINT32(p1, p2);  /* peek is idempotent */
    uint32_t got;
    netc_bsr_read(&r, 8, &got);
    TEST_ASSERT_EQUAL_UINT32(p1, got);
}

/* =========================================================================
 * Bitstream reader — empty detection
 * ========================================================================= */

void test_bsr_empty_after_consuming_all(void) {
    /* Write 8 data bits + sentinel → 2 bytes. After reading the 8 bits
     * the reader should report empty. */
    uint8_t buf[2];
    netc_bsw_t w;
    netc_bsw_init(&w, buf, sizeof(buf));
    netc_bsw_write(&w, 0x5CU, 8);
    size_t sz = netc_bsw_flush(&w);
    TEST_ASSERT_EQUAL_UINT(2, sz);

    netc_bsr_t r;
    netc_bsr_init(&r, buf, sz);
    TEST_ASSERT_EQUAL_INT(0, netc_bsr_empty(&r));
    uint32_t got;
    netc_bsr_read(&r, 8, &got);
    TEST_ASSERT_EQUAL_INT(1, netc_bsr_empty(&r));
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    /* CRC32 */
    RUN_TEST(test_crc32_empty);
    RUN_TEST(test_crc32_standard_vector);
    RUN_TEST(test_crc32_single_byte);
    RUN_TEST(test_crc32_all_zeros);
    RUN_TEST(test_crc32_all_ones);
    RUN_TEST(test_crc32_different_data);
    RUN_TEST(test_crc32_continue_chaining);
    RUN_TEST(test_crc32_continue_single_byte_chunks);

    /* Bitstream writer */
    RUN_TEST(test_bsw_init);
    RUN_TEST(test_bsw_write_zero_bits);
    RUN_TEST(test_bsw_flush_empty);
    RUN_TEST(test_bsw_write_single_byte);
    RUN_TEST(test_bsw_write_two_bytes);
    RUN_TEST(test_bsw_lsb_first_packing);
    RUN_TEST(test_bsw_write_32_bits);
    RUN_TEST(test_bsw_overflow_returns_error);
    RUN_TEST(test_bsw_flush_overflow);

    /* Bitstream round-trip */
    RUN_TEST(test_bitstream_roundtrip_bytes);
    RUN_TEST(test_bitstream_roundtrip_single_bits);
    RUN_TEST(test_bitstream_roundtrip_mixed_widths);
    RUN_TEST(test_bitstream_roundtrip_32bit);

    /* Bitstream reader */
    RUN_TEST(test_bsr_peek_does_not_consume);
    RUN_TEST(test_bsr_empty_after_consuming_all);

    return UNITY_END();
}
