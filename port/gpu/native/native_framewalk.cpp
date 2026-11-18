// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_objects.h"
#include "native/native_vkb_shader_dump.h"
#include "native/native_vkb_stream.h"
#include "native/native_framewalk.h"
#include "native/native_vkb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "native/native_byte_audit.h"
#include "native/native_debug_server.h"
#include "native/native_flight.h"
#include "native/native_log.h"
#include "native/native_marker.h"
#include "native/native_ui_trace.h"
#include "native/native_timeline.h"
#include "native/native_tsharp_watch.h"

namespace Core::FrameWalk {

namespace {

constexpr double FlightMs = 8000.0;   ///< flight recorder tail written at a freeze
constexpr u32 FramesInDrawTable = 64; ///< as many as the table holds (about five frames of the busiest scene)
constexpr u32 TimelineFilesKept = 3;  ///< timeline_fw_* files are up to 384 MB each
constexpr int GameBlockWaitMs = 3000; ///< the render thread has this long to reach the next submit before the dump is written without it
constexpr int DrainMs = 400;          ///< after the render thread stopped: time for the front and raster threads to finish what was already queued

const bool enabled = [] {
    const char* e = std::getenv("TLG_FRAMEWALK");
    return e == nullptr || *e != '0';
}();

std::mutex mutex;
std::condition_variable cv;
/// note 91: F5 in debugger mode captures one whole frame before the freeze: 1 armed (the next frame boundary starts the capture), 2 capturing (the next boundary
/// ends it), 3 the captured frame is being flipped (the next boundary freezes). TLG_FRAMEWALK_CAPTURE=0: F5 freezes at once, as before.
std::atomic<u32> capture_step{0};
bool FrameCaptureWanted() {
    static const bool on = [] { const char* e = std::getenv("TLG_FRAMEWALK_CAPTURE"); return e == nullptr || e[0] != '0'; }();
    return on;
}
std::atomic<bool> hold{false};  ///< a freeze is requested or active: AtSubmit waits
std::atomic<u32> state{static_cast<u32>(State::Idle)};
bool blocked = false;            ///< the render thread is waiting inside AtSubmit
bool recording_ring = false;     ///< a ring capture was started by the key
std::string record_tag;
std::string last_folder;
std::string last_timeline;
std::string last_note;
u64 freezes = 0;
std::jthread worker;
void (*toast_hook)(int kind) = nullptr;
ThumbHooks thumb_hooks;

void Toast(int kind) {
    if (toast_hook != nullptr) {
        toast_hook(kind);
    }
}

void SetState(State s) {
    state.store(static_cast<u32>(s));
}

std::string Stamp() {
    const std::time_t t = std::time(nullptr);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", std::localtime(&t));
    return buffer;
}

std::filesystem::path LogDir() {
    return Common::FS::GetUserPath(Common::FS::PathType::LogDir);
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}

/// Keeps the newest TimelineFilesKept ring captures and framewalk folders (names sort by time).
void PruneOld() {
    std::vector<std::filesystem::path> bins, folders;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(LogDir(), ec)) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind("timeline_fw_", 0) == 0 && entry.path().extension() == ".bin") {
            bins.push_back(entry.path());
        } else if (entry.is_directory() && name.rfind("framewalk_", 0) == 0) {
            folders.push_back(entry.path());
        }
    }
    std::ranges::sort(bins);
    while (bins.size() > TimelineFilesKept) {
        std::filesystem::remove(bins.front(), ec);
        std::filesystem::path json = bins.front();
        json.replace_extension(".json");
        std::filesystem::remove(json, ec);
        bins.erase(bins.begin());
    }
    std::ranges::sort(folders);
    while (folders.size() > 10) {
        std::filesystem::remove_all(folders.front(), ec);
        folders.erase(folders.begin());
    }
}

void StartRecording() {
    record_tag = "fw_" + Stamp();
    if (FrameTimeline::StartCapture(3600, record_tag, true)) {
        recording_ring = true;
        ByteAudit::Reset();
        ByteAudit::SetActive(true);
        Core::Vkb::ShaderDump::Reset(); // note 89
        { // note 90: the slow-pass draw dumps of the previous recording
            std::error_code ec;
            std::filesystem::remove_all(LogDir() / "vkb_image_dump" / "debugger", ec);
        }
        if (thumb_hooks.enable != nullptr) {
            thumb_hooks.enable(true);
        }
        SetState(State::Recording);
        Toast(9);
        LOG_INFO(Render, "frame walk: recording into a ring ({})", record_tag);
    } else {
        recording_ring = false;
        last_note = "could not start the ring capture (another capture or profiler is running)";
        LOG_WARNING(Render, "frame walk: {}", last_note);
    }
}

