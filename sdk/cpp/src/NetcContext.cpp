/**
 * NetcContext.cpp — netc::Context implementation.
 */

#include "netc/Context.hpp"
#include "netc/Dict.hpp"

extern "C" {
#include "netc.h"
}

namespace netc {

// -- Constructor --

Context::Context(
    const Dict& dict,
    Mode        mode,
    uint8_t     level,
    uint32_t    extra_flags)
{
    if (!dict.IsValid()) return;

    netc_cfg_t cfg;
    cfg.flags = extra_flags | NETC_CFG_FLAG_STATS;
    if (mode == Mode::TCP) {
        cfg.flags |= NETC_CFG_FLAG_STATEFUL;
    } else {
        cfg.flags |= NETC_CFG_FLAG_STATELESS;
    }
    cfg.ring_buffer_size  = 0;
    cfg.compression_level = level;
    cfg.simd_level        = 0;
    cfg.arena_size        = 0;

    native_ = netc_ctx_create(dict.GetNativeDict(), &cfg);
}

// -- Move semantics --

Context::Context(Context&& other) noexcept : native_(other.native_) {
    other.native_ = nullptr;
}

Context& Context::operator=(Context&& other) noexcept {
    if (this != &other) {
        netc_ctx_destroy(native_);
        native_ = other.native_;
        other.native_ = nullptr;
    }
    return *this;
}

Context::~Context() {
    netc_ctx_destroy(native_);
}

// -- Stateful compression --

Result Context::Compress(
    const uint8_t* src, size_t src_size,
    std::vector<uint8_t>& dst)
{
    if (native_ == nullptr) return Result::CtxNull;

    size_t bound = netc_compress_bound(src_size);
    if (dst.size() < bound) {
        dst.resize(bound);
    }

    size_t dst_size = 0;
    netc_result_t r = netc_compress(
        native_, src, src_size, dst.data(), dst.size(), &dst_size);
    if (r != NETC_OK) {
        dst.clear();
        return static_cast<Result>(r);
    }
    dst.resize(dst_size);
    return Result::OK;
}

Result Context::Decompress(
    const uint8_t* src, size_t src_size,
    std::vector<uint8_t>& dst)
{
    if (native_ == nullptr) return Result::CtxNull;

    if (dst.size() < NETC_MAX_PACKET_SIZE) {
        dst.resize(NETC_MAX_PACKET_SIZE);
    }

    size_t dst_size = 0;
    netc_result_t r = netc_decompress(
        native_, src, src_size, dst.data(), dst.size(), &dst_size);
    if (r != NETC_OK) {
        dst.clear();
        return static_cast<Result>(r);
    }
    dst.resize(dst_size);
    return Result::OK;
}

// -- Stateless compression --

Result Context::CompressStateless(
    const Dict& dict,
    const uint8_t* src, size_t src_size,
    std::vector<uint8_t>& dst)
{
    if (!dict.IsValid()) return Result::InvalidArg;

    size_t bound = netc_compress_bound(src_size);
    if (dst.size() < bound) {
        dst.resize(bound);
    }

    size_t dst_size = 0;
    netc_result_t r = netc_compress_stateless(
        dict.GetNativeDict(), src, src_size, dst.data(), dst.size(), &dst_size);
    if (r != NETC_OK) {
        dst.clear();
        return static_cast<Result>(r);
    }
    dst.resize(dst_size);
    return Result::OK;
}

Result Context::DecompressStateless(
    const Dict& dict,
    const uint8_t* src, size_t src_size,
    std::vector<uint8_t>& dst)
{
    if (!dict.IsValid()) return Result::InvalidArg;

    if (dst.size() < NETC_MAX_PACKET_SIZE) {
        dst.resize(NETC_MAX_PACKET_SIZE);
    }

    size_t dst_size = 0;
    netc_result_t r = netc_decompress_stateless(
        dict.GetNativeDict(), src, src_size, dst.data(), dst.size(), &dst_size);
    if (r != NETC_OK) {
        dst.clear();
        return static_cast<Result>(r);
    }
    dst.resize(dst_size);
    return Result::OK;
}

// -- Utilities --

size_t Context::MaxCompressedSize(size_t src_size) noexcept {
    return netc_compress_bound(src_size);
}

void Context::Reset() {
    if (native_ != nullptr) {
        netc_ctx_reset(native_);
    }
}

SimdLevel Context::GetSimdLevel() const noexcept {
    if (native_ == nullptr) return SimdLevel::Generic;
    uint8_t level = netc_ctx_simd_level(native_);
    if (level < 1 || level > 4) return SimdLevel::Generic;
    return static_cast<SimdLevel>(level);
}

Result Context::GetStats(Stats& out) const {
    if (native_ == nullptr) return Result::CtxNull;

    netc_stats_t raw;
    netc_result_t r = netc_ctx_stats(native_, &raw);
    if (r != NETC_OK) {
        return static_cast<Result>(r);
    }

    out.packets_compressed   = raw.packets_compressed;
    out.packets_decompressed = raw.packets_decompressed;
    out.bytes_in             = raw.bytes_in;
    out.bytes_out            = raw.bytes_out;
    out.passthrough_count    = raw.passthrough_count;
    return Result::OK;
}

bool Context::IsValid() const noexcept {
    return native_ != nullptr;
}

} // namespace netc
