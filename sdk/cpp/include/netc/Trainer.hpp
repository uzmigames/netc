/**
 * netc/Trainer.hpp — Dictionary trainer for the netc C++ SDK.
 *
 * Accumulates a packet corpus and produces a trained Dict.
 */

#pragma once

#include "Result.hpp"
#include "Dict.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace netc {

class Trainer final {
public:
    Trainer() = default;

    /// Add a single packet to the training corpus.
    void AddPacket(const uint8_t* data, size_t size);

    /// Add multiple packets at once.
    void AddPackets(const std::vector<std::vector<uint8_t>>& packets);

    /// Number of packets in the corpus.
    size_t GetCorpusCount() const noexcept;

    /// Train a dictionary from the accumulated corpus.
    /// model_id: 1-254.
    Result Train(uint8_t model_id, Dict& out_dict) const;

    /// Clear all accumulated packets.
    void Reset();

private:
    std::vector<std::vector<uint8_t>> corpus_;
};

} // namespace netc
