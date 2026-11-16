// SPDX-FileCopyrightText: Copyright 2026 tlg_pc
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL_keycode.h>

/// Our in-game dev keys under the engine-to-Vulkan backend (merge plan M3t (1), note 2's table: ours win where they clash): F5 / Shift+F5 frame walk,
/// F6 / F7 full / light glitch marker, F8 stall profiler, F9 / Shift+F9 frame / probe profiler, F12 / Shift+F12 screenshot / 5 s video, Print Screen
/// screenshot. Their F8 (upscaler switch), F9 (photo mode; its pad combo stays) and F12 (Fun move bar) are not reached while the backend runs; with their
/// renderer nothing here runs. F10, F11, Insert and Esc stay the shim's.
namespace Core::DevKeys {

/// Window thread, for a key press (not a repeat): true when it was one of ours (the event is used up)
bool Handle(SDL_Keycode key, SDL_Keymod mod);

/// Once at start under the backend: the glitch marker's and the frame walk's screenshot, toast and thumbnail hooks
void InstallHooks();

} // namespace Core::DevKeys
