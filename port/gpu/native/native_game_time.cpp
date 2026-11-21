// SPDX-License-Identifier: GPL-2.0-or-later
// 120 fps game timing (documents/tlg/native plan/pc-port/game-timing-120fps.md): the game's own code advances two kinds of effects by a constant 1/30 s per frame, which is right at
// 30 fps and runs four times too fast at 120. The constant is read at four places; each is followed by a mid hook (core/guest_hooks.h, mode "mid") that multiplies the register that
// holds the constant by (frame time * 30), so the step is the real frame time and stays exactly 1 at 30 fps.
//
//   env_phase_dt   0x01e0f071  FUN_01e0eec0: phase = fract(phase + rate * (time scale * 1/30))        xmm1 = time scale (the constant is multiplied in the next instruction)
//   particle_dt_a  0x01bcb5d1  FUN_01bcac40: count = max(1/30 * emission rate, 0) + carry               xmm0 = the constant
//   particle_dt_b  0x01bcbf34  FUN_01bcac40: the same, the other emission mode                           xmm0
//   particle_dt_c  0x01bcb790  FUN_01bcac40: the spread of the new particles over the frame (1/30 * ..)  xmm6
//
// The frame time is the engine clock's raw frame time (`[0x069a1b40 (ghidra)] + 0x190`, written by FUN_01d16480 every frame, before smoothing, the snap and the pause / slow motion
// scale), so a paused game keeps its environment animating as the console does. Switch: `TLG_GAME_TIME=0` turns it off (the factor is 1).

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include <nlohmann/json.hpp>

#include "common/types.h"
#include "native/native_debug_server.h"
#include "native/native_game_time.h"
#include "native/native_hooks.h"
#include "native/native_timeline.h"

