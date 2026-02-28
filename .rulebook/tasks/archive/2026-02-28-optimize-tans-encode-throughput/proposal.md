# Proposal: optimize-tans-encode-throughput

## Why

The tANS encoder is 2-3x slower than OodleNetwork (WL-002: 1400ns vs 600ns, 94 MB/s vs
222 MB/s) despite having worse compression ratio. Root-cause analysis identified three
structural bottlenecks in the encode hot path:

1. **Three dependent memory loads per symbol**: `freq.freq[sym]` (512B array) +
   `encode[sym].nb_hi` + `encode[sym].cumul` (1KB array) + `encode_state[cumul+j]` (8KB
   array). The last load cannot start until `cumul` is resolved — a serial dependency
   chain of ~8-12 cycles per symbol.

2. **Byte-at-a-time bitstream flush**: `netc_bsw_write` flushes 1 byte per loop iteration
   via a `while (bits >= 8)` loop, causing a branch per symbol and preventing the compiler
   from vectorizing or unrolling the bitstream path.

3. **Per-symbol bounds check in the decode hot path**: `if (X < TABLE_SIZE || X >= 2*TABLE_SIZE)`
   inside the decode loop causes a branch misprediction on every packet boundary.

## What Changes

### 1. Eliminate `encode_state[]` lookup chain — pre-compute `next_state` per symbol per rank

Replace the two-level encode lookup:
```
X = TABLE_SIZE + encode_state[encode[sym].cumul + j]
```
with a flat per-symbol state table embedded directly in the encode entry:
```
X = encode[sym].state_table[j]   // state_table stores TABLE_SIZE + slot directly
```

`netc_tans_encode_entry_t` gains a `uint16_t state_table[max_freq]` pointer, but since
`max_freq <= TABLE_SIZE` and symbols share `encode_state[]` via `cumul`, the cleanest
approach is to keep `encode_state[]` but index it as `encode_state[cumul[s] + j]` and
store the full `TABLE_SIZE + slot` value directly (removing the `+ TABLE_SIZE` from the
hot path). The load chain stays the same but the dependent add is removed.

**Better approach**: merge `freq.freq[s]` into `netc_tans_encode_entry_t` so both
`freq` and `nb_hi` are fetched in one 4-byte load:

```c
typedef struct {
    uint16_t freq;    // was in separate freq table
    uint8_t  nb_hi;
    uint8_t  _pad;
    uint16_t cumul;
    uint16_t _pad2;
} netc_tans_encode_entry_t;  // 8 bytes, 32-entry per cache line
```

This reduces loads 1+2 into a single cache line hit, cutting the per-symbol critical
path from 3 serial loads to 2.

Also pre-compute `lower = freq << nb_hi` and store it in the entry, eliminating the
shift from the hot path.

### 2. Word-at-a-time bitstream writer

Replace the `while (bits >= 8)` byte loop with a 4-byte word flush triggered when
`bits >= 32`:

```c
static NETC_INLINE void netc_bsw_write32(netc_bsw_t *w, uint32_t v, int nb) {
    w->accum |= (uint64_t)v << w->bits;
    w->bits  += nb;
    if (w->bits >= 32) {
        *(uint32_t *)w->ptr = (uint32_t)w->accum;  // unaligned store OK
        w->ptr  += 4;
        w->accum >>= 32;
        w->bits  -= 32;
    }
}
```

The branch fires at most once per ~3 symbols (vs once per symbol before). Combined with
the overflow check moved to `netc_bsw_flush()`, this eliminates the NETC_UNLIKELY check
from the hot path.

### 3. Remove per-symbol state bounds check from decode loop

Move `if (X < TABLE_SIZE || X >= 2*TABLE_SIZE)` to a single check at entry (initial
state) and trust the table invariant inside the loop. The table construction guarantees
that all transitions stay within `[TABLE_SIZE, 2*TABLE_SIZE)`.

## Impact

- Affected specs: `specs/codec/tans.md`, `specs/codec/bitstream.md`
- Affected code: `src/algo/netc_tans.h`, `src/algo/netc_tans.c`,
  `src/util/netc_bitstream.h`
- Breaking change: NO — wire format unchanged, only internal table layout and I/O
  implementation change
- User benefit: Target 2x compression throughput improvement (94 → ~180 MB/s for
  WL-002), closing the gap to Oodle (222 MB/s)