void Resume() {
    {
        std::scoped_lock lock{mutex};
        hold.store(false);
        blocked = false;
        SetState(State::Idle);
    }
    cv.notify_all();
    Toast(11);
    LOG_INFO(Render, "frame walk: resumed");
}

/// Waits for the render thread to stop at the submit, lets the queues drain, ends the recording, writes the folder, then reports Frozen.
void FreezeWorker() {
    Common::SetCurrentThreadName("tlg:framewalk");
    const auto begin = std::chrono::steady_clock::now();
    bool game_stopped = false;
    while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(GameBlockWaitMs)) {
        {
            std::scoped_lock lock{mutex};
            game_stopped = blocked;
        }
        if (game_stopped) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (game_stopped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(DrainMs));
    }
    const u64 frozen_at_present = DebugServer::PresentCount();

    std::string timeline;
    if (recording_ring) {
        FrameTimeline::StopCapture();
        const auto wait_begin = std::chrono::steady_clock::now();
        while (FrameTimeline::CaptureRunning() && std::chrono::steady_clock::now() - wait_begin < std::chrono::seconds(60)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        timeline = FrameTimeline::CaptureLastFile();
        recording_ring = false;
    }

    unsigned thumbs = 0;
    const std::string stamp = Stamp();
    const auto dir = LogDir() / fmt::format("framewalk_{}", stamp);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    try {
        WriteText(dir / "draws.json", Marker::DrawTable(static_cast<u32>(freezes + 1), FramesInDrawTable));
        if (Core::UiTrace::Armed()) {
            WriteText(dir / "uitrace.json", Core::UiTrace::Json());
        }
        WriteText(dir / "flight.json", Flight::Report(Flight::RingEvents, false, FlightMs));
        WriteText(dir / "counters.json", DebugServer::CountersSnapshot().dump(1));
        WriteText(dir / "vkb_frames.json", Core::Vkb::FrameSnapshots().dump()); // note 44
        WriteText(dir / "gpu_passes.json", Core::Vkb::Stream::GpuPassReport().dump(1)); // note 87: debugger mode GPU submissions and their slowest passes
        if (timeline.empty()) {
            // no debugger mode before the freeze: nothing to add
        } else {
            WriteText(dir / "byte_audit.json", ByteAudit::Report());
            { // note 91: the frame capture: wait for its copies (the GPU had the frame before the freeze), then its table, then the files into the folder
                std::error_code ec;
                const auto from = LogDir() / "vkb_image_dump" / "capture";
                if (std::filesystem::exists(from, ec)) {
                    const auto wait_begin = std::chrono::steady_clock::now();
                    while (Core::Vkb::Stream::CaptureJobsLeft() != 0 && std::chrono::steady_clock::now() - wait_begin < std::chrono::seconds(10)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                    const bool written = Core::Vkb::WaitDumps(180000);
                    nlohmann::json table = Core::Vkb::Stream::CaptureReport();
                    table["all_files_written"] = written;
                    table["draws_never_recorded"] = Core::Vkb::Stream::CaptureJobsLeft();
                    WriteText(from / "capture.json", table.dump(1));
                    std::filesystem::rename(from, dir / "capture", ec);
                    if (ec) {
                        std::filesystem::copy(from, dir / "capture", std::filesystem::copy_options::recursive, ec);
                        std::filesystem::remove_all(from, ec);
                    }
                }
            }
            (void)Core::Vkb::ShaderDump::Write(dir / "shaders"); // note 89: every shader used while recording
            { // note 90: the slow-pass draw dumps of this recording (written by their own threads once the GPU had them)
                std::error_code ec;
                const auto from = LogDir() / "vkb_image_dump" / "debugger";
                if (std::filesystem::exists(from, ec)) {
                    std::filesystem::rename(from, dir / "slow_draws", ec);
                    if (ec) {
                        std::filesystem::copy(from, dir / "slow_draws", std::filesystem::copy_options::recursive, ec);
                        std::filesystem::remove_all(from, ec);
                    }
                }
            }
            if (thumb_hooks.dump != nullptr) {
                thumbs = thumb_hooks.dump((dir / "thumbs.bin").string());
            }
        }
    } catch (const std::exception& e) {
        TLG_LOG(RUNTIME, Error, "frame walk dump failed: {}", e.what());
    }
    std::string note = fmt::format("frame walk freeze {} written {}\n", freezes + 1, stamp);
    note += fmt::format("presented frames at the freeze: {}\n", frozen_at_present);
    note += game_stopped ? "the game's render thread stopped at the start of a sceGnmSubmitAndFlipCommandBuffers: the last complete frame is the one before; the next frame's commands are built but not submitted\n"
                         : "WARNING: the render thread did not reach a submit within 3 s (loading? menu?), the dump was written while the game still ran\n";
    note += timeline.empty() ? "no timeline: the freeze was not preceded by Shift+F5\n" : fmt::format("timeline (ring, newest events): {}.bin / .json; read with documents/tlg/tools/framewalk_report.py\n", timeline);
    if (!timeline.empty()) {
        note += fmt::format("byte_audit.json: the byte audit (stale reads, write watch hits); thumbs.bin: {} thumbnails of presented frames (read with framewalk_report.py --sheet)\n", thumbs);
    }
    note += "live reads while frozen: /memory?addr=0x..&size=N, /framewalk, /resources, /registry, /native\n";
    WriteText(dir / "note.txt", note);
    PruneOld();
    ByteAudit::SetActive(false);
    if (thumb_hooks.enable != nullptr) {
        thumb_hooks.enable(false);
    }

    {
        std::scoped_lock lock{mutex};
        last_folder = dir.string();
        last_timeline = timeline;
        last_note = note;
        ++freezes;
        SetState(State::Frozen);
    }
    Toast(10);
    LOG_INFO(Render, "frame walk: frozen, written to {}", dir.string());
}

void StartFreeze() {
    {
        std::scoped_lock lock{mutex};
        blocked = false;
        hold.store(true);
        SetState(State::Freezing);
    }
    worker = std::jthread{[](std::stop_token) { FreezeWorker(); }};
}

} // namespace

