// SPDX-License-Identifier: GPL-2.0-or-later
// Frame timeline capture (documents/tlg/frame-timeline-capture-design.md): a big buffer that every FrameTimeline event is also written to while a capture runs,
// dumped to the log directory when the capture ends.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <x86intrin.h>
#endif

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"
#include "native/native_prof.h"
#include "native/native_stall.h"
#include "native/native_timeline.h"
#include "native/shadps4/video_core/thread_usage.h"

extern "C" size_t runtime_thread_names(uint64_t* os_tids, char (*names)[32], size_t max); // src/runtime.h

namespace Core::FrameTimeline {

/// A thread with no OS name and no guest record (a driver's or the system's): "<module of its start address> thread", e.g. "nvoglv64.dll thread".
std::string UnnamedThreadName(u32 tid, const std::string& fallback) {
#ifdef _WIN32
    using QueryFn = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (query == nullptr) {
        return fallback;
    }
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (thread == nullptr) {
        return fallback;
    }
    PVOID start = nullptr;
    const LONG status = query(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
    CloseHandle(thread);
    HMODULE module = nullptr;
    if (status != 0 || start == nullptr ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(start), &module)) {
        return fallback;
    }
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(module, path, MAX_PATH) == 0) {
        return fallback;
    }
    std::string file = path;
    if (const size_t slash = file.find_last_of("\\/"); slash != std::string::npos) {
        file = file.substr(slash + 1);
    }
    return file + " thread";
#else
    (void)tid;
    return fallback;
#endif
}

std::vector<std::pair<u32, std::string>> ThreadNames() {
    auto out = VideoCore::ThreadNames();
    constexpr size_t MaxGuest = 1024;
    static std::vector<uint64_t> tids(MaxGuest);
    static std::vector<std::array<char, 32>> guest(MaxGuest);
    static std::mutex guest_mutex;
    std::scoped_lock lock{guest_mutex};
    const size_t n = runtime_thread_names(tids.data(), reinterpret_cast<char(*)[32]>(guest.data()), MaxGuest);
    std::map<u32, std::string> by_tid;
    for (size_t i = 0; i < n; ++i) {
        by_tid[static_cast<u32>(tids[i])] = guest[i].data();
    }
    // The guest's name when the OS has none (a thread renamed from another thread, the main thread, a thread that ended), or only its first 15 characters;
    // the OS name otherwise (our own threads that call guest code are guest records named "host").
    for (auto& [tid, name] : out) {
        const auto it = by_tid.find(tid);
        if (it == by_tid.end() || it->second.empty()) {
            if (name.rfind("tid", 0) == 0) {
                name = UnnamedThreadName(tid, name);
            }
            continue;
        }
        if (name.rfind("tid", 0) == 0 || (it->second.size() > name.size() && it->second.rfind(name, 0) == 0)) {
            name = it->second;
        }
        by_tid.erase(it);
    }
    for (const auto& [tid, name] : by_tid) {
        if (!name.empty()) {
            out.emplace_back(tid, name); // ended threads: their events stay in the capture
        }
    }
    return out;
}

