# Network Compression Researcher - Memory

## Project: netc

### Key Architecture
- C11 library for compressing small network packets (8-65535 bytes)
- Primary codec: tANS (FSE) with trained dictionaries
- Header: `F:\Node\netc\src\core\netc_internal.h` - `netc_pkt_header_t` (8 bytes)
- Compress: `F:\Node\netc\src\core\netc_compress.c`
- Decompress: `F:\Node\netc\src\core\netc_decompress.c`
- Public API: `F:\Node\netc\include\netc.h`
- Dict training: `F:\Node\netc\src\core\netc_dict.c`
- tANS engine: `F:\Node\netc\src\algo\netc_tans.c` / `netc_tans.h`
- LZP codec: `F:\Node\netc\src\algo\netc_lzp.h` (header-only)

### Current Pipeline (as of 2026-02-28)
1. Delta prediction (XOR with prev packet, when enabled + matching sizes)
2. LZP XOR pre-filter (position-aware order-1: hash(prev_byte, offset))
3. tANS entropy coding (TABLE_LOG=12, 4096 states, 16 context buckets)
4. Competition: tANS vs LZ77 vs LZ77X (cross-packet) vs passthrough
5. Compact header: 2B for packets <= 127B, 4B otherwise
6. ANS state: 2B compacted from 4B (state range [4096,8192) = 13 bits)

### Current Ratios vs Oodle (2026-02-28)
- WL-001 64B: netc 0.783 vs oodle 0.68 (gap ~6.6B)
- WL-002 128B: netc 0.626 vs oodle 0.52 (gap ~13.6B)
- WL-003 256B: netc 0.381 vs oodle 0.35 (gap ~7.9B)

### Critical Finding: Header Overhead
- Current 4B minimum (2B compact hdr + 2B ANS state) vs Oodle 0B
- See detailed research: `header-overhead-research.md`

### Research Findings (2026-02-28)
- See `ratio-gap-research.md` for 6-technique analysis to close Oodle gap
- Top opportunities: (1) eliminate ANS state, (2) order-2 context, (3) nibble coding

### Oodle Benchmark Adapter
- `F:\Node\netc\bench\bench_oodle.c` - adds 4B rawLen prefix since Oodle is headerless
- CI gates: OODLE-01 (ratio), OODLE-02 (compress speed), OODLE-03 (decompress speed)

### Project Conventions
- AGENTS.md must be read first
- 95%+ test coverage required
- Edit files sequentially, never in parallel
- Tests must be complete, no placeholders
