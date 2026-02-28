/**
 * netc/Context.hpp — RAII compression context for the netc C++ SDK.
 *
 * Wraps netc_ctx_t* with move-only semantics and automatic cleanup.
 */

#pragma once

#include "Result.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

struct netc_ctx;

namespace netc {

class Dict;

enum class Mode : uint8_t {
    TCP = 0,   ///< Stateful — ring buffer accumulates history
    UDP = 1,   ///< Stateless — each packet is independent
};

enum class SimdLevel : uint8_t {
    Generic = 1,
    SSE42   = 2,
    AVX2    = 3,
    NEON    = 4,
};

struct Stats {
    uint64_t packets_compressed   = 0;
    uint64_t packets_decompressed = 0;
    uint64_t bytes_in             = 0;
    uint64_t bytes_out            = 0;
    uint64_t passthrough_count    = 0;

    /// Computed average ratio (bytes_out / bytes_in). Returns 0 if bytes_in == 0.
    double AverageRatio() const noexcept {
        if (bytes_in == 0) return 0.0;
        return static_cast<double>(bytes_out) / static_cast<double>(bytes_in);
    }
};

class Context final {
public:
    /// Create a compression context bound to a dictionary.
    /// The Dict must outlive this Context.
    /// extra_flags: additional NETC_CFG_FLAG_* bits (DELTA, BIGRAM, COMPACT_HDR, etc.)
    ///   Mode::TCP adds STATEFUL automatically; Mode::UDP adds STATELESS.
    ///   STATS flag is always added.
    explicit Context(
        const Dict& dict,
        Mode        mode        = Mode::TCP,
        uint8_t     level       = 5,
        uint32_t    extra_flags = 0);

    /// Move-only.
    Context(Context&& other) noexcept;
    Context& operator=(Context&& other) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ~Context();

    // -- Stateful compression --

    /// Compress a packet. dst is resized to the actual compressed size.
    /// Pre-reserve with MaxCompressedSize(src_size) to avoid reallocation.
    Result Compress(
        const uint8_t* src, size_t src_size,
        std::vector<uint8_t>& dst);

    /// Decompress a packet. dst is resized to the actual decompressed size.
    Result Decompress(
        const uint8_t* src, size_t src_size,
        std::vector<uint8_t>& dst);

    // -- Stateless compression --

    /// Stateless compress (no context state modified).
    static Result CompressStateless(
        const Dict& dict,
        const uint8_t* src, size_t src_size,
        std::vector<uint8_t>& dst);

    /// Stateless decompress.
    static Result DecompressStateless(
        const Dict& dict,
        const uint8_t* src, size_t src_size,
        std::vector<uint8_t>& dst);

    // -- Utilities --

    /// Maximum compressed output size for a given input size.
    static size_t MaxCompressedSize(size_t src_size) noexcept;

    /// Reset context state (ring buffer, sequence counter). Dict retained.
    void Reset();

    /// SIMD level active for this context.
    SimdLevel GetSimdLevel() const noexcept;

    /// Accumulated compression statistics.
    Result GetStats(Stats& out) const;

    /// True if the context holds a valid native handle.
    bool IsValid() const noexcept;

private:
    netc_ctx* native_ = nullptr;
};

} // namespace netc
