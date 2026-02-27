/**
 * bench_corpus.c — Deterministic workload corpus generators.
 *
 * Implements WL-001 through WL-008 per RFC-002 §3.
 */

#include "bench_corpus.h"
#include <string.h>
#include <math.h>

/* =========================================================================
 * splitmix64 PRNG — deterministic, period 2^64, excellent quality
 * ========================================================================= */
static uint64_t sm64_next(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/* Uniform float in [0, 1) */
static double sm64_f64(uint64_t *s)
{
    return (double)(sm64_next(s) >> 11) * (1.0 / (double)(UINT64_C(1) << 53));
}

/* Uniform integer in [lo, hi] (inclusive) */
static uint32_t sm64_range(uint64_t *s, uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(sm64_f64(s) * (double)(hi - lo + 1));
}

/* =========================================================================
 * WL-001 / WL-002 / WL-003 — Game State Packet
 *
 * Layout per RFC-002 §3.1–3.3 (varying size):
 *   Bytes  0– 3  player_id    uint32 (low-range)
 *   Bytes  4– 7  sequence     uint32 (monotone-ish + delta noise)
 *   Bytes  8–15  tick         uint64 (monotone + small delta)
 *   Bytes 16–17  flags        uint16 (enum-like, sparse)
 *   Bytes 18–31  pad          zeros (for 64-byte) or more fields
 *   Bytes 32+    pos[3]       float32 (clustered around local origin)
 *   Bytes 44+    vel[3]       float32 (small magnitudes)
 *   Bytes 56+    misc[8]      mixed enum/counters
 *   Extended (128B, 256B): animation_frame, health, inventory, etc.
 * ========================================================================= */

/* Shared per-corpus sequence counter (relative to seed) */
static void gen_game_state(bench_corpus_t *c, size_t pkt_size)
{
    uint8_t *p = c->packet;
    memset(p, 0, pkt_size);

    /* player_id: 1..1000 (low range → good compression) */
    uint32_t player_id = sm64_range(&c->rng, 1, 1000);
    memcpy(p + 0, &player_id, 4);

    /* sequence: increments per-player — simulate with noise */
    uint32_t seq = (uint32_t)(sm64_next(&c->rng) & 0x00FFFFFF);
    memcpy(p + 4, &seq, 4);

    /* tick: low 48 bits vary slowly */
    uint64_t tick = sm64_next(&c->rng) & 0x0000FFFFFFFFFFFFull;
    memcpy(p + 8, &tick, 8);

    /* flags: sparse bitmask from 8 possibilities */
    uint16_t flags = (uint16_t)(1u << sm64_range(&c->rng, 0, 7));
    memcpy(p + 16, &flags, 2);
    /* pad[7] already zeroed */

    /* pos[3]: clustered around small origin ± 100 units */
    float pos[3];
    for (int i = 0; i < 3; i++) {
        pos[i] = (float)((sm64_f64(&c->rng) - 0.5) * 200.0);
    }
    memcpy(p + 32, pos, 12);

    /* vel[3]: small magnitude ± 10 */
    float vel[3];
    for (int i = 0; i < 3; i++) {
        vel[i] = (float)((sm64_f64(&c->rng) - 0.5) * 20.0);
    }
    memcpy(p + 44, vel, 12);

    /* misc[8]: counters 0..255 */
    for (int i = 0; i < 8; i++) {
        p[56 + i] = (uint8_t)sm64_range(&c->rng, 0, 255);
    }

    /* Extended fields for 128-byte and 256-byte variants */
    if (pkt_size >= 128) {
        /* animation_frame: uint16 0..500 */
        uint16_t anim = (uint16_t)sm64_range(&c->rng, 0, 500);
        memcpy(p + 64, &anim, 2);
        /* health: uint16 0..10000 (clustered near max) */
        uint16_t health = (uint16_t)(10000 - sm64_range(&c->rng, 0, 200));
        memcpy(p + 66, &health, 2);
        /* inventory[20]: sparse item IDs 0..1000 */
        for (int i = 0; i < 20; i++) {
            uint16_t item = (uint16_t)(sm64_range(&c->rng, 0, 10) == 0
                                       ? sm64_range(&c->rng, 1, 1000) : 0);
            memcpy(p + 68 + i * 2, &item, 2);
        }
        /* rot[4] quaternion (unit): approximate with small angles */
        float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        rot[0] = (float)(sm64_f64(&c->rng) * 0.1 - 0.05);
        rot[1] = (float)(sm64_f64(&c->rng) * 0.1 - 0.05);
        rot[2] = (float)(sm64_f64(&c->rng) * 0.1 - 0.05);
        /* approximate normalize: w ≈ sqrt(1 - x²-y²-z²) */
        double w2 = 1.0 - (double)rot[0]*(double)rot[0]
                        - (double)rot[1]*(double)rot[1]
                        - (double)rot[2]*(double)rot[2];
        rot[3] = (w2 > 0.0) ? (float)sqrt(w2) : 0.0f;
        memcpy(p + 108, rot, 16);
    }

    if (pkt_size >= 256) {
        /* status_effects[16]: sparse bitmasks */
        for (int i = 0; i < 16; i++) {
            p[128 + i] = (uint8_t)(sm64_range(&c->rng, 0, 15) == 0
                                   ? (uint8_t)sm64_range(&c->rng, 1, 255) : 0);
        }
        /* chat[64]: mostly zeros (chat is rare) */
        if (sm64_range(&c->rng, 0, 9) == 0) {
            uint32_t chat_len = sm64_range(&c->rng, 1, 60);
            for (uint32_t i = 0; i < chat_len; i++) {
                p[144 + i] = (uint8_t)sm64_range(&c->rng, 32, 126);
            }
        }
        /* score/kills/deaths: small integers */
        uint32_t score = sm64_range(&c->rng, 0, 9999);
        memcpy(p + 208, &score, 4);
        uint16_t kills = (uint16_t)sm64_range(&c->rng, 0, 200);
        memcpy(p + 212, &kills, 2);
        uint16_t deaths = (uint16_t)sm64_range(&c->rng, 0, 100);
        memcpy(p + 214, &deaths, 2);
        /* padding to 256 already zeroed */
    }

    c->pkt_len = pkt_size;
}

/* =========================================================================
 * WL-004 — Financial Tick Data (32 bytes)
 *
 *   Bytes  0– 7  symbol      char[8] (8-char padded, e.g. "AAPL    ")
 *   Bytes  8–15  price       double  (clustered around moving average)
 *   Bytes 16–19  volume      uint32  (clustered around mean, fat tail)
 *   Bytes 20–27  timestamp   uint64  (monotone, nanoseconds)
 *   Bytes 28–31  flags       uint32  (sparse bitmask)
 * ========================================================================= */
static const char *const s_symbols[] = {
    "AAPL    ", "MSFT    ", "GOOGL   ", "AMZN    ", "TSLA    ",
    "NVDA    ", "META    ", "BRK.B   ", "JPM     ", "UNH     "
};
#define NUM_SYMBOLS 10

static void gen_financial_tick(bench_corpus_t *c)
{
    uint8_t *p = c->packet;
    memset(p, 0, 32);

    /* symbol: pick from fixed 10-symbol universe */
    uint32_t sym_idx = sm64_range(&c->rng, 0, NUM_SYMBOLS - 1);
    memcpy(p + 0, s_symbols[sym_idx], 8);

    /* price: random walk around moving average (starts at 100.0) */
    double delta = (sm64_f64(&c->rng) - 0.5) * 0.02;  /* ±1% tick */
    c->wl004_price *= (1.0 + delta);
    if (c->wl004_price < 1.0) c->wl004_price = 1.0;
    if (c->wl004_price > 10000.0) c->wl004_price = 10000.0;
    memcpy(p + 8, &c->wl004_price, 8);

    /* volume: log-normal around 1000, clamped 1..100000 */
    double vol_f = exp(sm64_f64(&c->rng) * 4.0 + 3.0); /* exp(3..7) ≈ 20..1097 */
    uint32_t volume = (uint32_t)(vol_f < 1.0 ? 1.0 : vol_f > 100000.0 ? 100000.0 : vol_f);
    memcpy(p + 16, &volume, 4);

    /* timestamp: monotone nanosecond clock (simulate 1us ticks) */
    static uint64_t tick_ns = 0;
    if (tick_ns == 0) tick_ns = UINT64_C(1704067200000000000); /* 2024-01-01 */
    tick_ns += sm64_range(&c->rng, 100, 10000); /* 100ns..10us inter-tick */
    memcpy(p + 20, &tick_ns, 8);

    /* flags: sparse — mostly 0, sometimes BID/ASK/TRADE */
    uint32_t flags = (sm64_range(&c->rng, 0, 7) == 0)
                     ? (uint32_t)(1u << sm64_range(&c->rng, 0, 3)) : 0u;
    memcpy(p + 28, &flags, 4);

    c->pkt_len = 32;
}

/* =========================================================================
 * WL-005 — Telemetry Packet (512 bytes)
 *
 * IoT / sensor aggregation packet:
 *   Bytes   0–  7  device_id    uint64
 *   Bytes   8– 11  sensor_count uint32 (1..32)
 *   Bytes  12– 15  flags        uint32
 *   Bytes  16–271  readings[32] struct { uint32 sensor_id; float value; }
 *   Bytes 272–399  counters[32] uint32 (monotone counters)
 *   Bytes 400–463  enums[64]    uint8  (0..15 sparse)
 *   Bytes 464–511  reserved     zeros
 * ========================================================================= */
static void gen_telemetry(bench_corpus_t *c)
{
    uint8_t *p = c->packet;
    memset(p, 0, 512);

    /* device_id: low cardinality (1..500 devices) */
    uint64_t dev_id = sm64_range(&c->rng, 1, 500);
    memcpy(p + 0, &dev_id, 8);

    /* sensor_count */
    uint32_t n_sensors = sm64_range(&c->rng, 8, 32);
    memcpy(p + 8, &n_sensors, 4);

    /* flags */
    uint32_t flags = (uint32_t)sm64_range(&c->rng, 0, 15);
    memcpy(p + 12, &flags, 4);

    /* readings[32]: sensor_id (1..100) + float value (clustered) */
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t sid = (i < n_sensors) ? sm64_range(&c->rng, 1, 100) : 0u;
        float val = (i < n_sensors)
                    ? (float)((sm64_f64(&c->rng) - 0.5) * 100.0)
                    : 0.0f;
        memcpy(p + 16 + i * 8 + 0, &sid, 4);
        memcpy(p + 16 + i * 8 + 4, &val, 4);
    }

    /* counters[32]: monotone, small deltas */
    static uint32_t base_ctrs[32];
    static int ctrs_init = 0;
    if (!ctrs_init) {
        ctrs_init = 1;
        memset(base_ctrs, 0, sizeof(base_ctrs));
    }
    for (int i = 0; i < 32; i++) {
        base_ctrs[i] += sm64_range(&c->rng, 0, 10);
        memcpy(p + 272 + i * 4, &base_ctrs[i], 4);
    }

    /* enums[64]: values 0..15, sparse non-zero */
    for (int i = 0; i < 64; i++) {
        p[400 + i] = (sm64_range(&c->rng, 0, 4) == 0)
                     ? (uint8_t)sm64_range(&c->rng, 1, 15) : 0;
    }

    c->pkt_len = 512;
}

