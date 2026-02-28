/**
 * netc/Dict.hpp — RAII dictionary wrapper for the netc C++ SDK.
 *
 * Wraps netc_dict_t* with move-only semantics and automatic cleanup.
 */

#pragma once

#include "Result.hpp"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct netc_dict;

namespace netc {

class Dict final {
public:
    /// Default-constructs an empty (invalid) dictionary.
    Dict() noexcept = default;

    /// Move constructor — transfers ownership.
    Dict(Dict&& other) noexcept;
    Dict& operator=(Dict&& other) noexcept;

    /// Non-copyable.
    Dict(const Dict&) = delete;
    Dict& operator=(const Dict&) = delete;

    /// Destructor — calls netc_dict_free.
    ~Dict();

    // -- Factory methods --

    /// Load from a binary blob in memory.
    static Result LoadFromBytes(
        const uint8_t* data, size_t size, Dict& out_dict);

    /// Load from a file on disk.
    static Result LoadFromFile(
        const std::string& file_path, Dict& out_dict);

    // -- Serialization --

    /// Serialize to a binary blob.
    Result SaveToBytes(std::vector<uint8_t>& out_bytes) const;

    /// Save to a file on disk.
    Result SaveToFile(const std::string& file_path) const;

    // -- Inspection --

    /// True if the dictionary holds a valid trained model.
    bool IsValid() const noexcept;

    /// Returns the model_id (1-254), or 0 if invalid.
    uint8_t GetModelId() const noexcept;

    /// Access the underlying C handle.
    const netc_dict* GetNativeDict() const noexcept { return native_; }

private:
    friend class Trainer;
    netc_dict* native_ = nullptr;
};

} // namespace netc
