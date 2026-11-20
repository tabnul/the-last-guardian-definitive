// SPDX-License-Identifier: GPL-2.0-or-later

#include <nlohmann/json.hpp>
#include "native/native_updates.h"

namespace Core::NativeUpdates {

namespace {
std::array<std::atomic<u64>, B8Counts> b8_counts{};
} // namespace

void NoteB8(B8Count what) noexcept {
    b8_counts[what].fetch_add(1, std::memory_order_relaxed);
}

nlohmann::json Report() {
    nlohmann::json counts = nlohmann::json::array();
    for (const auto& c : b8_counts) {
        counts.push_back(c.load(std::memory_order_relaxed));
    }
    return {{"b8_counts_by_enum", std::move(counts)}, {"note", "writer announcements deleted 2026-11-20 (backend note 102)"}};
}

} // namespace Core::NativeUpdates
