// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
// From the old port's shim/window.cpp key handlers and shim/tlggpu.cpp hook setup (merge plan M3t (1)).

#include "native/native_dev_keys.h"

#include <ctime>
#include <string>
#include "native/native_capture.h"
#include "native/native_framewalk.h"
#include "native/native_marker.h"
#include "native/native_prof.h"
#include "native/native_stall.h"
#include "native/native_timeline.h"
#include "tlg_overlay.h"

namespace Core::DevKeys {

bool Handle(SDL_Keycode key, SDL_Keymod mod) {
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    switch (key) {
    case SDLK_F9:
        // F9: the frame profiler (a 10 s capture into user/log/timeline_f9_<time>.bin/.json) or stops a running one. Shift+F9: the probe profiler
        // (10 s of the named probes; report user/log/prof_<time>.html), or stops it.
        if (shift || Core::Prof::Recording()) {
            if (!Core::Prof::Stop()) {
                Core::Prof::Start(Core::Prof::DefaultConfig());
            }
        } else if (!Core::FrameTimeline::StopCapture()) {
            Core::FrameTimeline::StartCapture(10, "f9_" + std::to_string(static_cast<long long>(std::time(nullptr))));
        }
        return true;
    case SDLK_F8:
        // the stall profiler (10 s: what the game's threads wait on, with the guest call site; user/log/stall_<time>.html), or stops a running one
        if (!Core::Stall::Stop()) {
            Core::Stall::Start(10, true);
        }
        return true;
    case SDLK_F5:
        // frame walk: Shift+F5 records every frame event into a ring; F5 freezes at the next frame boundary and writes user/log/framewalk_<time>/, F5 again resumes
        if (shift) {
            Core::FrameWalk::KeyRecord();
        } else {
            Core::FrameWalk::KeyFreeze();
        }
        return true;
    case SDLK_F6:
    case SDLK_F7:
        // glitch marker: F6 full (flight recorder tail, draw table, counters, the presented frame, a note), F7 light; written by a thread of its own
        Core::Marker::Request(key == SDLK_F6 ? 1 : 2);
        return true;
    case SDLK_F12:
        // F12 a screenshot, Shift+F12 5 s of video (user/screenshots)
        if (shift) {
            TlgCapture::RequestVideo(5.0);
        } else {
            TlgCapture::RequestScreenshot();
        }
        return true;
    case SDLK_PRINTSCREEN:
        TlgCapture::RequestScreenshot();
        return true;
    default:
        return false;
    }
}

void InstallHooks() {
    Core::Marker::SetHooks({.screenshot = [](const std::string& path) { TlgCapture::RequestScreenshotTo(path); },
                            .toast = [](int kind) { TlgOverlay::SaveToast(kind); }});
    Core::FrameWalk::SetToastHook([](int kind) { TlgOverlay::SaveToast(kind); });
    Core::FrameWalk::SetThumbHooks({.enable = [](bool on) { TlgCapture::SetThumbs(on); },
                                    .dump = [](const std::string& path) { return TlgCapture::DumpThumbs(path); }});
}

} // namespace Core::DevKeys
