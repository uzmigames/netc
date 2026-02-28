## 1. Implementation

- [ ] 1.1 Merge `freq` into `netc_tans_encode_entry_t` (add `freq` + `lower` fields),
       update `netc_tans_build` to populate them (`src/algo/netc_tans.h`, `src/algo/netc_tans.c`)
- [ ] 1.2 Pre-store `TABLE_SIZE + slot` in `encode_state[]` during build so the hot path
       drops the `+ TABLE_SIZE` add
- [ ] 1.3 Update `netc_tans_encode` hot loop to use merged entry (single load for
       `freq`+`nb_hi`+`lower`, direct `encode_state` result)
- [ ] 1.4 Replace byte-at-a-time `netc_bsw_write` with word-at-a-time `netc_bsw_write`
       in `src/util/netc_bitstream.h`; move overflow guard to `netc_bsw_flush`
- [ ] 1.5 Remove per-symbol `X` bounds check from `netc_tans_decode` inner loop;
       keep single entry-check on `initial_state`

## 2. Testing

- [ ] 2.1 All existing unit tests pass (226 tests across 9 suites)
- [ ] 2.2 Roundtrip fuzz: verify encode→decode for all workloads WL-001..WL-007
- [ ] 2.3 Benchmark: measure before/after throughput on WL-001, WL-002, WL-003, WL-007

## 3. Validation

- [ ] 3.1 `cmake --build build-bench --config Release` — zero warnings
- [ ] 3.2 All 226 unit tests pass
- [ ] 3.3 Compress throughput WL-002: target ≥ 160 MB/s (was 94 MB/s)
