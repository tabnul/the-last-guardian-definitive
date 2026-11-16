// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_side_census.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include "native/shadps4/core/guest_heap.h"
#include "native/native_vkb_util.h"
#include "native/shadps4/shader_recompiler/ir/program.h"

namespace Core::Vkb::SideCensus {
namespace {

constexpr u64 CtxBytes = 0x13200;
constexpr u64 ObjectBytes = 0x200;

struct Region {
    u64 address{};
    u64 bytes{};
    u8 index{}; ///< 0: the context; 1..6: argument a0..a5
    std::vector<u32> before;
};
struct Frame {
    const char* function{};
    std::vector<Region> regions;
};
thread_local std::vector<Frame> t_stack;

struct Entry {
    u64 changed{};
    u64 to_zero{};
    std::vector<std::pair<u32, u32>> samples; ///< old -> new, up to 4 distinct new values
};
std::mutex mutex;
std::map<std::string, u64> calls;
std::map<std::tuple<std::string, u8, u32>, Entry> entries;

void Snap(Frame& f, u64 address, u64 bytes, u8 index) {
    Region r{address, bytes, index, {}};
    r.before.resize(bytes / 4);
    std::memcpy(r.before.data(), reinterpret_cast<const void*>(address), bytes);
    f.regions.push_back(std::move(r));
}

} // namespace

bool On() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_SIDE_CENSUS"); return e != nullptr && e[0] == '1'; }();
    return on;
}

void Begin(const char* function, u64 ctx, std::initializer_list<u64> args) {
    Frame f;
    f.function = function;
    if (ctx != 0 && IsHeapPointer(ctx) && IsHeapPointer(ctx + CtxBytes - 1)) {
        Snap(f, ctx, CtxBytes, 0);
    }
    u8 index = 1;
    for (const u64 a : args) {
        // a heap object that is not the context itself (nor inside its snapshot)
        if (a != 0 && (a & 7) == 0 && IsHeapPointer(a) && IsHeapPointer(a + ObjectBytes - 1) && !(ctx != 0 && a >= ctx && a < ctx + CtxBytes)) {
            Snap(f, a, ObjectBytes, index);
        }
        ++index;
    }
    t_stack.push_back(std::move(f));
}

void End() {
    if (t_stack.empty()) {
        return;
    }
    Frame f = std::move(t_stack.back());
    t_stack.pop_back();
    std::scoped_lock lock{mutex};
    ++calls[f.function];
    for (const Region& r : f.regions) {
        const u32* now = reinterpret_cast<const u32*>(r.address);
        for (u32 i = 0; i < r.before.size(); ++i) {
            if (now[i] == r.before[i]) {
                continue;
            }
            Entry& e = entries[{f.function, r.index, i * 4}];
            ++e.changed;
            e.to_zero += now[i] == 0 ? 1 : 0;
            bool known = false;
            for (const auto& s : e.samples) {
                known = known || s.second == now[i];
            }
            if (!known && e.samples.size() < 4) {
                e.samples.emplace_back(r.before[i], now[i]);
            }
        }
    }
}

nlohmann::json Report() {
    std::scoped_lock lock{mutex};
    nlohmann::json j = nlohmann::json::object();
    j["on"] = On();
    for (const auto& [function, n] : calls) {
        nlohmann::json fn;
        fn["calls"] = n;
        nlohmann::json fields = nlohmann::json::array();
        for (const auto& [key, e] : entries) {
            if (std::get<0>(key) != function) {
                continue;
            }
            std::string samples;
            for (const auto& [from, to] : e.samples) {
                samples += fmt::format("{}{:#x}->{:#x}", samples.empty() ? "" : " ", from, to);
            }
            const u8 region = std::get<1>(key);
            fields.push_back(fmt::format("{}+{:#x}: {} of {} calls, to 0: {}; {}", region == 0 ? std::string("ctx") : fmt::format("a{}", region - 1), std::get<2>(key), e.changed, n,
                                         e.to_zero, samples));
        }
        fn["fields"] = std::move(fields);
        j[function] = std::move(fn);
    }
    return j;
}

} // namespace Core::Vkb::SideCensus

namespace Core::Vkb::FlatCensus {
namespace {
std::mutex flat_mutex;
std::map<std::string, u64> flat_lines;
/// The user-data input covering register `reg`: its type (0 resource, 1 sampler, 2 constant buffer, 4 UAV, 18 fetch shader, 19/21/22/25 descriptor tables, 23 vertex
/// buffer table, 26 internal table, 27 extended user data pointer), or 0xff (no input: a constant the program itself set, or none)
u32 InputAt(u64 gnm, u32 inputs_offset, u32 reg) {
    if (gnm == 0) {
        return 0xfe;
    }
    const u32 count = Util::Rd<u8>(gnm, 3);
    for (u32 i = 0; i < count && i < 40; ++i) {
        const u64 e = gnm + inputs_offset + i * 4;
        const u32 type = Util::Rd<u8>(e, 0), start = Util::Rd<u8>(e, 2), flags = Util::Rd<u8>(e, 3);
        const u32 size = (type == 0 || type == 4) ? ((flags & 1) ? 8 : 4) : (type == 1 || type == 2) ? 4 : 2;
        if (reg >= start && reg < start + size) {
            return type;
        }
    }
    return 0xff;
}
/// The lowest user-data register an address value is built from (0xffff: none)
u32 BaseRegister(const VkbShader::IR::Value& value, u32 depth) {
    if (value.IsImmediate() || depth > 4) {
        return 0xffff;
    }
    const VkbShader::IR::Inst* inst = value.TryInst();
    if (inst == nullptr) {
        return 0xffff;
    }
    if (inst->GetOpcode() == VkbShader::IR::Opcode::GetUserData) {
        return static_cast<u32>(inst->Arg(0).ScalarReg());
    }
    u32 best = 0xffff;
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        best = std::min(best, BaseRegister(inst->Arg(i), depth + 1));
    }
    return best;
}
} // namespace

bool On() {
    static const bool on = [] { const char* e = std::getenv("TLG_VKB_FLAT_CENSUS"); return e != nullptr && e[0] == '1'; }();
    return on;
}

void Note(const VkbShader::IR::Program& program, u64 gnm, u32 inputs_offset, const char* stage, u64 hash) {
    std::map<std::string, u64> lines;
    for (const VkbShader::IR::Block* block : program.blocks) {
        for (const VkbShader::IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == VkbShader::IR::Opcode::GetUserData) {
                const u32 reg = static_cast<u32>(inst.Arg(0).ScalarReg());
                ++lines[fmt::format("{} {:#x}: user data s{} (input type {})", stage, hash, reg, InputAt(gnm, inputs_offset, reg))];
            } else if (inst.GetOpcode() == VkbShader::IR::Opcode::ReadConst) {
                const u32 base = BaseRegister(inst.Arg(0), 0);
                ++lines[fmt::format("{} {:#x}: read through s{} (input type {}){}", stage, hash, base, base < 0xffff ? InputAt(gnm, inputs_offset, base) : 0xfe,
                                    inst.Flags<u32>() == 0 ? " dynamic" : "")];
            }
        }
    }
    std::scoped_lock lock{flat_mutex};
    for (auto& [line, n] : lines) {
        flat_lines[line] += n;
    }
}

nlohmann::json Report() {
    std::scoped_lock lock{flat_mutex};
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [line, n] : flat_lines) {
        j[line] = n;
    }
    return j;
}

} // namespace Core::Vkb::FlatCensus
