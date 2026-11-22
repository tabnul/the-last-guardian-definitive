// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// TLG_VKB_GPU_READ_TRAP (debug, issue 83): see native_gpu_read_trap.h. How it works:
// - the pages under every watched object are made inaccessible (Linux mprotect PROT_NONE, Windows PAGE_NOACCESS);
// - a processor access to such a page faults; the loader's fault handler asks tlggpu_read_trap first. It notes the place
//   (the instruction, and the game's return addresses by the frame pointer chain), opens the page, and sets the trap flag,
//   so the processor runs that one instruction and traps again (single step); then the page is protected again;
// - a page that traps more than TrapsPerPageFrame times in one frame (a long copy) stays open until the next frame, so
//   the run cannot stall; that is counted (pages_opened). A read by a second thread while a page is open is not seen.
// Everything the fault path touches is under one spin lock and in tables that only the normal code paths grow.

#include "native/native_gpu_read_trap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "native/native_draw.h"
#include "tlg_freeze.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace Core::GpuReadTrap {
namespace {

constexpr u64 PageBytes = 4096;
constexpr u32 TrapsPerPageFrame = 32; ///< more traps of one page in one frame: it stays open until the next frame
constexpr u32 MaxSites = 4096;
constexpr u32 ChainDepth = 6;
constexpr u32 MaxStepPages = 4;   ///< pages one instruction may touch while it is single-stepped
constexpr u64 ZombieFrames = 8;   ///< a page entry whose objects are all gone is kept this many frames (a fault may still be on its way)
constexpr u64 OpenCapacity = 1u << 16;

struct Range {
    u64 end{};
    u64 id{};
    Kind kind{};
    u64 made_frame{};
};
struct Page {
    u32 cover{};    ///< watched objects on this page
    u32 stepping{}; ///< threads single-stepping an access to it
    u32 traps{};    ///< traps in frame `frame`
    u64 frame{};
    u64 released_frame{}; ///< the frame its cover went to 0
    bool prot{};    ///< protected now
    bool open{};    ///< left open until the next frame
};
struct Site {
    u64 key{};  ///< the game instruction, or for an access by our own (host) code the first game return address
    u64 rip{};  ///< the faulting instruction (raw) at the first hit
    std::array<u64, ChainDepth> chain{}; ///< the game's return addresses (raw) at the first hit
    u64 hits{}, first_frame{}, last_frame{};
    u64 range_bytes{}, offset{}, age_frames{};
    Kind kind{};
    bool write{}, host{};
};

/// A spin lock: the fault path may not sleep on a mutex the faulting thread could hold
std::atomic_flag spin = ATOMIC_FLAG_INIT;
struct SpinGuard {
    SpinGuard() {
        while (spin.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
    }
    ~SpinGuard() {
        spin.clear(std::memory_order_release);
    }
    SpinGuard(const SpinGuard&) = delete;
    SpinGuard& operator=(const SpinGuard&) = delete;
};

// under `spin`
std::map<u64, Range> ranges; ///< watched objects by start
std::unordered_map<u64, u64> start_of_id;
std::unordered_map<u64, Page> pages;
std::vector<u64> open_pages; ///< capacity reserved at the start; the fault path never grows it
bool open_overflow = false;
std::vector<Site> sites;     ///< capacity MaxSites, reserved at the start
std::vector<u32> site_index; ///< open addressing over `sites` (index + 1), 2 * MaxSites
u64 c_traps = 0, c_reads = 0, c_writes = 0, c_neighbour = 0, c_steps = 0, c_opened = 0, c_site_overflow = 0, c_protect_failed = 0, c_watched_total = 0,
    c_replaced = 0, c_cpu_over_watched = 0, c_unwatched = 0, c_late_faults = 0, c_watched_bytes = 0;
u64 frame_now = 0;

std::atomic<bool> armed{false};
#ifndef _WIN32
pid_t self_pid = 0;
#endif

struct StepState {
    u32 n;
    u64 pages[MaxStepPages];
};
#ifdef _WIN32
thread_local StepState step{};
#else
__thread StepState step __attribute__((tls_model("initial-exec")));
#endif

u64 ImageStart() {
    return TlgFreeze::guest_base.load(std::memory_order_relaxed);
}
u64 ImageEnd() {
    const u64 base = ImageStart(), size = TlgFreeze::guest_size.load(std::memory_order_relaxed);
    return base + (size != 0 ? size : 0x6000000ull);
}
bool InImage(u64 a) {
    return a >= ImageStart() && a < ImageEnd() && ImageStart() != 0;
}

/// Changes the protection of [first, first + count pages); false when the system refused
bool Protect(u64 first, u64 count, bool on) {
#ifdef _WIN32
    bool ok = true;
    for (u64 i = 0; i < count; ++i) { // one call per page: a range may span several allocations
        DWORD old = 0;
        ok = VirtualProtect(reinterpret_cast<void*>(first + i * PageBytes), PageBytes, on ? PAGE_NOACCESS : PAGE_READWRITE, &old) != 0 && ok;
    }
    return ok;
#else
    return mprotect(reinterpret_cast<void*>(first), count * PageBytes, on ? PROT_NONE : (PROT_READ | PROT_WRITE)) == 0;
#endif
}

/// Reads `bytes` of this process's memory without faulting (false: not readable)
bool SafeRead(u64 address, void* out, u64 bytes) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, bytes, &got) != 0 && got == bytes;
#else
    iovec local{out, bytes};
    iovec remote{reinterpret_cast<void*>(address), bytes};
    return process_vm_readv(self_pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(bytes);
#endif
}

/// The game's return addresses: the word at the stack pointer (a leaf function, often our own copy routine), then the frame pointer chain
void Chain(u64 rsp, u64 rbp, std::array<u64, ChainDepth>& out) {
    u32 n = 0;
    u64 word = 0;
    if (SafeRead(rsp, &word, 8) && InImage(word)) {
        out[n++] = word;
    }
    u64 fp = rbp;
    for (u32 depth = 0; depth < 16 && n < ChainDepth && fp != 0 && (fp & 7) == 0; ++depth) {
        u64 frame[2];
        if (!SafeRead(fp, frame, sizeof(frame)) || frame[0] <= fp) {
            break;
        }
        if (InImage(frame[1]) && (n == 0 || out[n - 1] != frame[1])) {
            out[n++] = frame[1];
        }
        fp = frame[0];
    }
}

/// Protects the pages of [start, end) that should be (under `spin`)
void ApplyRange(u64 start, u64 end) {
    u64 run = 0, run_n = 0;
    const auto flush = [&] {
        if (run_n != 0 && !Protect(run, run_n, true)) {
            ++c_protect_failed;
            for (u64 i = 0; i < run_n; ++i) {
                pages[run + i * PageBytes].prot = false;
            }
        }
        run_n = 0;
    };
    for (u64 p = start & ~(PageBytes - 1); p < end; p += PageBytes) {
        Page& pg = pages[p];
        if (pg.cover != 0 && !pg.prot && pg.stepping == 0 && !pg.open) {
            pg.prot = true;
            if (run_n != 0 && run + run_n * PageBytes == p) {
                ++run_n;
            } else {
                flush();
                run = p;
                run_n = 1;
            }
        }
    }
    flush();
}

void Arm() {
    bool expected = false;
    if (armed.load(std::memory_order_acquire)) {
        return;
    }
    {
        SpinGuard g;
        if (sites.capacity() < MaxSites) {
            sites.reserve(MaxSites);
            site_index.assign(2 * MaxSites, 0);
            open_pages.reserve(OpenCapacity);
        }
    }
#ifndef _WIN32
    self_pid = getpid();
#endif
    if (armed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        LOG_INFO(Render_Vulkan, "vkb read trap (debug, TLG_VKB_GPU_READ_TRAP={}): every processor access to memory the graphics card writes is caught; the game runs slower",
                 Mode());
    }
}

/// Drops the watched object starting at `start` (under `spin`)
void RemoveAt(std::map<u64, Range>::iterator it) {
    const u64 start = it->first, end = it->second.end;
    start_of_id.erase(it->second.id);
    ranges.erase(it);
    c_watched_bytes -= end - start;
    ++c_unwatched;
    u64 run = 0, run_n = 0;
    const auto flush = [&] {
        if (run_n != 0 && !Protect(run, run_n, false)) {
            ++c_protect_failed;
        }
        run_n = 0;
    };
    for (u64 p = start & ~(PageBytes - 1); p < end; p += PageBytes) {
        const auto pit = pages.find(p);
        if (pit == pages.end()) {
            continue;
        }
        Page& pg = pit->second;
        if (pg.cover != 0 && --pg.cover == 0) {
            pg.released_frame = frame_now;
            if (pg.prot) {
                pg.prot = false;
                if (run_n != 0 && run + run_n * PageBytes == p) {
                    ++run_n;
                } else {
                    flush();
                    run = p;
                    run_n = 1;
                }
            }
        }
    }
    flush();
}

Site* SiteFor(u64 key, Kind kind, bool write, bool host) {
    const u64 h = (key * 0x9e3779b97f4a7c15ull) ^ (static_cast<u64>(kind) << 1) ^ (write ? 1u : 0u) ^ (host ? 8u : 0u);
    const u32 mask = static_cast<u32>(site_index.size() - 1);
    for (u32 i = 0; i <= mask; ++i) {
        u32& slot = site_index[(static_cast<u32>(h >> 20) + i) & mask];
        if (slot == 0) {
            if (sites.size() >= sites.capacity()) {
                return nullptr;
            }
            sites.push_back({});
            Site& s = sites.back();
            s.key = key, s.kind = kind, s.write = write, s.host = host;
            slot = static_cast<u32>(sites.size());
            return &s;
        }
        Site& s = sites[slot - 1];
        if (s.key == key && s.kind == kind && s.write == write && s.host == host) {
            return &s;
        }
    }
    return nullptr;
}

void Lock() {
    while (spin.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }
}
void Unlock() {
    spin.clear(std::memory_order_release);
}

#ifdef _WIN32
thread_local u64 late_address = 0;
thread_local u32 late_count = 0;
#else
__thread u64 late_address __attribute__((tls_model("initial-exec")));
__thread u32 late_count __attribute__((tls_model("initial-exec")));
#endif

/// The fault path: an access to `address`. True: it was ours (the instruction runs again); `set_tf`: single-step it
bool OnAccess(u64 rip, u64 rsp, u64 rbp, u64 address, bool write, bool& set_tf) {
    set_tf = false;
    if (!armed.load(std::memory_order_acquire)) {
        return false;
    }
    const u64 page = address & ~(PageBytes - 1);
    const bool host = !InImage(rip);
    std::array<u64, ChainDepth> chain{};
    bool chained = false;
    if (host) {
        // our own code (or a system library) touched it: the place is the game code that called it (the walk is a system call: not under the lock)
        Chain(rsp, rbp, chain);
        chained = true;
    }
    Lock();
    auto pit = pages.find(page);
    if (pit == pages.end()) {
        Unlock();
        return false;
    }
    ++c_traps;
    if (!pit->second.prot) {
        // opened meanwhile by another thread's step, a frame's opening or the object's end: run it again. A fault that keeps coming back on a page we
        // do not protect is not ours (another protection): after a few tries it goes on to the other handlers
        const bool ours_lately = pit->second.stepping != 0 || pit->second.open || frame_now - pit->second.released_frame <= ZombieFrames;
        late_count = late_address == address ? late_count + 1 : 1;
        late_address = address;
        ++c_late_faults;
        Unlock();
        return ours_lately && late_count < 16;
    }
    late_count = 0;
    // the object the address belongs to (values: the lock may be let go below)
    bool in_range = false;
    u64 range_start = 0, range_end = 0, made_frame = 0;
    Kind kind{};
    if (auto rit = ranges.upper_bound(address); rit != ranges.begin()) {
        --rit;
        if (address < rit->second.end) {
            in_range = true;
            range_start = rit->first, range_end = rit->second.end, made_frame = rit->second.made_frame, kind = rit->second.kind;
        }
    }
    if (!in_range) {
        ++c_neighbour; // another object on the same page
    } else {
        ++(write ? c_writes : c_reads);
        const u64 key = host && chain[0] != 0 ? chain[0] : rip; // (our own code with no game caller: by its own instruction)
        // `sites` never moves (its capacity is reserved) and never shrinks: the pointer stays valid without the lock
        if (Site* s = SiteFor(key, kind, write, host); s == nullptr) {
            ++c_site_overflow;
        } else if (s->hits++ == 0) {
            s->first_frame = s->last_frame = frame_now;
            s->rip = rip;
            s->range_bytes = range_end - range_start;
            s->offset = address - range_start;
            s->age_frames = frame_now - made_frame;
            if (!chained) {
                Unlock();
                Chain(rsp, rbp, chain);
                Lock();
            }
            s->chain = chain;
        } else {
            s->last_frame = frame_now;
        }
    }
    pit = pages.find(page); // (again: the lock may have been let go)
    if (pit == pages.end()) {
        Unlock();
        return true;
    }
    Page& p = pit->second;
    if (p.frame != frame_now) {
        p.frame = frame_now;
        p.traps = 0;
    }
    const bool open = ++p.traps > TrapsPerPageFrame || step.n >= MaxStepPages;
    if (p.prot) {
        p.prot = false;
        if (!Protect(page, 1, false)) {
            ++c_protect_failed;
        }
    }
    if (open) {
        if (!p.open) {
            p.open = true;
            ++c_opened;
            if (open_pages.size() < open_pages.capacity()) {
                open_pages.push_back(page);
            } else {
                open_overflow = true;
            }
        }
        Unlock();
        return true;
    }
    ++p.stepping;
    step.pages[step.n++] = page;
    ++c_steps;
    set_tf = true;
    Unlock();
    return true;
}

/// The single-step trap after an access: the pages are protected again. False: this thread was not stepping (not ours)
bool OnStep() {
    if (step.n == 0) {
        return false;
    }
    SpinGuard g;
    for (u32 i = 0; i < step.n; ++i) {
        const auto pit = pages.find(step.pages[i]);
        if (pit == pages.end()) {
            continue;
        }
        Page& p = pit->second;
        if (p.stepping != 0) {
            --p.stepping;
        }
        if (p.stepping == 0 && p.cover != 0 && !p.open && !p.prot) {
            p.prot = true;
            if (!Protect(step.pages[i], 1, true)) {
                p.prot = false;
                ++c_protect_failed;
            }
        }
    }
    step.n = 0;
    return true;
}

const char* KindName(Kind k) {
    switch (k) {
    case Kind::UavBuffer:
        return "buffer the GPU writes (unordered access)";
    case Kind::CopyTarget:
        return "buffer our GPU copy or fill wrote";
    case Kind::Texture:
        return "texture the GPU draws or writes into";
    }
    return "?";
}

std::string HostPlace(u64 rip) {
#ifdef _WIN32
    HMODULE module = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(rip), &module) && module != nullptr &&
        GetModuleFileNameA(module, name, sizeof(name)) != 0) {
        const char* base = std::strrchr(name, '\\');
        return fmt::format("{}+{:#x}", base ? base + 1 : name, rip - reinterpret_cast<u64>(module));
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(rip), &info) != 0 && info.dli_fname != nullptr) {
        const char* base = std::strrchr(info.dli_fname, '/');
        return fmt::format("{}+{:#x}", base ? base + 1 : info.dli_fname, rip - reinterpret_cast<u64>(info.dli_fbase));
    }
