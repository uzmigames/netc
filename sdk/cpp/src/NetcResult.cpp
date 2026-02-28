/**
 * NetcResult.cpp — netc::Result string conversion.
 */

#include "netc/Result.hpp"

extern "C" {
#include "netc.h"
}

namespace netc {

const char* ResultToString(Result r) noexcept {
    return netc_strerror(static_cast<netc_result_t>(static_cast<int32_t>(r)));
}

} // namespace netc