namespace {
constexpr u64 CaptureCapacity = 12ull << 20; // events, 384 MiB
std::mutex capture_mutex;
std::map<u64, std::string> capture_names;
std::map<u32, std::string> span_names;
std::string last_file;
u64 capture_start_ns = 0;
u64 capture_end_ns = 0;
std::jthread capture_thread;
std::atomic<bool> running{false};
double capture_cycles_per_ns = 0.0;

#ifdef _WIN32
/// Capture, source E: every millisecond, the cycles each thread of the process ran since the previous sample (QueryThreadCycleTime: exact, unlike the 15 ms
/// tick of GetThreadTimes). One ThreadCpu event per thread that ran, written for that thread's id.
void SampleThreads(const std::stop_token& stop) {
    struct Tracked {
        HANDLE handle;
        ULONG64 last;
    };
    std::map<DWORD, Tracked> tracked;
    u64 last_refresh = 0;
    Common::SetCurrentThreadName("tlg:capture_cpu"); // the capture's own cost (about 100 threads read every millisecond), named so the report shows it
    const DWORD pid = GetCurrentProcessId();
    while (!stop.stop_requested() && capture_active.load(std::memory_order_relaxed)) {
        const u64 now = Now();
        if (last_refresh == 0 || now - last_refresh > 250'000'000ull) {
            last_refresh = now;
            if (HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
                    if (entry.th32OwnerProcessID != pid || tracked.contains(entry.th32ThreadID)) {
                        continue;
                    }
                    if (HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
                        ULONG64 cycles = 0;
                        QueryThreadCycleTime(handle, &cycles);
                        tracked[entry.th32ThreadID] = {handle, cycles};
                    }
                }
                CloseHandle(snapshot);
            }
        }
        for (auto& [tid, thread] : tracked) {
            ULONG64 cycles = 0;
            if (!QueryThreadCycleTime(thread.handle, &cycles)) {
                continue;
            }
            if (cycles > thread.last) {
                CaptureAppend(ThreadCpu, cycles - thread.last, Now(), 0, tid);
            }
            thread.last = cycles;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& [tid, thread] : tracked) {
        CloseHandle(thread.handle);
    }
}
#endif

void WriteCapture(const std::string& tag) {
    if (Prof::ThisIsProfCapture()) {
        // the profiler (Shift+F9): the probes were accumulated on the threads that ran them, the events only give the frames
        Prof::Analyze(capture_events, std::min<u64>(capture_count.load(), capture_capacity), capture_start_ns, capture_end_ns);
        return;
    }
    if (Stall::ThisIsStallCapture()) {
        // the stall profiler (F8): analysed on the spot, the events are not written
        std::map<u32, std::string> threads;
        for (const auto& [id, name] : ThreadNames()) {
            threads[id] = name;
        }
        std::map<u64, std::string> names;
        {
            std::scoped_lock lock{capture_mutex};
            names = capture_names;
        }
        Stall::Analyze(capture_events, std::min<u64>(capture_count.load(), capture_capacity), capture_start_ns, capture_end_ns, threads, names);
        return;
    }
    const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const auto base = dir / fmt::format("timeline_{}", tag);
    const u64 total = capture_count.load();
    const u64 count = std::min<u64>(total, capture_capacity);
    const u64 lost = total > capture_capacity ? total - capture_capacity : 0;
    const bool ring = capture_ring.load();
    // ring mode that wrapped: the oldest event is at total % capacity; write it first so the file is in time order
    const u64 first = ring && lost != 0 ? total % capture_capacity : 0;
    {
        std::ofstream out(base.string() + ".bin", std::ios::binary);
        const char magic[8] = {'T', 'L', 'T', 'L', '0', '0', '0', '1'};
        out.write(magic, 8);
        const u64 header[3] = {count, capture_start_ns, capture_end_ns};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        out.write(reinterpret_cast<const char*>(capture_events + first), static_cast<std::streamsize>((count - first) * sizeof(CapEvent)));
        if (first != 0) {
            out.write(reinterpret_cast<const char*>(capture_events), static_cast<std::streamsize>(first * sizeof(CapEvent)));
        }
    }
    nlohmann::json j;
    j["events"] = count;
    j["lost_events"] = lost;
    j["ring"] = ring;
    j["start_ns"] = capture_start_ns;
    j["end_ns"] = capture_end_ns;
    j["record_bytes"] = sizeof(CapEvent);
    j["cycles_per_ns"] = capture_cycles_per_ns;
    j["record"] = "u64 ns, u64 arg, u64 arg2, u32 type, u32 thread";
    j["types"] = {{"1", "FirstHook"}, {"2", "Submit"}, {"3", "SubmitFlip"}, {"4", "FlipCp"}, {"5", "FlipPrepared"}, {"6", "PresentStart"}, {"7", "PresentEnd"},
                  {"8", "CpWait"}, {"9", "FlipWait"}, {"10", "GameWait"}, {"11", "GameSignal"}, {"12", "CpIdleQueued"}, {"13", "RasterIdleSignal"},
                  {"14", "StreamPublish"}, {"15", "SubmitDone"}, {"16", "DingDongWait"}, {"17", "EopParsed"}, {"18", "EopSignaled"}, {"20", "StageSpan"},
                  {"21", "GpuCommandBuffer"}, {"22", "GpuPass"}, {"23", "ThreadCpu"}, {"24", "GpuClockSync"}, {"25", "GuestSpan"}, {"26", "GuestSpanInfo"},
                  {"27", "SchedPoll"}, {"28", "SchedNode"}, {"29", "AscKick"}, {"30", "CpResume"}, {"31", "FenceSignaled"}, {"32", "LabelWrite"}, {"33", "ResourceUse"}, {"34", "ResourceHash"}, {"35", "GameWaitSite"}, {"36", "ResourceStale"}, {"37", "ThumbFrame"}, {"38", "GlitchMark"}, {"39", "WriterHit"}};
    j["stages"] = {{"1", "FrontOp"}, {"2", "FrontQueueFull"}, {"3", "FrontDrain"}, {"4", "RasterOp"}, {"5", "RasterIdle"}, {"6", "RasterPass"},
                   {"7", "EncoderReplay"}, {"8", "EncoderSubmit"}, {"9", "EncoderIdle"}};
    {
        auto& spans = j["spans"] = nlohmann::json::object();
        std::scoped_lock lock{capture_mutex};
        for (const auto& [id, text] : span_names) {
            spans[std::to_string(id)] = text;
        }
    }
    auto& threads = j["threads"] = nlohmann::json::object();
    for (const auto& [id, name] : ThreadNames()) {
        threads[std::to_string(id)] = name;
    }
    auto& names = j["names"] = nlohmann::json::object();
    {
        std::scoped_lock lock{capture_mutex};
        for (const auto& [id, text] : capture_names) {
            names[std::to_string(id)] = text;
        }
        last_file = base.string();
    }
    std::ofstream(base.string() + ".json") << j.dump(1);
    LOG_INFO(Render, "timeline capture '{}': {} events ({} lost) written to {}.bin", tag, count, lost, base.string());
}
} // namespace

