// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "native/native_draw.h"
#include "native/native_log.h"
#include "native/native_timeline.h"
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include "native/native_tsharp_watch.h"
#include "native/native_debug_server.h"
#include "common/path_util.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Core::NativeTsharpWatch {

#ifdef _WIN32
namespace {

constexpr u32 MaxWatches = 4;
struct Watch {
    std::atomic<u64> address{};
    std::atomic<u32> last{};
    u64 view_object{};
    std::atomic<u32> hits{};
};
std::array<Watch, MaxWatches> g_watch;
std::atomic<u32> g_armed{};
std::atomic<bool> g_handler{};

/// Debugger mode: every write the watches catch, kept for the report (the log lines stop after 24 per watch)
struct Hit {
    u64 ns;
    u64 address;
    u64 rip;        ///< raw instruction address
    u64 rva;        ///< Ghidra address when the instruction is in the game image, else 0
    u64 returns[6]; ///< Ghidra addresses of the return addresses found on the stack
    u64 stack[32];  ///< the first stack words (raw), resolved to modules in the report
    u32 thread;
    u32 slot;
};
constexpr u32 HitCapacity = 256;
std::array<Hit, HitCapacity> g_hits{};
std::atomic<u64> g_hit_count{};
std::array<std::string, MaxWatches> g_watch_tag;

/// S1b census: every write the watches catch, counted by (watched address, instruction, first two game return addresses)
struct AggEntry {
    std::atomic<u64> key{0};
    u64 address{};
    u64 rip{};
    u64 rva{};
    u64 ret0{};
    u64 ret1{};
    u64 ret2{};
    std::atomic<u32> count{0};
};
constexpr u32 AggCapacity = 8192;
std::array<AggEntry, AggCapacity> g_agg{};
std::atomic<bool> g_agg_on{false};
void AggNote(u64 address, u64 rip, u64 rva, const u64* returns) {
    u64 key = address * 0x9E3779B97F4A7C15ull ^ rip * 0xC2B2AE3D27D4EB4Full ^ returns[0] * 0x165667B19E3779F9ull ^ returns[1];
    key |= 1;
    for (u32 probe = 0; probe < 64; ++probe) {
        AggEntry& e = g_agg[(key + probe * 0x9E37u) % AggCapacity];
        u64 seen = e.key.load(std::memory_order_acquire);
        if (seen == 0) {
            if (e.key.compare_exchange_strong(seen, key, std::memory_order_acq_rel)) {
                e.address = address;
                e.rip = rip;
                e.rva = rva;
                e.ret0 = returns[0];
                e.ret1 = returns[1];
                e.ret2 = returns[2];
                e.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (seen == key) {
            e.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    const u64 dr6 = ctx->Dr6;
    if ((dr6 & 0xF) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    for (u32 i = 0; i < MaxWatches; ++i) {
        if ((dr6 & (1ull << i)) == 0) {
            continue;
        }
        Watch& w = g_watch[i];
        const u64 address = w.address.load();
        {
            const u64 base = Core::NativeDraw::ImageBase();
            Hit& hit = g_hits[g_hit_count.fetch_add(1) % HitCapacity];
            hit = {};
            hit.ns = Core::FrameTimeline::Now();
            hit.address = address;
            hit.rip = ctx->Rip;
            hit.rva = ctx->Rip > base && ctx->Rip < base + 0x6000000 ? ctx->Rip - base + 0x1000000 : 0;
            hit.thread = GetCurrentThreadId();
            hit.slot = i;
            const u64* sp = reinterpret_cast<const u64*>(ctx->Rsp);
            u32 found = 0;
            for (u32 k = 0; k < 160 && found < 6; ++k) {
                u64 v = 0;
                __try {
                    v = sp[k];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (k < 32) {
                    hit.stack[k] = v;
                }
                if (v > base && v < base + 0x6000000) {
                    hit.returns[found++] = v - base + 0x1000000;
                }
            }
            Core::FrameTimeline::Record(Core::FrameTimeline::WriterHit, address, hit.ns, hit.rva | (hit.returns[0] << 32));
            if (g_agg_on.load(std::memory_order_relaxed)) {
                AggNote(address, hit.rip, hit.rva, hit.returns);
                ctx->Dr6 = 0;
                continue; // (the census: no log line per write)
            }
        }
        u32 now = 0;
        std::memcpy(&now, reinterpret_cast<const void*>(address), sizeof(now));
        const u32 before = w.last.exchange(now);
        const u32 count = w.hits.fetch_add(1);
        if (count < 24) {
            const u64 base = Core::NativeDraw::ImageBase();
            char text[1024];
            int n = std::snprintf(text, sizeof(text), "tsharp watch %u: view %#llx dword %#x -> %#x (height %u -> %u) rip %#llx thread %lu returns:", i,
                                  static_cast<unsigned long long>(w.view_object), before, now, ((before >> 14) & 0x3fff) + 1, ((now >> 14) & 0x3fff) + 1,
                                  static_cast<unsigned long long>(ctx->Rip - base + 0x1000000), GetCurrentThreadId());
            // return addresses: every stack word that points into the game image
            const u64* sp = reinterpret_cast<const u64*>(ctx->Rsp);
            u32 found = 0;
            for (u32 k = 0; k < 160 && found < 8 && n < static_cast<int>(sizeof(text)) - 24; ++k) {
                u64 v = 0;
                __try {
                    v = sp[k];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
                if (v > base && v < base + 0x6000000) {
                    n += std::snprintf(text + n, sizeof(text) - n, " %#llx", static_cast<unsigned long long>(v - base + 0x1000000));
                    ++found;
                }
            }
            TLG_LOG(IMAGE, Error, "{}", text);
        }
    }
    ctx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION; // a data breakpoint traps after the write, nothing to skip
}

/// Sets the debug register `slot` of every thread of the process except the helper (done from a helper thread that suspends the others, the caller included)
void SetDebugRegisters(u32 slot, u64 address) {
    const DWORD pid = GetCurrentProcessId();
    std::thread([=] {
        const DWORD self = GetCurrentThreadId();
        std::vector<DWORD> ids;
        ids.reserve(512);
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 e{};
            e.dwSize = sizeof(e);
            for (bool ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
                if (e.th32OwnerProcessID == pid && e.th32ThreadID != self) {
                    ids.push_back(e.th32ThreadID);
                }
            }
            CloseHandle(snap);
        }
        std::vector<HANDLE> handles;
        handles.reserve(ids.size());
        for (const DWORD id : ids) {
            if (const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id)) {
                handles.push_back(h);
            }
        }
        for (const HANDLE h : handles) {
            SuspendThread(h);
        }
        for (const HANDLE h : handles) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (!GetThreadContext(h, &c)) {
                continue;
            }
            (&c.Dr0)[slot] = address;
            // local enable bit of the slot, condition 01 (write), length 11 (4 bytes)
            c.Dr7 &= ~((3ull << (slot * 2)) | (0xFull << (16 + slot * 4)));
            c.Dr7 |= (1ull << (slot * 2)) | (0x1ull << (16 + slot * 4)) | (0x3ull << (18 + slot * 4));
            SetThreadContext(h, &c);
        }
        for (const HANDLE h : handles) {
            ResumeThread(h);
            CloseHandle(h);
        }
    }).join();
}

} // namespace

bool On() noexcept {
    static const bool on = [] {
        const char* e = std::getenv("TLG_TSHARP_WATCH");
        return e != nullptr && *e == '1';
    }();
    return on;
}

void Arm(u64 view_object, u64 address, u32 width, u32 height) {
    if (!On() || width != 1920 || height != 1088) {
        return;
    }
    const u32 slot = g_armed.fetch_add(1);
    if (slot >= MaxWatches) {
        return;
    }
    if (!g_handler.exchange(true)) {
        AddVectoredExceptionHandler(1, Handler);
    }
    Watch& w = g_watch[slot];
    w.view_object = view_object;
    u32 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    w.last.store(value);
    w.address.store(address);
    SetDebugRegisters(slot, address);
    TLG_LOG(IMAGE, Error, "tsharp watch {} armed: view {:#x} dword at {:#x} = {:#x} (height {})", slot, view_object, address, value, ((value >> 14) & 0x3fff) + 1);
}

void ArmAddress(u64 address, const char* tag) {
    // W1.5 census: a data breakpoint on any dword (logs the writing instruction and the game return addresses, Ghidra addresses)
    address &= ~u64{3};
    const u32 slot = g_armed.fetch_add(1);
    if (slot >= MaxWatches) {
        return;
    }
    if (!g_handler.exchange(true)) {
        AddVectoredExceptionHandler(1, Handler);
    }
    Watch& w = g_watch[slot];
    w.view_object = 0;
    u32 value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    w.last.store(value);
    w.address.store(address);
    SetDebugRegisters(slot, address);
    TLG_LOG(IMAGE, Error, "write watch {} armed ({}): dword at {:#x} = {:#x}", slot, tag, address, value);
}

void ArmRotating(u64 address, const char* tag) {
    // debugger mode: the watches are reused in turn, armed from a thread of its own (arming suspends every thread of the process for a moment)
    address &= ~u64{3};
    const u32 slot = g_armed.fetch_add(1) % MaxWatches;
    if (!g_handler.exchange(true)) {
        AddVectoredExceptionHandler(1, Handler);
    }
    Watch& w = g_watch[slot];
    w.view_object = 0;
    w.hits.store(0);
    u32 value = 0;
    __try {
        std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    w.last.store(value);
    w.address.store(address);
    g_watch_tag[slot] = tag;
    std::thread([slot, address] { SetDebugRegisters(slot, address); }).detach();
}

/// "module.dll+0xoffset" when the address is inside a loaded module's image, else empty
std::string ModuleOf(u64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) || module == nullptr) {
        return {};
    }
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    return fmt::format("{}+{:#x}", name != nullptr ? name + 1 : path, address - reinterpret_cast<u64>(module));
}

std::string HitsJson() {
    nlohmann::json j = nlohmann::json::array();
    const u64 total = g_hit_count.load();
    const u64 first = total > HitCapacity ? total - HitCapacity : 0;
    for (u64 n = first; n < total; ++n) {
        const Hit& h = g_hits[n % HitCapacity];
        nlohmann::json e;
        e["ns"] = h.ns;
        e["address"] = fmt::format("{:#x}", h.address);
        e["rip"] = fmt::format("{:#x}", h.rip);
        e["rip_module"] = ModuleOf(h.rip);
        nlohmann::json chain = nlohmann::json::array();
        for (const u64 v : h.stack) {
            const std::string m = v > 0x10000 ? ModuleOf(v) : std::string();
            if (!m.empty() && chain.size() < 10) {
                chain.push_back(m);
            }
        }
        e["stack_code_addresses"] = chain;
        e["game_instruction"] = h.rva != 0 ? fmt::format("{:#x}", h.rva) : std::string("(host code, not the game image)");
        nlohmann::json r = nlohmann::json::array();
        for (const u64 v : h.returns) {
            if (v != 0) {
                r.push_back(fmt::format("{:#x}", v));
            }
        }
        e["returns"] = r;
        e["thread"] = h.thread;
        e["watch_tag"] = g_watch_tag[h.slot % MaxWatches];
        j.push_back(e);
    }
    return j.dump();
}

namespace {
struct CtxField {
    u32 offset;
    const char* name;
};
// the draw context's fields the hook and the record read (native_draw.cpp, namespace Ctx and MakeRecord); tables: slot 0 of the vertex (0) and pixel (1) stages
constexpr CtxField CtxFields[] = {
    {0x58, "index buffer view"},        {0x60, "index format"},          {0x64, "index type"},          {0x68, "input layout"},
    {0x70, "vertex buffer slot 0"},     {0x170, "vertex stride 0"},      {0x1f0, "vertex offset 0"},    {0x280, "constant buffer VS slot 0"},
    {0x300, "constant buffer PS slot 0"}, {0x308, "constant buffer PS slot 1"}, {0x580, "shader resource VS slot 0"}, {0x980, "shader resource PS slot 0"},
    {0x988, "shader resource PS slot 1"}, {0x1d80, "sampler VS slot 0"},  {0x1e00, "sampler PS slot 0"},  {0x4480, "topology"},
    {0x4484, "render target count"},    {0x4488, "render target 0"},     {0x44d0, "depth stencil view"}, {0x44ec, "viewport"},
    {0x4690, "pixel shader"},           {0x4698, "vertex shader"},       {0x46c8, "rasterizer state"},  {0x46e0, "depth stencil state"},
    {0x46e8, "stencil ref"},            {0x46f0, "blend state"},         {0x46f8, "blend factor"},      {0x4724, "instance count"},
    {0x4670, "compute shader"},         {0x4678, "domain shader"},       {0x4680, "geometry shader"},   {0x4688, "hull shader"},
};
std::mutex g_ctx_mutex;
std::array<std::pair<u64, u64>, 16> g_ctx_counts{};
std::atomic<bool> g_census_started{false};

void DisarmAll() {
    const DWORD pid = GetCurrentProcessId();
    std::thread([=] {
        const DWORD self = GetCurrentThreadId();
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            return;
        }
        THREADENTRY32 e{};
        e.dwSize = sizeof(e);
        for (bool ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
            if (e.th32OwnerProcessID != pid || e.th32ThreadID == self) {
                continue;
            }
            if (const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, e.th32ThreadID)) {
                SuspendThread(h);
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(h, &c)) {
                    c.Dr7 = 0;
                    SetThreadContext(h, &c);
                }
                ResumeThread(h);
                CloseHandle(h);
            }
        }
        CloseHandle(snap);
    }).join();
}

void RunCtxCensus(u64 ctx, std::vector<std::pair<u64, u64>> contexts) {
    if (!g_handler.exchange(true)) {
        AddVectoredExceptionHandler(1, Handler);
    }
    g_agg_on.store(true);
    constexpr u32 Count = sizeof(CtxFields) / sizeof(CtxFields[0]);
    for (u32 first = 0; first < Count; first += MaxWatches) {
        for (u32 s = 0; s < MaxWatches; ++s) {
            const u64 address = first + s < Count ? ctx + CtxFields[first + s].offset : 0;
            g_watch[s].address.store(address);
            g_watch[s].hits.store(0);
            g_watch_tag[s] = first + s < Count ? CtxFields[first + s].name : "";
            if (address != 0) {
                SetDebugRegisters(s, address);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        DisarmAll();
    }
    g_agg_on.store(false);
    // the result
    nlohmann::json out;
    out["ctx"] = fmt::format("{:#x}", ctx);
    nlohmann::json ctxs = nlohmann::json::array();
    for (const auto& [c, n] : contexts) {
        ctxs.push_back({{"ctx", fmt::format("{:#x}", c)}, {"tags", n}});
    }
    out["contexts_seen_by_the_draw_hook"] = ctxs;
    out["seconds_per_field"] = 2;
    nlohmann::json fields = nlohmann::json::array();
    for (u32 f = 0; f < Count; ++f) {
        const u64 address = ctx + CtxFields[f].offset;
        std::vector<const AggEntry*> rows;
        for (const auto& e : g_agg) {
            if (e.key.load() != 0 && e.address == (address & ~u64{3})) {
                rows.push_back(&e);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const AggEntry* a, const AggEntry* b) { return a->count.load() > b->count.load(); });
        u64 total = 0;
        nlohmann::json writers = nlohmann::json::array();
        for (const AggEntry* e : rows) {
            total += e->count.load();
            if (writers.size() < 12) {
                writers.push_back({{"writes", e->count.load()},
                                   {"instruction", e->rva != 0 ? fmt::format("{:#x}", e->rva) : ModuleOf(e->rip)},
                                   {"called_from", {e->ret0 != 0 ? fmt::format("{:#x}", e->ret0) : "", e->ret1 != 0 ? fmt::format("{:#x}", e->ret1) : "",
                                                    e->ret2 != 0 ? fmt::format("{:#x}", e->ret2) : ""}}});
            }
        }
        fields.push_back({{"offset", fmt::format("{:#x}", CtxFields[f].offset)}, {"field", CtxFields[f].name}, {"writes_in_2s", total}, {"writers", writers}});
    }
    out["fields"] = fields;
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "ctx_writers.json";
    std::ofstream(path) << out.dump(1);
    TLG_LOG(IMAGE, Error, "S1b ctx census written: {}", path.string());
}
} // namespace

bool CtxCensusOn() noexcept {
    static const bool on = [] {
        const char* e = std::getenv("TLG_X_CTX_CENSUS");
        return e != nullptr && *e == '1';
    }();
    return on;
}

void CtxCensusNote(u64 ctx) {
    if (g_census_started.load(std::memory_order_relaxed) || ctx == 0) {
        return;
    }
    std::vector<std::pair<u64, u64>> contexts;
    u64 best = 0, best_n = 0;
    {
        std::scoped_lock lock{g_ctx_mutex};
        bool found = false;
        for (auto& [c, n] : g_ctx_counts) {
            if (c == ctx) {
                ++n;
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto& [c, n] : g_ctx_counts) {
                if (c == 0) {
                    c = ctx;
                    n = 1;
                    break;
                }
            }
        }
        if (Core::DebugServer::PresentCount() < 2000) {
            return;
        }
        for (const auto& [c, n] : g_ctx_counts) {
            if (c != 0) {
                contexts.emplace_back(c, n);
                if (n > best_n) {
                    best = c;
                    best_n = n;
                }
            }
        }
    }
    if (best == 0 || g_census_started.exchange(true)) {
        return;
    }
    std::thread(RunCtxCensus, best, std::move(contexts)).detach();
}

#else
bool CtxCensusOn() noexcept {
    return false;
}
void CtxCensusNote(u64) {}
bool On() noexcept {
    return false;
}
void ArmRotating(u64, const char*) {}
std::string HitsJson() {
    return "[]";
}
void Arm(u64, u64, u32, u32) {}
void ArmAddress(u64, const char*) {}
#endif

} // namespace Core::NativeTsharpWatch