#endif
    return fmt::format("{:#x}", rip);
}

std::string GameAddress(u64 raw) {
    const u64 base = Core::NativeDraw::ImageBase();
    return raw != 0 && base != 0 && raw >= base ? fmt::format("{:#x}", raw - base + 0x1000000) : std::string("0");
}

/// A short id of a game place, safe to paste in a report: 20 bits of a hash of the game address. Many addresses share each id, so it
/// cannot be turned back into an address; the box matches it against the ids of the places it caught itself. 0: no game place.
std::string PlaceId(u64 raw) {
    const u64 base = Core::NativeDraw::ImageBase();
    if (raw == 0 || base == 0 || raw < base) {
        return "-----";
    }
    const u64 game = raw - base + 0x1000000;
    u64 h = 0xcbf29ce484222325ull; // FNV-1a over the 8 bytes
    for (u32 i = 0; i < 8; ++i) {
        h = (h ^ ((game >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    }
    return fmt::format("{:05x}", h >> 44);
}

/// The module of host code at `rip` (no offset): which program part did the access (our own, the C library, a driver)
std::string HostModule(u64 rip) {
    const std::string place = HostPlace(rip);
    const auto plus = place.find('+');
    return plus == std::string::npos ? std::string("?") : place.substr(0, plus);
}

std::string FilePath() {
    if (const char* e = std::getenv("TLG_VKB_GPU_READ_TRAP_FILE"); e != nullptr && e[0] != 0) {
        return e;
    }
    return (Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "vkb_gpu_read_trap.json").string();
}

} // namespace

u32 Mode() {
    static const u32 mode = [] {
        const char* e = std::getenv("TLG_VKB_GPU_READ_TRAP");
        return e != nullptr && (e[0] == '1' || e[0] == '2') ? static_cast<u32>(e[0] - '0') : 0u;
    }();
    return mode;
}

void Watch(u64 id, u64 address, u64 bytes, Kind kind) {
    if (!On() || address == 0 || bytes == 0 || (kind == Kind::Texture && Mode() < 2)) {
        return;
    }
    Arm();
    SpinGuard g;
    if (start_of_id.contains(id)) {
        return;
    }
    if (const auto old = ranges.find(address); old != ranges.end()) {
        ++c_replaced; // an older object at the same memory (its delete not seen)
        RemoveAt(old);
    }
    ranges[address] = {address + bytes, id, kind, frame_now};
    start_of_id[id] = address;
    ++c_watched_total;
    c_watched_bytes += bytes;
    for (u64 p = address & ~(PageBytes - 1); p < address + bytes; p += PageBytes) {
        ++pages[p].cover;
    }
    ApplyRange(address, address + bytes);
}

void Unwatch(u64 id) {
    if (!On() || !armed.load(std::memory_order_acquire)) {
        return;
    }
    SpinGuard g;
    const auto s = start_of_id.find(id);
    if (s == start_of_id.end()) {
        return;
    }
    if (const auto it = ranges.find(s->second); it != ranges.end() && it->second.id == id) {
        RemoveAt(it);
    } else {
        start_of_id.erase(s);
    }
}

void CpuObjectMade(u64 address, u64 bytes) {
    if (!On() || !armed.load(std::memory_order_acquire) || address == 0 || bytes == 0) {
        return;
    }
    SpinGuard g;
    if (const auto it = ranges.find(address); it != ranges.end()) {
        ++c_cpu_over_watched;
        RemoveAt(it);
    }
}

void Tick() {
    if (!On() || !armed.load(std::memory_order_acquire)) {
        return;
    }
    static auto last_write = std::chrono::steady_clock::now();
    static u64 last_sites = 0;
    {
        SpinGuard g;
        ++frame_now;
        const auto reprotect = [](u64 page, Page& p) {
            p.open = false;
            if (p.cover != 0 && p.stepping == 0 && !p.prot) {
                p.prot = true;
                if (!Protect(page, 1, true)) {
                    p.prot = false;
                    ++c_protect_failed;
                }
            }
        };
        for (const u64 page : open_pages) {
            if (const auto it = pages.find(page); it != pages.end()) {
                reprotect(page, it->second);
            }
        }
        open_pages.clear();
        if (open_overflow) {
            for (auto& [page, p] : pages) {
                if (p.open) {
                    reprotect(page, p);
                }
            }
            open_overflow = false;
        }
        if ((frame_now % 64) == 0) {
            for (auto it = pages.begin(); it != pages.end();) {
                const Page& p = it->second;
                if (p.cover == 0 && p.stepping == 0 && !p.prot && frame_now - p.released_frame > ZombieFrames) {
                    it = pages.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_write < std::chrono::seconds(5)) {
        return;
    }
    last_write = now;
    const nlohmann::json report = Report();
    const std::string path = FilePath(), temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << report.dump(1);
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    { // the same places without any address, safe to paste in a report (path + "_summary.txt")
        std::string text = fmt::format("vkb read trap summary (mode {}): {} frames, {} traps, {} reads, {} writes, {} places, protect failed {}, late faults {}\n"
                                       "id    caller access memory                                   code        hits      age_frames object_bytes\n",
                                       report.value("mode", 0u), report.value("frames", 0ull), report.value("traps", 0ull), report.value("reads", 0ull),
                                       report.value("writes", 0ull), report.value("places", 0ull), report.value("protect_failed", 0ull), report.value("late_faults", 0ull));
        for (const auto& site : report["sites"]) {
            text += fmt::format("{:<5} {:<6} {:<6} {:<41} {:<11} {:<9} {:<10} {}\n", site.value("place_id", std::string("?")),
                                site.value("caller_id", std::string("?")), site.value("access", std::string("?")),
                                site.value("memory", std::string("?")), site.value("host_module", std::string("?")), site.value("hits", 0ull),
                                site.value("object_age_frames", 0ull), site.value("object_bytes", 0ull));
        }
        const std::string summary = path + "_summary.txt", summary_temp = summary + ".tmp";
        {
            std::ofstream out(summary_temp, std::ios::trunc);
            out << text;
        }
        std::filesystem::rename(summary_temp, summary, ec);
    }
    const u64 n = report.value("places", 0ull);
    if (n != last_sites) {
        last_sites = n;
        LOG_INFO(Render_Vulkan, "vkb read trap: {} places caught so far ({} reads, {} writes)", n, report.value("reads", 0ull), report.value("writes", 0ull));
    }
}

nlohmann::json Report() {
    if (!On()) {
        return nullptr;
    }
    std::vector<Site> copy;
    nlohmann::json j;
    {
        SpinGuard g;
        copy = sites;
        u64 protected_pages = 0;
        for (const auto& [page, p] : pages) {
            protected_pages += p.prot ? 1 : 0;
        }
        j = {{"mode", Mode()},
             {"frames", frame_now},
             {"traps", c_traps},
             {"reads", c_reads},
             {"writes", c_writes},
             {"neighbour_traps", c_neighbour},
             {"single_steps", c_steps},
             {"late_faults", c_late_faults},
             {"pages_opened_for_rest_of_frame", c_opened},
             {"site_overflow", c_site_overflow},
             {"protect_failed", c_protect_failed},
             {"watched", {{"now", ranges.size()}, {"total", c_watched_total}, {"mb_now", c_watched_bytes >> 20}, {"pages_protected_now", protected_pages},
                          {"replaced_same_start", c_replaced}, {"cpu_object_at_watched_start", c_cpu_over_watched}, {"unwatched", c_unwatched}}}};
    }
    std::sort(copy.begin(), copy.end(), [](const Site& a, const Site& b) { return a.hits > b.hits; });
    nlohmann::json list = nlohmann::json::array();
    for (const Site& s : copy) {
        nlohmann::json callers = nlohmann::json::array();
        for (const u64 c : s.chain) {
            if (c != 0) {
                callers.push_back(GameAddress(c));
            }
        }
        // the caller's id: the game return address above the place (for host code the place itself is the first return address)
        const u64 caller = s.host ? s.chain[1] : s.chain[0];
        list.push_back({{"place_id", s.host && s.chain[0] == 0 ? std::string("-----") : PlaceId(s.key)},
                        {"caller_id", PlaceId(caller)},
                        {"host_module", s.host ? HostModule(s.rip) : std::string("game")},
                        {"game_address", s.host && s.chain[0] == 0 ? std::string("0") : GameAddress(s.key)},
                        {"by_host_code", s.host},
                        {"instruction", s.host ? HostPlace(s.rip) : GameAddress(s.rip)},
                        {"callers", callers},
                        {"access", s.write ? "write" : "read"},
                        {"memory", KindName(s.kind)},
                        {"hits", s.hits},
                        {"first_frame", s.first_frame},
                        {"last_frame", s.last_frame},
                        {"object_bytes", s.range_bytes},
                        {"offset", s.offset},
                        {"object_age_frames", s.age_frames}});
    }
    j["places"] = static_cast<u64>(copy.size());
    j["sites"] = std::move(list);
    return j;
}

} // namespace Core::GpuReadTrap

/// The loader's fault handler asks this first (gpu_enabled): an access to memory the trap protects, or the single step after it.
/// Linux: `info` is the ucontext, `sig` SIGSEGV or SIGTRAP; Windows: `info` is the EXCEPTION_POINTERS (sig, address unused).
extern "C" int tlggpu_read_trap(void* info, int sig, void* address) {
    using namespace Core::GpuReadTrap;
    if (!armed.load(std::memory_order_acquire)) {
        return 0;
    }
#ifdef _WIN32
    (void)sig, (void)address;
    auto* ep = static_cast<EXCEPTION_POINTERS*>(info);
    CONTEXT* c = ep->ContextRecord;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP) {
        if (!OnStep()) {
            return 0;
        }
        c->EFlags &= ~0x100u;
        return 1;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2 || ep->ExceptionRecord->ExceptionInformation[0] == 8) {
        return 0;
    }
    bool tf = false;
    if (!OnAccess(c->Rip, c->Rsp, c->Rbp, ep->ExceptionRecord->ExceptionInformation[1], ep->ExceptionRecord->ExceptionInformation[0] == 1, tf)) {
        return 0;
    }
    if (tf) {
        c->EFlags |= 0x100u;
    }
    return 1;
#else
    greg_t* g = static_cast<ucontext_t*>(info)->uc_mcontext.gregs;
    if (sig == SIGTRAP) {
        if (!OnStep()) {
            return 0;
        }
        g[REG_EFL] &= ~static_cast<greg_t>(0x100);
        return 1;
    }
    if (sig != SIGSEGV) {
        return 0;
    }
    bool tf = false;
    if (!OnAccess(static_cast<u64>(g[REG_RIP]), static_cast<u64>(g[REG_RSP]), static_cast<u64>(g[REG_RBP]), reinterpret_cast<u64>(address), (g[REG_ERR] & 2) != 0, tf)) {
        return 0;
    }
    if (tf) {
        g[REG_EFL] |= static_cast<greg_t>(0x100);
    }
    return 1;
#endif
}