bool CaptureRunning() {
    return running.load();
}

std::string CaptureLastFile() {
    std::scoped_lock lock{capture_mutex};
    return last_file;
}

bool StartCapture(u32 seconds, const std::string& tag, bool ring) {
    if (running.exchange(true)) {
        return false;
    }
    capture_ring.store(ring);
    if (!capture_events) {
        capture_events = static_cast<CapEvent*>(std::calloc(CaptureCapacity, sizeof(CapEvent)));
        capture_capacity = capture_events ? CaptureCapacity : 0;
    }
    if (!capture_events) {
        running = false;
        return false;
    }
    {
        std::scoped_lock lock{capture_mutex};
        capture_names.clear();
    }
    capture_count.store(0);
    capture_start_ns = Now();
#ifdef _WIN32
    const unsigned long long tsc_start = __rdtsc();
#else
    const unsigned long long tsc_start = 0; // Linux: no cycle rate in the capture (cycles_per_ns stays 0, as ThreadCycles() is 0 there)
#endif
    capture_active.store(true, std::memory_order_release);
    capture_thread = std::jthread([seconds, tag, tsc_start](std::stop_token stop) {
        Common::SetCurrentThreadName("tlg:capture");
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
#ifdef _WIN32
        std::jthread sampler(SampleThreads);
#endif
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        capture_active.store(false, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // writers that passed the check finish
        capture_end_ns = Now();
#ifdef _WIN32
        capture_cycles_per_ns = static_cast<double>(__rdtsc() - tsc_start) / static_cast<double>(capture_end_ns - capture_start_ns);
#endif
        WriteCapture(tag);
        capture_ring.store(false);
        running.store(false);
    });
    return true;
}

bool StopCapture() {
    if (!running.load()) {
        return false;
    }
    capture_thread.request_stop();
    return true;
}

void RegisterSpan(u32 id, const std::string& name) {
    std::scoped_lock lock{capture_mutex};
    span_names[id] = name;
}

std::string CaptureStatus() {
    nlohmann::json j;
    j["running"] = running.load();
    j["events"] = std::min<u64>(capture_count.load(), capture_capacity);
    j["capacity"] = capture_capacity;
    std::scoped_lock lock{capture_mutex};
    j["last_file"] = last_file;
    return j.dump();
}

void CaptureNote(u64 id, const char* text) {
    if (!capture_active.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{capture_mutex};
    capture_names.emplace(id, text);
}
} // namespace Core::FrameTimeline
