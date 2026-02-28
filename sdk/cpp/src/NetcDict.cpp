/**
 * NetcDict.cpp — netc::Dict implementation.
 */

#include "netc/Dict.hpp"

extern "C" {
#include "netc.h"
}

#include <cstdio>
#include <cstring>

namespace netc {

// -- Move semantics --

Dict::Dict(Dict&& other) noexcept : native_(other.native_) {
    other.native_ = nullptr;
}

Dict& Dict::operator=(Dict&& other) noexcept {
    if (this != &other) {
        netc_dict_free(native_);
        native_ = other.native_;
        other.native_ = nullptr;
    }
    return *this;
}

Dict::~Dict() {
    netc_dict_free(native_);
}

// -- Factory methods --

Result Dict::LoadFromBytes(
    const uint8_t* data, size_t size, Dict& out_dict)
{
    if (data == nullptr || size == 0) {
        return Result::InvalidArg;
    }
    netc_dict_t* raw = nullptr;
    netc_result_t r = netc_dict_load(data, size, &raw);
    if (r != NETC_OK) {
        return static_cast<Result>(r);
    }
    netc_dict_free(out_dict.native_);
    out_dict.native_ = raw;
    return Result::OK;
}

Result Dict::LoadFromFile(
    const std::string& file_path, Dict& out_dict)
{
    FILE* f = std::fopen(file_path.c_str(), "rb");
    if (f == nullptr) {
        return Result::InvalidArg;
    }

    std::fseek(f, 0, SEEK_END);
    long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        std::fclose(f);
        return Result::InvalidArg;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    size_t read = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    if (read != buf.size()) {
        return Result::InvalidArg;
    }

    return LoadFromBytes(buf.data(), buf.size(), out_dict);
}

// -- Serialization --

Result Dict::SaveToBytes(std::vector<uint8_t>& out_bytes) const {
    if (native_ == nullptr) {
        return Result::InvalidArg;
    }
    void* blob = nullptr;
    size_t blob_size = 0;
    netc_result_t r = netc_dict_save(native_, &blob, &blob_size);
    if (r != NETC_OK) {
        return static_cast<Result>(r);
    }
    out_bytes.resize(blob_size);
    std::memcpy(out_bytes.data(), blob, blob_size);
    netc_dict_free_blob(blob);
    return Result::OK;
}

Result Dict::SaveToFile(const std::string& file_path) const {
    std::vector<uint8_t> blob;
    Result r = SaveToBytes(blob);
    if (r != Result::OK) {
        return r;
    }

    FILE* f = std::fopen(file_path.c_str(), "wb");
    if (f == nullptr) {
        return Result::InvalidArg;
    }
    size_t written = std::fwrite(blob.data(), 1, blob.size(), f);
    std::fclose(f);

    if (written != blob.size()) {
        return Result::InvalidArg;
    }
    return Result::OK;
}

// -- Inspection --

bool Dict::IsValid() const noexcept {
    return native_ != nullptr;
}

uint8_t Dict::GetModelId() const noexcept {
    if (native_ == nullptr) return 0;
    return netc_dict_model_id(native_);
}

} // namespace netc
