/**
 * netc/Result.hpp — Error result enum for the netc C++ SDK.
 *
 * Maps 1:1 to netc_result_t from the C API.
 */

#pragma once

#include <cstdint>

namespace netc {

enum class Result : int32_t {
    OK            =  0,
    NoMem         = -1,
    TooBig        = -2,
    Corrupt       = -3,
    DictInvalid   = -4,
    BufSmall      = -5,
    CtxNull       = -6,
    Unsupported   = -7,
    Version       = -8,
    InvalidArg    = -9,
};

/// Convert a Result to a human-readable string. Never returns nullptr.
const char* ResultToString(Result r) noexcept;

} // namespace netc
