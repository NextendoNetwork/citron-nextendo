// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace Tegra::Decoder {

// Reconstruct the VP9 uncompressed segmentation header from NVDEC state.
// NVDEC supplies complete feature state rather than a bitstream update_data
// flag. Re-emitting that state is equivalent even when the source retained it.
template <class Writer, class Segmentation>
void WriteVp9Segmentation(Writer& writer, const Segmentation& segment,
                          const std::array<std::uint8_t, 7>& tree_probs,
                          const std::array<std::uint8_t, 3>& pred_probs) {
    writer.WriteBit(segment.enabled != 0);
    if (!segment.enabled) {
        return;
    }
    writer.WriteBit(segment.update_map != 0);
    const auto write_probability = [&](std::uint8_t probability) {
        writer.WriteBit(probability != 255);
        if (probability != 255) {
            writer.WriteU(probability, 8);
        }
    };
    if (segment.update_map) {
        for (auto probability : tree_probs) {
            write_probability(probability);
        }
        writer.WriteBit(segment.temporal_update != 0);
        if (segment.temporal_update) {
            for (auto probability : pred_probs) {
                write_probability(probability);
            }
        }
    }
    writer.WriteBit(true); // Complete feature state follows.
    writer.WriteBit(segment.abs_delta != 0);
    constexpr std::array<unsigned, 4> widths{8, 6, 2, 0};
    constexpr std::array<int, 4> maxima{255, 63, 3, 0};
    for (std::size_t index = 0; index < 8; ++index) {
        for (std::size_t feature = 0; feature < 4; ++feature) {
            const bool enabled = (segment.feature_mask[index] & (1u << feature)) != 0;
            writer.WriteBit(enabled);
            if (!enabled) {
                continue;
            }
            const int value = segment.feature_data[index][feature];
            if (feature < 2) {
                writer.WriteS(std::clamp(value, -maxima[feature], maxima[feature]),
                              widths[feature]);
            } else if (widths[feature]) {
                writer.WriteU(std::clamp(value, 0, maxima[feature]), widths[feature]);
            }
        }
    }
}

} // namespace Tegra::Decoder
