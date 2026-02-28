# Proposal: implement-lzp-hash-prediction

## Why

Deep analysis of OodleNetwork reveals it uses **Static LZP (Lempel-Ziv Prediction)**,
NOT tANS/FSE. This is the core algorithm difference that gives Oodle its superior
compression ratios (0.55 on WL-001 64B vs netc's 0.94).

How OodleNetwork LZP works:
- **Training**: Stores raw training bytes in a large hash-indexed dictionary (4-32MB).
  `OodleNetwork1_Shared_SetWindow(shared, htbits, window, window_size)` concatenates
  training packets into one contiguous "window" buffer.
- **Per-byte prediction**: For each input byte, hash the N previous bytes (context) →
  look up the hash table → get predicted byte from the dictionary window.
  If match: emit a ~0.3-bit flag. If miss: emit a ~9-bit flag + literal byte.
- **Zero per-packet overhead**: Oodle's encode/decode API writes NO header at all.
  The 4B rawLen in our benchmark adapter is added by `bench_oodle.c`, not by Oodle.
- **Static dictionary**: The hash table is read-only after training — shared across
  all connections. State is only in the arithmetic encoder (TCP mode) or nothing (UDP).

Current netc gaps vs Oodle:
1. **12B/pkt fixed overhead** (8B header + 4B ANS state) vs 0B for Oodle — 37.5% on 32B
2. **Frequency-based model** (tANS tables) vs **prediction-based model** (exact byte match
   via hash context) — frequency tables average over all training data and lose byte-level
   correlation; LZP preserves exact sequences
3. **41KB normalized frequencies** vs **megabytes of raw training bytes** — Oodle can predict
   any byte sequence that appeared in training; netc can only exploit aggregate statistics

Current benchmark (stateful mode, WL-001 through WL-007):
| WL | Size | netc ratio | oodle-udp ratio | Gap |
|----|------|-----------|-----------------|-----|
| WL-001 | 64B | 0.94 | 0.55 | 41% |
| WL-002 | 128B | 0.69 | 0.43 | 26% |
| WL-003 | 256B | 0.41 | 0.26 | 15% |
| WL-004 | 32B | 1.14 | 0.56 | 52% |
| WL-007 | 128B | 0.14 | 0.05 | 9% |

## What Changes

### Phase 1: Compact Header (depends on task `compact-packet-header`)
Already specified in the compact-packet-header task. Reduces 8B→2-3B overhead.
Expected gain: ~5-6B/pkt → biggest impact on WL-004 (32B).

### Phase 2: LZP Hash-Prediction Pre-Pass (this task, biggest impact)

Add an LZP prediction stage that runs BEFORE tANS encoding. For each byte,
look up the trained hash table to predict the byte. If the prediction is correct,
emit a single flag bit. If incorrect, emit a flag bit + the literal byte.
The result is a "prediction residual" stream that has much lower entropy than
the raw bytes, which tANS can then compress even more effectively.

#### 2.1 Training: Store raw byte sequences in hash table

Extend `netc_dict_t` with a hash-indexed dictionary of raw training bytes.
During `netc_dict_train()`, in addition to computing frequency tables,
store the raw training data in a hash table indexed by N-byte context:

```
hash(context[pos-ORDER..pos-1]) → predicted_byte[pos]
```

- ORDER = 3 (3 bytes of context → predict next byte)
- Hash table size: 2^17 = 131072 entries (matches Oodle's htbits=17)
- Each entry: 1 byte (predicted value) + 1 byte (confidence/valid flag)
- Total additional dictionary size: 131072 × 2 = 256KB
- Training builds the hash table from ALL training packets; on collision,
  keep the most frequent prediction (majority vote across training data)

#### 2.2 Compression: LZP prediction pre-pass

Before tANS encoding, run LZP prediction on the input:

```
For each byte at position pos:
  if pos >= ORDER:
    context = src[pos-ORDER .. pos-1]
    h = hash(context)
    predicted = dict->lzp_table[h].value
    if predicted == src[pos] AND dict->lzp_table[h].valid:
      emit flag=1 (match)
    else:
      emit flag=0 + src[pos] (miss + literal)
  else:
    emit flag=0 + src[pos] (no context yet, always literal)
```

Output format: bitstream of flags + byte stream of literals.
The flags compress extremely well with tANS (mostly 1s for good training data).
The literals are much rarer and also compress well because they represent
the "surprise" bytes that the model couldn't predict.

#### 2.3 New algorithm identifier

- `NETC_ALG_LZP` = new algorithm ID for LZP-compressed packets
- Wire format: `[header][lzp_payload]` where lzp_payload contains:
  - `[2B] n_literals` (uint16 LE): number of literal bytes
  - `[ceil(src_size/8) bytes] flag_bits`: packed bitstream, 1=match, 0=miss
  - `[n_literals bytes] literals`: the unpredicted bytes in order
- This format avoids interleaving bits and bytes, making decode fast

#### 2.4 Hybrid LZP+tANS

After LZP prediction, the flag bitstream and literal stream are concatenated
and fed to tANS for entropy coding. This gives the benefit of BOTH:
- LZP removes most of the byte values (matches are free)
- tANS compresses the remaining flags+literals using trained frequencies

The competition logic becomes:
```
lzp_output = lzp_predict(src, dict->lzp_table)
tans_of_lzp = tans_encode(lzp_output)
tans_of_raw = tans_encode(src)  // existing path
best = min(tans_of_lzp, tans_of_raw, lz77x, passthrough)
```

#### 2.5 Dictionary format v4

Bump `NETC_DICT_VERSION` from 3 to 4. New blob layout:
```
[0..7]      header (magic, version=4, model_id, ctx_count, flags, pad)
[8..8199]   unigram freq tables (16 × 256 × uint16)
[8200..40967] bigram freq tables (16 × 4 × 256 × uint16)
[40968..40971] lzp_table_size (uint32 LE) — number of hash entries
[40972..40972+lzp_table_size*2-1] lzp_table entries (value + valid)
[last 4]    checksum (CRC32)
```

The LZP table is optional — if `lzp_table_size == 0`, the dict has no LZP model
and the codec falls back to existing tANS-only compression. This preserves
backward compatibility with v3 dicts.

### Phase 3: ANS State Compaction (optional, lower priority)

Current tANS emits 4B initial_state per stream. Since ANS state range is
[4096, 8192), only 13 bits are needed. Compact to 2B with 3 reserved bits.
Saves ~2B/pkt on single-region, more on MREG.

## Impact
- Affected specs: RFC-001 Section 6 (Compression), Section 7 (Dictionary), Section 9 (Packet Format)
- Affected code: netc_internal.h, netc_dict.c, netc_compress.c, netc_decompress.c, netc_tans.h, include/netc.h
- Breaking change: NO (new algorithm is opt-in; v3 dicts continue to work without LZP)
- User benefit: Expected ratio improvement of 20-35% on small packets (64-256B),
  approaching Oodle parity on most workloads.

## Expected Results
| WL | Size | netc current | netc+LZP target | oodle-udp |
|----|------|-------------|-----------------|-----------|
| WL-001 | 64B | 0.94 | **0.60-0.65** | 0.55 |
| WL-002 | 128B | 0.69 | **0.45-0.50** | 0.43 |
| WL-003 | 256B | 0.41 | **0.28-0.33** | 0.26 |
| WL-004 | 32B | 1.14 | **0.70-0.80** | 0.56 |
| WL-007 | 128B | 0.14 | **0.08-0.12** | 0.05 |

With compact header (Phase 1) stacked on top, WL-001 and WL-003 should reach Oodle parity.
