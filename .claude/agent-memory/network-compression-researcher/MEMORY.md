# Network Compression Researcher - Memory

## Project: netc

### Key Architecture
- C11 library for compressing small network packets (8-65535 bytes)
- Primary codec: tANS (FSE) with trained dictionaries
- Header: `F:\Node\netc\src\core\netc_internal.h` - `netc_pkt_header_t` (8 bytes)
- Compress: `F:\Node\netc\src\core\netc_compress.c`
- Decompress: `F:\Node\netc\src\core\netc_decompress.c`
- Public API: `F:\Node\netc\include\netc.h`

### Critical Finding: Header Overhead
- Current 8B header: [2B orig_size][2B comp_size][1B flags][1B algo][1B model_id][1B ctx_seq]
- `compressed_size` is ALWAYS derivable from transport frame size - redundant field
- `model_id` is per-connection, not per-packet - redundant field
- `algorithm` is per-connection in practice - redundant field
- `context_seq` only needed in stateless delta mode
- Oodle Network uses 0B header (headerless codec) - confirmed in bench_oodle.c line 219
- Compact 2B header proposed: [1B control+flags][1B orig_size] for packets <= 255B
- See detailed research: `header-overhead-research.md`

### Oodle Benchmark Adapter
- `F:\Node\netc\bench\bench_oodle.c` - adds 4B rawLen prefix since Oodle is headerless
- CI gates: OODLE-01 (ratio), OODLE-02 (compress speed), OODLE-03 (decompress speed)

### Project Conventions
- AGENTS.md must be read first
- 95%+ test coverage required
- Edit files sequentially, never in parallel
- Tests must be complete, no placeholders
