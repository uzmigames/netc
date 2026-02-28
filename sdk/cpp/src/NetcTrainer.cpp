/**
 * NetcTrainer.cpp — netc::Trainer implementation.
 */

#include "netc/Trainer.hpp"

extern "C" {
#include "netc.h"
}

namespace netc {

void Trainer::AddPacket(const uint8_t* data, size_t size) {
    if (data != nullptr && size > 0) {
        corpus_.emplace_back(data, data + size);
    }
}

void Trainer::AddPackets(const std::vector<std::vector<uint8_t>>& packets) {
    for (const auto& pkt : packets) {
        if (!pkt.empty()) {
            corpus_.push_back(pkt);
        }
    }
}

size_t Trainer::GetCorpusCount() const noexcept {
    return corpus_.size();
}

Result Trainer::Train(uint8_t model_id, Dict& out_dict) const {
    if (corpus_.empty()) {
        return Result::InvalidArg;
    }

    std::vector<const uint8_t*> ptrs(corpus_.size());
    std::vector<size_t> sizes(corpus_.size());
    for (size_t i = 0; i < corpus_.size(); i++) {
        ptrs[i]  = corpus_[i].data();
        sizes[i] = corpus_[i].size();
    }

    netc_dict_t* raw = nullptr;
    netc_result_t r = netc_dict_train(
        ptrs.data(), sizes.data(), corpus_.size(), model_id, &raw);
    if (r != NETC_OK) {
        return static_cast<Result>(r);
    }

    /* Transfer ownership (Trainer is a friend of Dict). */
    netc_dict_free(out_dict.native_);
    out_dict.native_ = raw;
    return Result::OK;
}

void Trainer::Reset() {
    corpus_.clear();
}

} // namespace netc
