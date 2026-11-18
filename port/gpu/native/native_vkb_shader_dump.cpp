// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_shader_dump.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace Core::Vkb::ShaderDump {
namespace {

constexpr const char* StageNames[] = {"vs", "ps", "cs", "cs_async"};

struct Module {
    std::vector<u32> spv, gcn;
};
struct Shader {
    std::vector<Module> modules; ///< distinct SPIR-V of this hash (specialisations)
};

std::mutex mutex;
std::map<std::pair<u8, u64>, Shader> shaders;              ///< (stage, hash): kept at compile time
std::map<std::pair<u8, u64>, u64> uses;                   ///< (stage, hash): draws / dispatches since debugger mode started
std::unordered_map<u64, std::unordered_map<u64, u64>> pairs; ///< vertex hash -> pixel hash -> draws

void WriteFile(const std::filesystem::path& path, const std::vector<u32>& words) {
    if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(words.data(), sizeof(u32), words.size(), f);
        std::fclose(f);
    }
}

} // namespace

void Keep(Stage stage, u64 hash, std::span<const u32> spv, std::span<const u32> gcn) {
    std::scoped_lock lock{mutex};
    Shader& s = shaders[{static_cast<u8>(stage), hash}];
    for (const Module& m : s.modules) {
        if (m.spv.size() == spv.size() && std::equal(m.spv.begin(), m.spv.end(), spv.begin())) {
            return;
        }
    }
    s.modules.push_back({{spv.begin(), spv.end()}, {gcn.begin(), gcn.end()}});
}

void NoteDraw(u64 vs, u64 ps) {
    std::scoped_lock lock{mutex};
    ++uses[{static_cast<u8>(Stage::Vertex), vs}];
    if (ps != 0) {
        ++uses[{static_cast<u8>(Stage::Pixel), ps}];
    }
    ++pairs[vs][ps];
}

void NoteDispatch(u64 cs, bool async) {
    std::scoped_lock lock{mutex};
    ++uses[{static_cast<u8>(async ? Stage::AsyncCompute : Stage::Compute), cs}];
}

void Reset() {
    std::scoped_lock lock{mutex};
    uses.clear();
    pairs.clear();
}

nlohmann::json Write(const std::filesystem::path& dir) {
    std::scoped_lock lock{mutex};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    nlohmann::json list = nlohmann::json::array();
    u64 files = 0, bytes = 0;
    for (const auto& [key, count] : uses) {
        const auto [stage, hash] = key;
        nlohmann::json entry{{"stage", StageNames[stage]}, {"hash", fmt::format("{:016x}", hash)}, {"short", fmt::format("{:08x}", hash & 0xffffffffull)}, {"uses", count}};
        nlohmann::json names = nlohmann::json::array();
        if (const auto it = shaders.find(key); it != shaders.end()) {
            for (size_t n = 0; n < it->second.modules.size(); ++n) {
                const Module& m = it->second.modules[n];
                const std::string base = fmt::format("{}_{:016x}_{}", StageNames[stage], hash, n);
                WriteFile(dir / (base + ".spv"), m.spv);
                WriteFile(dir / (base + ".gcn"), m.gcn);
                names.push_back(base);
                files += 2;
                bytes += (m.spv.size() + m.gcn.size()) * sizeof(u32);
            }
        }
        entry["files"] = names; // empty: compiled before this build kept copies, or never compiled
        list.push_back(std::move(entry));
    }
    std::sort(list.begin(), list.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a["uses"].get<u64>() > b["uses"].get<u64>(); });
    nlohmann::json pair_list = nlohmann::json::array();
    for (const auto& [vs, by_ps] : pairs) {
        for (const auto& [ps, n] : by_ps) {
            pair_list.push_back({{"vs", fmt::format("{:016x}", vs)}, {"ps", ps ? fmt::format("{:016x}", ps) : std::string{"none"}}, {"draws", n}});
        }
    }
    std::sort(pair_list.begin(), pair_list.end(), [](const nlohmann::json& a, const nlohmann::json& b) { return a["draws"].get<u64>() > b["draws"].get<u64>(); });
    size_t kept = 0;
    for (const auto& [key, s] : shaders) {
        kept += s.modules.size();
    }
    nlohmann::json index{{"note", "every shader used while debugger mode recorded (Shift+F5 .. freeze), most used first: <stage>_<hash>_<n>.spv is our SPIR-V module (n: a "
                                  "specialisation of that hash), .gcn the PS4 program it was translated from; the draw table's 8-digit ps / vs hashes are the low half "
                                  "(short). pairs: vertex and pixel shader used together, by draws. Disassemble: spirv-dis <file>.spv"},
                         {"shaders_used", list.size()}, {"files_written", files}, {"mb_written", static_cast<double>(bytes) / (1 << 20)},
                         {"modules_kept_in_memory", kept}, {"shaders", list}, {"pairs", pair_list}};
    if (std::FILE* f = std::fopen((dir / "index.json").string().c_str(), "wb")) {
        const std::string text = index.dump(1);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    return index;
}

} // namespace Core::Vkb::ShaderDump
