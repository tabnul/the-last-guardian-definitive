// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <vector>
#include "common/types.h"
#include "native/shadps4/core/guest_heap.h"
#include "native/native_draw.h"

/// What the compute (V3) and graphics (V4) parts of the engine-to-Vulkan backend share: reading engine objects, the variant hash, the class of a bound object, and the slot map
/// (which engine slot the dwords a resource fetch reads name, from a layout built with a tag per slot).
namespace Core::Vkb::Util {

template <typename T>
T Rd(u64 base, u64 offset) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(base + offset), sizeof(T));
    return value;
}

inline u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

/// The properties of a bound engine view / buffer that can change a module's specialization: the view's format code and kind, the resource's kind, format, array and
/// multisample flags, a buffer's flags and structure stride (read from the engine objects, not from descriptor words; no sizes)
inline u64 ObjectClass(u64 object) {
    if (object == 0) {
        return 0x51;
    }
    if (!IsHeapPointer(object)) {
        return 0x52; // note 67: not an engine object (an unused slot can hold anything); never dereferenced
    }
    u64 h = 0;
    const u32 tag = Rd<u32>(object, 0x18);
    h = Mix(h, tag);
    u64 resource = object;
    if (tag >= 22 && tag <= 25) { // a view: its format code and kind, then its resource
        h = Mix(h, Rd<u32>(object, 0x40));
        h = Mix(h, Rd<u32>(object, 0x44));
        resource = Rd<u64>(object, 0x30);
    }
    if (resource != 0 && IsHeapPointer(resource)) {
        const u32 kind = static_cast<u32>(Rd<u64>(resource, 0x120) >> 32);
        const auto d = [&](u32 i) { return Rd<u32>(resource, 0x180 + i * 4); };
        h = Mix(h, kind);
        switch (kind) {
        case 1: h = Mix(Mix(h, d(4)), d(5)); break;                    // MiscFlags, StructureByteStride
        case 2: h = Mix(Mix(h, d(3)), d(2) > 1); break;                // Format, array
        case 3: h = Mix(Mix(Mix(h, d(4)), d(3) > 1), d(5) > 1); break; // Format, array, multisampled
        case 4: h = Mix(h, d(4)); break;                               // Format
        default: break;
        }
    }
    return h;
}

/// Where a resource a module reads comes from
struct Source {
    enum class Class : u8 { Slot, Special, ProgramConstant, Unmapped } cls{Class::Unmapped};
    u8 kind{};  ///< for a slot: 1 resource, 2 sampler, 3 constant buffer, 4 unordered access, 5 vertex buffer table element (UserLayout tags)
    u16 slot{};
    u64 address{}; ///< a program constant: where the buffer is (fixed: the shader's own memory)
    u64 size{};
};

/// Which slot the dwords a resource fetch reads from the tagged layout's flat user data name
template <typename Fetch>
Source SourceOf(const Fetch& fetch, const std::vector<u32>& flat, bool special, bool program_constant) {
    Source s;
    if (special) {
        s.cls = Source::Class::Special;
        return s;
    }
    using Summary = typename Fetch::Summary;
    u32 first = ~0u;
    if (fetch.summary == Summary::SingleLoad) {
        first = fetch.offsets[0];
    } else if (fetch.summary == Summary::MultiLoad && (fetch.load_mask & 1) != 0) {
        first = fetch.offsets[0];
    } else if (fetch.summary == Summary::MultiLoad && fetch.load_mask == 0 && program_constant) {
        s.cls = Source::Class::ProgramConstant;
        return s;
    }
    u32 kind = 0, slot = 0;
    if (first < flat.size() && Core::NativeDraw::UserLayout::TagOf(flat[first], kind, slot)) {
        s.cls = Source::Class::Slot;
        s.kind = static_cast<u8>(kind);
        s.slot = static_cast<u16>(slot);
    }
    return s;
}

} // namespace Core::Vkb::Util