/* =========================================================================
 * WL-006 — Random Data (128 bytes, entropy ≈ 8 bits/byte)
 * ========================================================================= */
static void gen_random(bench_corpus_t *c)
{
    uint8_t *p = c->packet;
    size_t i = 0;
    while (i + 8 <= 128) {
        uint64_t v = sm64_next(&c->rng);
        memcpy(p + i, &v, 8);
        i += 8;
    }
    while (i < 128) {
        p[i++] = (uint8_t)sm64_next(&c->rng);
    }
    c->pkt_len = 128;
}

/* =========================================================================
 * WL-007 — Highly Repetitive (128 bytes)
 *
 * Cycles through 4 patterns every 4 packets:
 *   Phase 0: all-zeros
 *   Phase 1: all-ones (0xFF)
 *   Phase 2: run-length (0x00 0x00 ... 0xFF 0xFF ...)
 *   Phase 3: alternating 0xAA / 0x55
 * ========================================================================= */
static void gen_repetitive(bench_corpus_t *c)
{
    uint8_t *p = c->packet;
    switch (c->wl007_phase & 3) {
        case 0: memset(p, 0x00, 128); break;
        case 1: memset(p, 0xFF, 128); break;
        case 2:
            memset(p,       0x00, 64);
            memset(p + 64,  0xFF, 64);
            break;
        case 3:
            for (int i = 0; i < 128; i++) p[i] = (i & 1) ? 0x55 : 0xAA;
            break;
    }
    c->wl007_phase++;
    c->pkt_len = 128;
}