void KeyRecord() {
    if (!enabled) {
        return;
    }
    switch (Current()) {
    case State::Idle:
        StartRecording();
        break;
    case State::Recording:
        // already recording: nothing to do (a restart would write the whole ring to disk, up to 384 MB); F5 ends the recording
        Toast(9);
        break;
    case State::Frozen:
        Resume();
        StartRecording();
        break;
    case State::Freezing:
        break;
    }
}

void KeyFreeze() {
    if (!enabled) {
        return;
    }
    switch (Current()) {
    case State::Idle:
    case State::Recording:
        if (Current() == State::Recording && DebuggerMode() && FrameCaptureWanted()) {
            u32 idle = 0;
            if (capture_step.compare_exchange_strong(idle, 1)) {
                LOG_INFO(Render, "frame walk: capturing the next frame, then freezing");
            }
            break;
        }
        StartFreeze();
        break;
    case State::Frozen:
        Resume();
        break;
    case State::Freezing:
        break;
    }
}

void AtSubmit() {
    if (const u32 step = capture_step.load(std::memory_order_relaxed); step != 0) [[unlikely]] { // note 91
        if (step == 1) {
            std::error_code ec;
            std::filesystem::remove_all(LogDir() / "vkb_image_dump" / "capture", ec);
            Core::Vkb::Stream::SetFrameCapture(true);
            capture_step.store(2);
        } else if (step == 2) {
            Core::Vkb::Stream::SetFrameCapture(false);
            capture_step.store(3);
        } else {
            capture_step.store(0);
            StartFreeze();
        }
    }
    if (!hold.load(std::memory_order_relaxed)) {
        return;
    }
    std::unique_lock lock{mutex};
    if (!hold.load()) {
        return;
    }
    blocked = true;
    cv.wait(lock, [] { return !hold.load(); });
    blocked = false;
}

State Current() {
    return static_cast<State>(state.load());
}

void SetToastHook(void (*toast)(int kind)) {
    toast_hook = toast;
}

void SetThumbHooks(const ThumbHooks& hooks) {
    thumb_hooks = hooks;
}

bool DebuggerMode() {
    return ByteAudit::Active();
}

std::string Report(const std::string& action) {
    if (action == "audit") {
        return ByteAudit::Report();
    }
    if (action == "record") {
        KeyRecord();
    } else if (action == "freeze") {
        if (Current() == State::Idle || Current() == State::Recording) {
            KeyFreeze();
        }
    } else if (action == "resume") {
        if (Current() == State::Frozen) {
            Resume();
        }
    }
    static const char* names[] = {"idle", "recording", "freezing", "frozen"};
    nlohmann::json j;
    j["enabled"] = enabled;
    j["state"] = names[static_cast<u32>(Current())];
    j["keys"] = "Shift+F5 starts recording (ring of the newest events); F5 freezes at the next frame boundary, F5 again resumes; /framewalk?action=record|freeze|resume does the same";
    j["freezes"] = freezes;
    {
        std::scoped_lock lock{mutex};
        j["folder"] = last_folder;
        j["timeline"] = last_timeline;
        j["note"] = last_note;
        j["render_thread_blocked"] = blocked;
    }
    j["recording"] = recording_ring;
    j["timeline_status"] = nlohmann::json::parse(FrameTimeline::CaptureStatus(), nullptr, false);
    return j.dump(1);
}

} // namespace Core::FrameWalk
