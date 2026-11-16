// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "native/native_vkb_write_window.h"

#include <string_view>
#include <unordered_map>

#include "native/shadps4/shader_recompiler/info.h"
#include "native/shadps4/shader_recompiler/ir/abstract_syntax_list.h"
#include "native/shadps4/shader_recompiler/ir/attribute.h"
#include "native/shadps4/shader_recompiler/ir/basic_block.h"
#include "native/shadps4/shader_recompiler/ir/opcodes.h"
#include "native/shadps4/shader_recompiler/ir/program.h"

namespace Core::Vkb::WriteWindow {

namespace {

using VkbShader::IR::Opcode;

/// a_wg * wg.x + a_lid * lid.x + k * cb[binding][index] + c
struct Affine {
    bool ok{};
    u64 a_wg{}, a_lid{}, k{}, c{};
    u32 cb_binding{~0u}, cb_index{};
};

Affine Fail() {
    return {};
}

bool ImmU32(const VkbShader::IR::Value& v, u32& out) {
    if (v.IsImmediate() && v.Type() == VkbShader::IR::Type::U32) {
        out = v.U32();
        return true;
    }
    return false;
}

const VkbShader::IR::Inst* Producer(const VkbShader::IR::Value& v) {
    return v.TryInst();
}

Affine Add(const Affine& x, const Affine& y) {
    if (!x.ok || !y.ok) {
        return Fail();
    }
    Affine r{true, x.a_wg + y.a_wg, x.a_lid + y.a_lid, x.k + y.k, x.c + y.c, x.cb_binding, x.cb_index};
    if (x.k != 0 && y.k != 0 && (x.cb_binding != y.cb_binding || x.cb_index != y.cb_index)) {
        return Fail(); // two different constant-buffer terms
    }
    if (x.k == 0) {
        r.cb_binding = y.cb_binding, r.cb_index = y.cb_index;
    }
    return r;
}

Affine Scale(const Affine& x, u64 m) {
    if (!x.ok) {
        return Fail();
    }
    return {true, x.a_wg * m, x.a_lid * m, x.k * m, x.c * m, x.cb_binding, x.cb_index};
}

Affine Eval(const VkbShader::IR::Value& v, u32 local_x, int depth = 0) {
    if (depth > 32) {
        return Fail();
    }
    if (u32 imm = 0; ImmU32(v, imm)) {
        return {true, 0, 0, 0, imm};
    }
    const VkbShader::IR::Inst* inst = Producer(v);
    if (inst == nullptr) {
        return Fail();
    }
    switch (inst->GetOpcode()) {
    case Opcode::IAdd32:
        return Add(Eval(inst->Arg(0), local_x, depth + 1), Eval(inst->Arg(1), local_x, depth + 1));
    case Opcode::IMul32: {
        u32 m = 0;
        if (ImmU32(inst->Arg(1), m)) {
            return Scale(Eval(inst->Arg(0), local_x, depth + 1), m);
        }
        if (ImmU32(inst->Arg(0), m)) {
            return Scale(Eval(inst->Arg(1), local_x, depth + 1), m);
        }
        return Fail();
    }
    case Opcode::ShiftLeftLogical32: {
        u32 s = 0;
        if (ImmU32(inst->Arg(1), s) && s < 32) {
            return Scale(Eval(inst->Arg(0), local_x, depth + 1), 1ull << s);
        }
        return Fail();
    }
    case Opcode::ReadConstBuffer: {
        u32 binding = 0, index = 0;
        if (ImmU32(inst->Arg(0), binding) && ImmU32(inst->Arg(1), index)) {
            return {true, 0, 0, 1, 0, binding, index};
        }
        return Fail();
    }
    case Opcode::GetAttributeU32: {
        const auto& a = inst->Arg(0);
        u32 comp = 0;
        if (a.Type() != VkbShader::IR::Type::Attribute || !ImmU32(inst->Arg(1), comp) || comp != 0) {
            return Fail();
        }
        switch (a.Attribute()) {
        case VkbShader::IR::Attribute::WorkgroupId:
            return {true, 1, 0, 0, 0};
        case VkbShader::IR::Attribute::LocalInvocationId:
            return {true, 0, 1, 0, 0};
        case VkbShader::IR::Attribute::GlobalInvocationId:
            return {true, local_x, 1, 0, 0};
        default:
            return Fail();
        }
    }
    default:
        return Fail();
    }
}

/// gid.x when `v` is the global id in x
bool IsGlobalId(const VkbShader::IR::Value& v, u32 local_x) {
    const Affine a = Eval(v, local_x);
    return a.ok && a.k == 0 && a.c == 0 && a.a_lid == 1 && a.a_wg == local_x;
}

struct Guard {
    bool ok{};
    bool immediate{}, is_signed{};
    u32 binding{~0u}, index{}, value{};
    bool operator==(const Guard&) const = default;
};

/// `x` as the bound of a guard (an immediate or one constant-buffer dword)
bool BoundOf(const VkbShader::IR::Value& x, bool is_signed, Guard& g) {
    if (u32 imm = 0; ImmU32(x, imm)) {
        g = {true, true, is_signed, ~0u, 0, imm};
        return true;
    }
    const VkbShader::IR::Inst* inst = Producer(x);
    u32 binding = 0, index = 0;
    if (inst != nullptr && inst->GetOpcode() == Opcode::ReadConstBuffer && ImmU32(inst->Arg(0), binding) && ImmU32(inst->Arg(1), index)) {
        g = {true, false, is_signed, binding, index, 0};
        return true;
    }
    return false;
}

/// A condition that, when true, implies gid < bound. `negated`: the condition is under a logical not.
bool Implies(const VkbShader::IR::Value& cond, bool negated, u32 local_x, Guard& g, int depth = 0) {
    if (depth > 16) {
        return false;
    }
    const VkbShader::IR::Inst* inst = Producer(cond);
    if (inst == nullptr) {
        return false;
    }
    switch (inst->GetOpcode()) {
    case Opcode::ConditionRef:
        return Implies(inst->Arg(0), negated, local_x, g, depth + 1);
    case Opcode::LogicalNot:
        return Implies(inst->Arg(0), !negated, local_x, g, depth + 1);
    case Opcode::LogicalAnd: // true: both hold
        return !negated && (Implies(inst->Arg(0), false, local_x, g, depth + 1) || Implies(inst->Arg(1), false, local_x, g, depth + 1));
    case Opcode::LogicalOr: // not(a or b): neither holds
        return negated && (Implies(inst->Arg(0), true, local_x, g, depth + 1) || Implies(inst->Arg(1), true, local_x, g, depth + 1));
    case Opcode::ULessThan32:
    case Opcode::SLessThan32: // gid < X; negated: X <= gid... nothing
        return !negated && IsGlobalId(inst->Arg(0), local_x) && BoundOf(inst->Arg(1), inst->GetOpcode() == Opcode::SLessThan32, g);
    case Opcode::UGreaterThan32:
    case Opcode::SGreaterThan32: // X > gid
        return !negated && IsGlobalId(inst->Arg(1), local_x) && BoundOf(inst->Arg(0), inst->GetOpcode() == Opcode::SGreaterThan32, g);
    case Opcode::ULessThanEqual32:
    case Opcode::SLessThanEqual32: // not(X <= gid) = gid < X
        return negated && IsGlobalId(inst->Arg(1), local_x) && BoundOf(inst->Arg(0), inst->GetOpcode() == Opcode::SLessThanEqual32, g);
    case Opcode::UGreaterThanEqual32:
    case Opcode::SGreaterThanEqual32: // not(gid >= X) = gid < X
        return negated && IsGlobalId(inst->Arg(0), local_x) && BoundOf(inst->Arg(1), inst->GetOpcode() == Opcode::SGreaterThanEqual32, g);
    default:
        return false;
    }
}

u32 StoreComponents(Opcode op) {
    switch (op) {
    case Opcode::StoreBufferU32:
    case Opcode::StoreBufferF32:
        return 1;
    case Opcode::StoreBufferU32x2:
    case Opcode::StoreBufferF32x2:
        return 2;
    case Opcode::StoreBufferU32x3:
    case Opcode::StoreBufferF32x3:
        return 3;
    case Opcode::StoreBufferU32x4:
    case Opcode::StoreBufferF32x4:
        return 4;
    default:
        return 0; // U8, U16, U64, format stores: not handled
    }
}

} // namespace

std::vector<Window> Analyze(const VkbShader::IR::Program& program, const VkbShader::Info& info, u32 local_x) {
    std::vector<Window> out(info.buffers.size());
    if (local_x == 0) {
        return out;
    }
    // the guards of each block: the conditions of the `if`s it is inside (syntax list order: If ... EndIf, nested)
    std::unordered_map<const VkbShader::IR::Block*, std::vector<VkbShader::IR::Value>> conds_of;
    {
        std::vector<std::pair<VkbShader::IR::Value, const VkbShader::IR::Block*>> open; // condition, merge block
        for (const auto& node : program.syntax_list) {
            using T = VkbShader::IR::AbstractSyntaxNode::Type;
            if (node.type == T::If) {
                open.push_back({VkbShader::IR::Value{node.data.if_node.cond}, node.data.if_node.merge});
            } else if (node.type == T::EndIf) {
                if (!open.empty() && open.back().second == node.data.end_if.merge) {
                    open.pop_back();
                } else {
                    return out; // not the nesting expected: no windows
                }
            } else if (node.type == T::Block) {
                auto& list = conds_of[node.data.block];
                for (const auto& [cond, merge] : open) {
                    list.push_back(cond);
                }
            }
        }
    }
    struct Acc {
        bool any_store{}, bad{};
        bool first{true};
        Affine form{};
        u64 c_min{~0ull}, c_max{};
        bool all_guarded{true};
        Guard guard{};
    };
    std::vector<Acc> acc(info.buffers.size());
    for (const VkbShader::IR::Block* block : program.blocks) {
        const auto cit = conds_of.find(block);
        for (const VkbShader::IR::Inst& inst : block->Instructions()) {
            const Opcode op = inst.GetOpcode();
            const std::string_view name = VkbShader::IR::NameOf(op);
            if (name.find("Buffer") == std::string_view::npos || inst.NumArgs() == 0) {
                continue;
            }
            u32 binding = 0;
            if (!ImmU32(inst.Arg(0), binding) || binding >= acc.size()) {
                continue;
            }
            Acc& a = acc[binding];
            const u32 n = StoreComponents(op);
            if (n == 0) {
                a.bad = true; // a load, an atomic, a constant read or a store we do not size
                continue;
            }
            const Affine f = Eval(inst.Arg(1), local_x);
            if (!f.ok) {
                a.bad = true;
                continue;
            }
            if (a.first) {
                a.form = f, a.first = false;
            } else if (f.a_wg != a.form.a_wg || f.a_lid != a.form.a_lid || f.k != a.form.k || (f.k != 0 && (f.cb_binding != a.form.cb_binding || f.cb_index != a.form.cb_index))) {
                a.bad = true;
                continue;
            }
            a.any_store = true;
            a.c_min = std::min(a.c_min, f.c);
            a.c_max = std::max(a.c_max, f.c + n - 1);
            Guard g{};
            bool guarded = false;
            if (cit != conds_of.end()) {
                for (const auto& cond : cit->second) {
                    Guard gi{};
                    if (Implies(cond, false, local_x, gi)) {
                        g = gi, guarded = true;
                        break;
                    }
                }
            }
            if (!guarded) {
                a.all_guarded = false;
            } else if (a.guard.ok && !(a.guard == g)) {
                a.all_guarded = false; // different guards: use none
            } else {
                a.guard = g;
            }
        }
    }
    for (size_t i = 0; i < acc.size(); ++i) {
        const Acc& a = acc[i];
        if (a.bad || !a.any_store || !info.buffers[i].is_written) {
            continue;
        }
        Window& w = out[i];
        w.valid = true;
        w.a_wg = a.form.a_wg, w.a_lid = a.form.a_lid, w.k = a.form.k;
        w.cb_binding = a.form.k != 0 ? a.form.cb_binding : ~0u;
        w.cb_index = a.form.cb_index;
        w.c_min = a.c_min, w.c_max = a.c_max;
        if (a.all_guarded && a.guard.ok) {
            w.guarded = true;
            w.guard_immediate = a.guard.immediate;
            w.guard_signed = a.guard.is_signed;
            w.guard_binding = a.guard.binding;
            w.guard_index = a.guard.index;
            w.guard_value = a.guard.value;
        }
    }
    return out;
}

} // namespace Core::Vkb::WriteWindow
