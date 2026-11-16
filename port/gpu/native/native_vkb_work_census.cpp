// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_work_census.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include "native/native_draw.h"

namespace Core::Vkb::WorkCensus {
namespace {
struct Count {
    u64 calls{};
    u64 bytes{};
};
std::mutex mutex;
std::map<std::tuple<std::string, std::string, u64>, Count> counts; ///< scope, kind, Ghidra site -> count
thread_local const char* t_scope = nullptr;
} // namespace

bool On() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_WORK_CENSUS"); return e != nullptr && e[0] == '1'; }();
    return on;
}

void Note(const char* kind, u64 ret, u64 bytes) {
    if (!On()) {
        return;
    }
    const u64 base = Core::NativeDraw::ImageBase();
    const u64 site = base != 0 && ret >= base ? ret - base + 0x1000000 : 0;
    std::scoped_lock lock{mutex};
    Count& c = counts[{t_scope != nullptr ? t_scope : "", kind, site}];
    ++c.calls;
    c.bytes += bytes;
}

Scope::Scope(const char* name) {
    if (!On()) {
        return;
    }
    active = true;
    previous = t_scope;
    t_scope = name;
}

Scope::~Scope() {
    if (active) {
        t_scope = previous;
    }
}

nlohmann::json Report() {
    nlohmann::json out = nlohmann::json::array();
    if (!On()) {
        return out;
    }
    std::vector<std::pair<std::tuple<std::string, std::string, u64>, Count>> rows;
    {
        std::scoped_lock lock{mutex};
        rows.assign(counts.begin(), counts.end());
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.calls > b.second.calls; });
    for (const auto& [key, c] : rows) {
        out.push_back({{"inside", std::get<0>(key)}, {"kind", std::get<1>(key)}, {"site", fmt::format("{:#x}", std::get<2>(key))}, {"calls", c.calls}, {"bytes", c.bytes}});
    }
    return out;
}

} // namespace Core::Vkb::WorkCensus