/* =========================================================================
 * WL-008 — Mixed Traffic (32–512 bytes)
 *
 * Weighted mix per RFC-002 §3.8:
 *   60% WL-001 (64 B)
 *   20% WL-002 (128 B)
 *   10% WL-005 (512 B)
 *   10% WL-006 (128 B random)
 * ========================================================================= */
static void gen_mixed(bench_corpus_t *c)
{
    uint32_t r = sm64_range(&c->rng, 0, 99);
    if (r < 60) {
        gen_game_state(c, 64);
    } else if (r < 80) {
        gen_game_state(c, 128);
    } else if (r < 90) {
        gen_telemetry(c);
    } else {
        gen_random(c);
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void bench_corpus_init(bench_corpus_t *c, bench_workload_t wl, uint64_t seed)
{
    memset(c, 0, sizeof(*c));
    c->workload     = wl;
    c->seed         = seed;
    c->rng          = seed;
    c->wl007_phase  = 0;
    c->wl004_price  = 100.0;
    c->pkt_len      = 0;
}

void bench_corpus_reset(bench_corpus_t *c)
{
    bench_workload_t wl   = c->workload;
    uint64_t         seed = c->seed;
    bench_corpus_init(c, wl, seed);
}

size_t bench_corpus_next(bench_corpus_t *c)
{
    switch (c->workload) {
        case BENCH_WL_001: gen_game_state(c, 64);  break;
        case BENCH_WL_002: gen_game_state(c, 128); break;
        case BENCH_WL_003: gen_game_state(c, 256); break;
        case BENCH_WL_004: gen_financial_tick(c);  break;
        case BENCH_WL_005: gen_telemetry(c);       break;
        case BENCH_WL_006: gen_random(c);          break;
        case BENCH_WL_007: gen_repetitive(c);      break;
        case BENCH_WL_008: gen_mixed(c);           break;
        default:
            c->pkt_len = 0;
            break;
    }
    return c->pkt_len;
}

void bench_corpus_train(bench_workload_t wl, uint64_t seed,
                        uint8_t **bufs, size_t *lens, size_t n,
                        uint8_t *storage)
{
    bench_corpus_t c;
    bench_corpus_init(&c, wl, seed);
    for (size_t i = 0; i < n; i++) {
        bufs[i] = storage + i * BENCH_CORPUS_MAX_PKT;
        lens[i] = bench_corpus_next(&c);
        memcpy(bufs[i], c.packet, lens[i]);
    }
}

const char *bench_workload_name(bench_workload_t wl)
{
    switch (wl) {
        case BENCH_WL_001: return "WL-001 Game State 64B";
        case BENCH_WL_002: return "WL-002 Game State 128B";
        case BENCH_WL_003: return "WL-003 Game State 256B";
        case BENCH_WL_004: return "WL-004 Financial Tick 32B";
        case BENCH_WL_005: return "WL-005 Telemetry 512B";
        case BENCH_WL_006: return "WL-006 Random 128B";
        case BENCH_WL_007: return "WL-007 Repetitive 128B";
        case BENCH_WL_008: return "WL-008 Mixed Traffic";
        default:           return "WL-??? Unknown";
    }
}

size_t bench_workload_pkt_size(bench_workload_t wl)
{
    switch (wl) {
        case BENCH_WL_001: return 64;
        case BENCH_WL_002: return 128;
        case BENCH_WL_003: return 256;
        case BENCH_WL_004: return 32;
        case BENCH_WL_005: return 512;
        case BENCH_WL_006: return 128;
        case BENCH_WL_007: return 128;
        case BENCH_WL_008: return 0;   /* variable */
        default:           return 0;
    }
}
