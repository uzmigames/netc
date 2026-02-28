# Proposal: Implement ANS Codec (Core Compression Engine)

## Why

The primary compression algorithm for netc is Asymmetric Numeral Systems (ANS), specifically rANS for packets > 64 bytes and tANS for packets ≤ 64 bytes. ANS achieves near-optimal entropy coding with fractional-bit precision, outperforming Huffman by 5–15% on skewed byte distributions typical of game/simulation packets. This is the core compression engine — without it, netc cannot achieve its RFC-002 performance targets.

## What Changes

- Implement rANS encoder/decoder (src/algo/netc_ans.c)
- Implement tANS encoder/decoder with 12-bit table (4096 entries)
- Implement dictionary training (byte frequency tables, ANS probability normalization)
- Implement CRC32 for dictionary validation (src/util/netc_crc32.c)
- Implement bitstream I/O (src/util/netc_bitstream.c)
- Integrate ANS into netc_compress/netc_decompress pipeline
- Dictionary serialization/deserialization (netc_dict_save, netc_dict_load)

## Impact

- Affected specs: algo/spec.md (new), dict/spec.md (new)
- Affected code: src/algo/netc_ans.c (new), src/util/netc_crc32.c (new), src/util/netc_bitstream.c (new), src/core/netc_dict.c (new), src/core/netc_compress.c (updated), src/core/netc_decompress.c (updated)
- Breaking change: NO (adds functionality to existing passthrough baseline)
- User benefit: Core compression functionality — enables sub-0.55 compression ratio on structured packets