namespace Core::NativeGameTime {

namespace {
constexpr u64 ClockSlotImageOffset = 0x069a1b40 - 0x1000000;
constexpr u32 RawFrameTimeOffset = 0x190;
constexpr float ConsoleStep = 1.0f / 30.0f;

std::atomic<u64> calls[4]{}, fallbacks{0};
std::atomic<u32> lua_dt_bits{0}, lua_edt_bits{0};
std::atomic<u32> last_dt_bits{0}, min_factor_bits{0x7f7fffff}, max_factor_bits{0};

bool Enabled() {
    static const bool on = [] {
        const char* e = std::getenv("TLG_GAME_TIME");
        return e == nullptr || e[0] != '0';
    }();
    return on;
}

/// dt * 30 for this frame (1 when it cannot be read or is not a plausible frame time).
float Factor() {
    const u64 image = GuestHooks::EbootBase();
    if (!Enabled() || image == 0) {
        return 1.0f;
    }
    const u64 clock = *reinterpret_cast<const u64*>(image + ClockSlotImageOffset);
    if (clock == 0) {
        fallbacks.fetch_add(1, std::memory_order_relaxed);
        return 1.0f;
    }
    float dt;
    std::memcpy(&dt, reinterpret_cast<const void*>(clock + RawFrameTimeOffset), sizeof(dt));
    if (!(dt > 0.0001f && dt <= 0.1f)) {
        fallbacks.fetch_add(1, std::memory_order_relaxed);
        return 1.0f;
    }
    const float factor = dt / ConsoleStep;
    u32 bits;
    // the two values the Lua ticks get as (dt, edt): needed to pick the input of the Lua frame factor
    std::memcpy(&bits, reinterpret_cast<const void*>(clock + 0x19c), sizeof(bits));
    lua_dt_bits.store(bits, std::memory_order_relaxed);
    std::memcpy(&bits, reinterpret_cast<const void*>(clock + 0x194), sizeof(bits));
    lua_edt_bits.store(bits, std::memory_order_relaxed);
    std::memcpy(&bits, &dt, sizeof(bits));
    last_dt_bits.store(bits, std::memory_order_relaxed);
    std::memcpy(&bits, &factor, sizeof(bits));
    u32 low = min_factor_bits.load(std::memory_order_relaxed);
    while (bits < low && !min_factor_bits.compare_exchange_weak(low, bits)) {
    }
    u32 high = max_factor_bits.load(std::memory_order_relaxed);
    while (bits > high && !max_factor_bits.compare_exchange_weak(high, bits)) {
    }
    return factor;
}

/// Frame pacing (2026-11-21, parallel-passes-research.md section 14): TLG_GAME_STEP=raw makes the game move its world by the real frame time. FUN_01d16480 first
/// stores the unsmoothed step at clock + 0x19c (raw frame time, clamped, times the time scales; 0 when paused), then overrides it with a smoothed step cut to
/// 1/360 s (the filter object at clock + 0xb0). The hook sits on the override's store (0x01d16670, VMOVSS [RDI + 0x19c], XMM0, where both branches meet) and puts
/// the unsmoothed step back into XMM0 (and into the filter's copy, + 0x39c); the catch-up term after it then stays near zero. Measured: speed per frame
/// sd 0.175 -> 0.121, frames off by > 20 % 23 -> 8 % (the step against the frames' screen time, owner capture).
/// TLG_GAME_STEP=raw: always the real frame time. =auto (the launcher's 120 mode): the game's own step while it holds its frame rate (the step is then an exact
/// 3/360 s and the limiter paces the presents: steadier than the raw time's +-1 ms), the real frame time while it runs below (dips: the rounded step alternates 3
/// and 4 units and the error term corrects in jumps). Below = the game's smoothed frame time (the filter, + 0x3a0) over 1.05 x the target (1 / TLG_ENGINE_FPS),
/// back above at 1.02 x. Heaviest save at 4224x2376, 120 mode (~110 fps): frames whose motion is off by > 20 % 33.5 -> 10.0 % (raw, with the 1000 Hz tick).
enum class StepMode { Game, Raw, Auto };
StepMode Mode() {
    static const StepMode mode = [] {
        const char* e = std::getenv("TLG_GAME_STEP");
        const std::string_view v = e != nullptr ? e : "";
        return v == "raw" ? StepMode::Raw : v == "auto" ? StepMode::Auto : StepMode::Game;
    }();
    return mode;
}
bool ContinuousStepOn() {
    return Mode() != StepMode::Game;
}
float TargetFrameTime() {
    static const float t = [] {
        const char* e = std::getenv("TLG_ENGINE_FPS");
        const int fps = e != nullptr ? std::atoi(e) : 0;
        return fps > 0 ? 1.0f / static_cast<float>(fps) : 0.0f;
    }();
    return t;
}
std::atomic<u64> c_continuous{0}, c_auto_raw_frames{0}, c_auto_switches{0};
bool auto_raw = false; // (the game's main thread only)
u64 PS4_SYSV_ABI ContinuousStep(GuestHooks::MidContext* context) {
    if (!ContinuousStepOn()) {
        return 0;
    }
    if (Mode() == StepMode::Auto) {
        const float target = TargetFrameTime();
        float smooth = 0.0f;
        if (context->rax != 0) {
            std::memcpy(&smooth, reinterpret_cast<const void*>(context->rax + 0x3a0), sizeof(smooth));
        }
        const bool was = auto_raw;
        auto_raw = target > 0.0f && (auto_raw ? smooth > target * 1.02f : smooth > target * 1.05f);
        if (auto_raw != was) {
            c_auto_switches.fetch_add(1, std::memory_order_relaxed);
        }
        if (!auto_raw) {
            return 0; // the game's step
        }
        c_auto_raw_frames.fetch_add(1, std::memory_order_relaxed);
    }
    float step;
    std::memcpy(&step, reinterpret_cast<const void*>(context->rdi + 0x19c), sizeof(step)); // (not yet overwritten: the hooked store writes it)
    // TLG_GAME_STEP_AVG=N: one shared step for the world, characters, animation and physics (they must match: a separate physics step made the boy slide,
    // 2026-11-21), the average of the last N raw steps: steadier for Havok, close to the real time for the camera. Pause (step 0) resets the history.
    // default 3 since 2026-11-21 (owner play OK: boy walks normally, contacts steadier); TLG_GAME_STEP_AVG=1: the raw step
    static const u32 avg_n = [] { const char* e = std::getenv("TLG_GAME_STEP_AVG"); const int n = e != nullptr ? std::atoi(e) : 3; return static_cast<u32>(std::clamp(n, 1, 8)); }();
    if (avg_n > 1) {
        static float history[8]{};
        static u32 count = 0, at = 0;
        if (step < 1e-5f) {
            count = 0, at = 0;
        } else {
            history[at] = step;
            at = (at + 1) % avg_n;
            count = std::min(count + 1, avg_n);
            float sum = 0.0f;
            for (u32 i = 0; i < count; ++i) {
                sum += history[i];
            }
            step = sum / static_cast<float>(count);
        }
    }
    std::memcpy(context->ymm[0], &step, sizeof(step));
    if (context->rax != 0) {
        std::memcpy(reinterpret_cast<void*>(context->rax + 0x39c), &step, sizeof(step));
    }
    c_continuous.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

template <u32 Site, u32 Reg>
u64 PS4_SYSV_ABI ScaleRegister(GuestHooks::MidContext* context) {
    const u64 n = calls[Site].fetch_add(1, std::memory_order_relaxed);
    float value;
    std::memcpy(&value, context->ymm[Reg], sizeof(value));
    value *= Factor();
    std::memcpy(context->ymm[Reg], &value, sizeof(value));
    if (Site == 0 && n % 600 == 0) {
        Report();
    }
    return 0;
}
} // namespace

void NoteClock() {
    const u64 image = GuestHooks::EbootBase();
    if (image == 0) {
        return;
    }
    const u64 clock = *reinterpret_cast<const u64*>(image + ClockSlotImageOffset);
    if (clock == 0) {
        return;
    }
    u32 raw = 0, step = 0, error = 0, smooth = 0;
    std::memcpy(&raw, reinterpret_cast<const void*>(clock + 0x190), 4);
    std::memcpy(&step, reinterpret_cast<const void*>(clock + 0x19c), 4);
    std::memcpy(&error, reinterpret_cast<const void*>(clock + 0x1a4), 4);
    if (const u64 filter = *reinterpret_cast<const u64*>(clock + 0xb0); filter != 0) {
        std::memcpy(&smooth, reinterpret_cast<const void*>(filter + 0x3a0), 4);
    }
    FrameTimeline::Record(FrameTimeline::GameClock, u64{raw} | (u64{step} << 32), FrameTimeline::Now(), u64{error} | (u64{smooth} << 32));
}

u64 FindHandler(const char* name) {
    const std::string_view n{name};
    if (n == "env_phase_dt") {
        return reinterpret_cast<u64>(&ScaleRegister<0, 1>);
    }
    if (n == "particle_dt_a") {
        return reinterpret_cast<u64>(&ScaleRegister<1, 0>);
    }
    if (n == "particle_dt_b") {
        return reinterpret_cast<u64>(&ScaleRegister<2, 0>);
    }
    if (n == "particle_dt_c") {
        return reinterpret_cast<u64>(&ScaleRegister<3, 6>);
    }
    if (n == "continuous_step") {
        return reinterpret_cast<u64>(&ContinuousStep);
    }
    return 0;
}

void Report() {
    const auto f = [](u32 bits) {
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    };
    DebugServer::Publish("game_time", {{"enabled", Enabled()},
                                       {"env_phase_calls", calls[0].load()},
                                       {"particle_a_calls", calls[1].load()},
                                       {"particle_b_calls", calls[2].load()},
                                       {"particle_c_calls", calls[3].load()},
                                       {"last_raw_frame_time_s", f(last_dt_bits.load())},
                                       {"last_lua_dt_s_clock_19c", f(lua_dt_bits.load())},
                                       {"last_lua_edt_s_clock_194", f(lua_edt_bits.load())},
                                       {"factor_min", f(min_factor_bits.load())},
                                       {"factor_max", f(max_factor_bits.load())},
                                       {"unreadable_or_implausible_frame_time", fallbacks.load()},
                                       {"continuous_step", {{"on", ContinuousStepOn()}, {"auto", Mode() == StepMode::Auto}, {"frames", c_continuous.load()},
                                                            {"auto_raw_frames", c_auto_raw_frames.load()}, {"auto_switches", c_auto_switches.load()}}},
                                       {"note", "factor = raw frame time * 30; 1 at 30 fps, 0.25 at 120 fps"}});
}

} // namespace Core::NativeGameTime
