// SPDX-License-Identifier: GPL-2.0-or-later
// tlg (issue 75): texture reads with automatic level choice out of per-pixel branches (pixel shaders).
//
// The console shaders switch pixels of a 2x2 group on and off inside one wave (exec mask); the translator turns every
// such region into an "if". A read with automatic level choice (plain sample, sample with bias, compare sample, level
// query) works out its level and direction from the neighbours of its 2x2 group. Inside an "if" that not every pixel
// of the group enters, those neighbours did not run the read's coordinate code: Vulkan leaves the result undefined, and
// the driver uses whatever its registers hold. With a sharp filter (forced anisotropic filtering) that shows as
// stripes along the edges of the "if".
//
// This pass moves each such read, together with the pure arithmetic its operands need, to the end of the block before
// the outermost per-pixel "if" around it (passing through branches that are the same for all pixels). A read has no
// side effects, so the values inside the "if" stay as they were; every pixel of the group now runs the coordinate code,
// so the level is defined. The move stops at anything that is not pure arithmetic (buffer and shared memory loads, other kinds
// of texture reads, values merged from branches, lane exchanges, constant reads by raw address or at a per-pixel address), at
// loops, at code after an early return or a per-pixel loop exit, and at shaders that write memory (a read must not pass a
// write); such a
// read stays where it is and is counted with its reason (ReadsOutOfBranchStats).
//
// Switch: TLG_VKB_READS_OUT_OF_BRANCH (default on, =0 the old translation), read once at start (Profile).

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "native/shadps4/shader_recompiler/info.h"
#include "native/shadps4/shader_recompiler/ir/basic_block.h"
#include "native/shadps4/shader_recompiler/ir/passes/ir_passes.h"
#include "native/shadps4/shader_recompiler/ir/program.h"

namespace VkbShader::Optimization {

namespace {

ReadsOutOfBranchStats g_stats;
std::mutex g_shaders_mutex;
std::unordered_map<u64, ReadsOutOfBranchShader> g_shaders; ///< per shader program (by hash), the last translation

bool IsAutoLevelRead(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleDrefImplicitLod ||
           op == IR::Opcode::ImageQueryLod;
}

/// Pure arithmetic: the result depends only on the operands; no memory, no other lanes, no side effects.
bool IsPure(IR::Opcode op) {
    if (op >= IR::Opcode::CompositeConstructU32x2 && op <= IR::Opcode::ConvertS32S16) {
        return true; // vector utility, select, bit casts and packing, float, integer, logical and conversion operations
    }
    if (op >= IR::Opcode::UndefU1 && op <= IR::Opcode::UndefU64) {
        return true;
    }
    switch (op) {
    case IR::Opcode::CubeFaceIndex:
    case IR::Opcode::CubeFaceCoordS:
    case IR::Opcode::CubeFaceCoordT:
    case IR::Opcode::CubeFaceMajorAxis:
    case IR::Opcode::GetAttribute: // the pixel's own inputs (interpolated values, position, sample number)
    case IR::Opcode::GetAttributeU32:
    case IR::Opcode::GetAttributeU1:
        return true;
    default:
        return false;
    }
}

ReadsOutOfBranchReason ReasonOf(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::Phi:
        return ReadsOutOfBranchReason::MergedValue;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        return ReadsOutOfBranchReason::ConstantRead;
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::LoadSharedU64:
        return ReadsOutOfBranchReason::MemoryLoad;
    case IR::Opcode::ImageSampleExplicitLod:
    case IR::Opcode::ImageSampleDrefExplicitLod:
    case IR::Opcode::ImageGather:
    case IR::Opcode::ImageGatherDref:
    case IR::Opcode::ImageGradient:
    case IR::Opcode::ImageRead:
    case IR::Opcode::ImageQueryDimensions:
        return ReadsOutOfBranchReason::TextureRead;
    case IR::Opcode::LaneId:
    case IR::Opcode::WarpId:
    case IR::Opcode::QuadBroadcast:
    case IR::Opcode::Shuffle:
    case IR::Opcode::ShuffleXor:
    case IR::Opcode::ReadFirstLane:
    case IR::Opcode::ReadLane:
    case IR::Opcode::WriteLane:
    case IR::Opcode::Ballot:
    case IR::Opcode::BallotFindLsb:
    case IR::Opcode::InverseBallot:
    case IR::Opcode::GroupAny:
        return ReadsOutOfBranchReason::LaneExchange;
    default:
        if (op >= IR::Opcode::LoadBufferU8 && op <= IR::Opcode::LoadBufferFormatF32) {
            return ReadsOutOfBranchReason::MemoryLoad;
        }
        return ReadsOutOfBranchReason::Other;
    }
}

/// True when the value is provably the same for every pixel of the wave: built only from immediates, user data and
/// constant buffer reads at such addresses (used only in shaders that write no memory). Everything else (inputs, texture and buffer reads, merged values) counts as per
/// pixel: a branch on it is treated as a per-pixel branch (the safe side: at worst a read moves that did not need to).
class Uniformity {
public:
    bool Of(const IR::Value& value) {
        if (value.IsImmediate()) {
            return true;
        }
        IR::Inst* inst = value.TryInst();
        if (inst == nullptr) {
            return false;
        }
        if (const auto it = memo.find(inst); it != memo.end()) {
            return it->second;
        }
        memo[inst] = false; // a cycle (only through merged values, which are per pixel) stays per pixel
        const IR::Opcode op = inst->GetOpcode();
        bool uniform = false;
        if (op == IR::Opcode::GetUserData || op == IR::Opcode::ReadConstBuffer ||
            op == IR::Opcode::ConditionRef || (IsPure(op) && op != IR::Opcode::GetAttribute && op != IR::Opcode::GetAttributeU32 &&
                                               op != IR::Opcode::GetAttributeU1)) {
            uniform = true;
            for (size_t i = 0; i < inst->NumArgs() && uniform; ++i) {
                const IR::Value arg = inst->Arg(i);
                if (!arg.IsImmediate()) {
                    uniform = Of(arg);
                }
            }
        }
        memo[inst] = uniform;
        return uniform;
    }

private:
    std::unordered_map<const IR::Inst*, bool> memo;
};

/// The shader writes memory (buffers, images): then a read of memory must keep its place after those writes.
bool WritesMemory(const IR::Program& program) {
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (!inst.MayHaveSideEffects()) {
                continue;
            }
            switch (inst.GetOpcode()) {
            case IR::Opcode::ConditionRef:
            case IR::Opcode::Prologue:
            case IR::Opcode::Epilogue:
            case IR::Opcode::Discard:
            case IR::Opcode::DiscardCond:
            case IR::Opcode::SetAttribute: // the pixel's outputs
            case IR::Opcode::DebugPrint:
                break;
            default:
                return true;
            }
        }
    }
    return false;
}

struct Construct {
    bool is_loop{};
    bool exited{};    ///< a loop that some pixels may leave early (a per-pixel exit was seen so far)
    bool per_pixel{}; ///< an "if" whose condition can differ between pixels
    IR::Block* header{}; ///< an "if": the block that ends with the branch into it
};

struct Place {
    std::vector<u32> constructs; ///< the "if"s and loops around the block, outermost first
    bool after_early_return{};   ///< some pixels may have left the shader before this block
    bool after_loop_exit{};      ///< some pixels may have left a loop around this block before it
};

} // namespace

ReadsOutOfBranchStats& ReadsOutOfBranchCounters() {
    return g_stats;
}

std::vector<ReadsOutOfBranchShader> ReadsOutOfBranchShaders() {
    std::scoped_lock lock{g_shaders_mutex};
    std::vector<ReadsOutOfBranchShader> out;
    out.reserve(g_shaders.size());
    for (const auto& [hash, s] : g_shaders) {
        out.push_back(s);
    }
    std::ranges::sort(out, {}, &ReadsOutOfBranchShader::hash);
    return out;
}

void ReadsOutOfBranchPass(IR::Program& program, bool move) {
    if (program.info.sw_stage != SwStage::Fragment) {
        return;
    }
    g_stats.pixel_shaders.fetch_add(1, std::memory_order_relaxed);

    // Where every block sits: the constructs around it, from the structured list.
    std::vector<Construct> constructs;
    std::unordered_map<const IR::Block*, Place> places;
    std::vector<u32> stack;
    IR::Block* last_block = nullptr;
    bool after_return = false;
    bool structure_ok = true;
    Uniformity uniformity;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block:
            places[node.data.block] = Place{stack, after_return, std::ranges::any_of(stack, [&](u32 c) { return constructs[c].exited; })};
            last_block = node.data.block;
            break;
        case IR::AbstractSyntaxNode::Type::If: {
            const auto& preds = node.data.if_node.body->ImmPredecessors();
            IR::Block* header = last_block;
            if (header == nullptr || preds.size() != 1 || preds[0] != header) {
                structure_ok = false;
            }
            constructs.push_back(Construct{false, false, !uniformity.Of(node.data.if_node.cond), header});
            stack.push_back(static_cast<u32>(constructs.size() - 1));
            break;
        }
        case IR::AbstractSyntaxNode::Type::Loop:
            constructs.push_back(Construct{true, false, false, nullptr});
            stack.push_back(static_cast<u32>(constructs.size() - 1));
            break;
        case IR::AbstractSyntaxNode::Type::EndIf:
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (stack.empty()) {
                structure_ok = false;
            } else {
                stack.pop_back();
            }
            break;
        case IR::AbstractSyntaxNode::Type::Return:
            if (std::ranges::any_of(stack, [&](u32 c) { return constructs[c].per_pixel; })) {
                after_return = true; // pixels that left the shader are missing for every read after this point
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (!uniformity.Of(node.data.break_node.cond) || std::ranges::any_of(stack, [&](u32 c) { return constructs[c].per_pixel; })) {
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (constructs[*it].is_loop) {
                        constructs[*it].exited = true; // pixels that left are missing for the rest of this loop
                        break;
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    // A constant buffer read at an address that is the same for every pixel may move with the read: on the console it is
    // a scalar load, which runs for the whole wave whatever the exec mask says, so before the "if" is where the console has
    // it; out of range it reads zero (the translator's bound, else the device's robust buffer access, required at start).
    // Not a constant read by raw address (its fault check records what it touches).
    const bool writes_memory = WritesMemory(program);
    const auto movable_constant = [&](IR::Inst* inst) {
        if (inst->GetOpcode() != IR::Opcode::ReadConstBuffer) {
            return false;
        }
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            if (!uniformity.Of(inst->Arg(i))) {
                return false;
            }
        }
        return true;
    };

    ReadsOutOfBranchShader shader{.hash = program.info.pgm_hash};
    const auto place_of = [&](const IR::Block* block) -> const Place* {
        const auto it = places.find(block);
        return it == places.end() ? nullptr : &it->second;
    };
    std::vector<const IR::Inst*> counted; ///< switch off: what a move would have taken out already
    // An instruction is "inside" construct c when its block sits in c.
    const auto inside = [&](const IR::Inst* inst, u32 c) {
        if (std::ranges::find(counted, inst) != counted.end()) {
            return false;
        }
        const Place* p = place_of(inst->GetParent());
        return p == nullptr || std::ranges::find(p->constructs, c) != p->constructs.end();
    };

    std::vector<IR::Inst*> reads;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (IsAutoLevelRead(inst.GetOpcode())) {
                reads.push_back(&inst);
            }
        }
    }
    shader.reads = static_cast<u32>(reads.size());

    std::vector<const IR::Inst*> taken; ///< reads already moved as an operand of another read
    for (IR::Inst* read : reads) {
        if (std::ranges::find(taken, read) != taken.end()) {
            continue;
        }
        const Place* place = place_of(read->GetParent());
        if (place == nullptr) {
            ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::Other)];
            continue;
        }
        const bool in_per_pixel = std::ranges::any_of(place->constructs, [&](u32 c) { return constructs[c].per_pixel; });
        if (!in_per_pixel) {
            if (place->after_early_return) {
                ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::AfterEarlyReturn)];
            } else if (place->after_loop_exit) {
                ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::Loop)];
            } else if (!place->constructs.empty()) {
                ++shader.in_uniform_branch; // the same for all pixels: the level is defined
            } else {
                ++shader.outside_branches;
            }
            continue;
        }
        if (!structure_ok) {
            ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::Other)];
            continue;
        }
        if (writes_memory) {
            ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::WritesMemory)];
            continue;
        }
        // The outermost per-pixel "if" around the read with no loop between them and the read.
        s32 target = -1;
        bool loop = false;
        for (auto it = place->constructs.rbegin(); it != place->constructs.rend(); ++it) {
            if (constructs[*it].is_loop) {
                loop = true;
                break;
            }
            if (constructs[*it].per_pixel) {
                target = static_cast<s32>(*it);
            }
        }
        if (loop || target < 0 || place->after_loop_exit) {
            ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::Loop)];
            continue;
        }
        const Construct& c = constructs[target];
        const Place* header_place = place_of(c.header);
        if (header_place == nullptr || header_place->after_early_return) {
            ++shader.not_moved[static_cast<u32>(ReadsOutOfBranchReason::AfterEarlyReturn)];
            continue;
        }
        // The read and the operands it needs from inside the "if", operands first.
        std::vector<IR::Inst*> order;
        std::vector<IR::Inst*> seen;
        ReadsOutOfBranchReason reason{};
        bool ok = true;
        const auto collect = [&](auto&& self, IR::Inst* inst) -> void {
            if (!ok || std::ranges::find(seen, inst) != seen.end()) {
                return;
            }
            seen.push_back(inst);
            if (!inside(inst, static_cast<u32>(target))) {
                return; // defined before the "if": available at the header
            }
            const IR::Opcode op = inst->GetOpcode();
            if (inst != read && !IsPure(op) && !IsAutoLevelRead(op) && !movable_constant(inst)) {
                ok = false;
                reason = ReasonOf(op);
                return;
            }
            for (size_t i = 0; i < inst->NumArgs(); ++i) {
                const IR::Value arg = inst->Arg(i);
                if (IR::Inst* a = arg.TryInst(); a != nullptr) {
                    self(self, a);
                }
            }
            order.push_back(inst);
        };
        collect(collect, read);
        if (!ok) {
            ++shader.not_moved[static_cast<u32>(reason)];
            continue;
        }
        for (IR::Inst* inst : order) {
            if (IsAutoLevelRead(inst->GetOpcode())) {
                ++shader.moved;
                taken.push_back(inst);
            }
        }
        shader.ops_moved += static_cast<u32>(order.size());
        if (!move) {
            counted.insert(counted.end(), order.begin(), order.end()); // switch off: count only
            continue;
        }
        for (IR::Inst* inst : order) {
            IR::Block* from = inst->GetParent();
            from->Instructions().erase(IR::Block::InstructionList::s_iterator_to(*inst));
            c.header->Instructions().push_back(*inst);
            inst->SetParent(c.header);
        }
    }

    u32 not_moved = 0;
    for (u32 i = 0; i < ReadsOutOfBranchReasonCount; ++i) {
        not_moved += shader.not_moved[i];
        g_stats.not_moved[i].fetch_add(shader.not_moved[i], std::memory_order_relaxed);
    }
    g_stats.reads.fetch_add(shader.reads, std::memory_order_relaxed);
    g_stats.moved.fetch_add(shader.moved, std::memory_order_relaxed);
    g_stats.ops_moved.fetch_add(shader.ops_moved, std::memory_order_relaxed);
    g_stats.in_uniform_branch.fetch_add(shader.in_uniform_branch, std::memory_order_relaxed);
    g_stats.outside_branches.fetch_add(shader.outside_branches, std::memory_order_relaxed);
    if (shader.moved != 0 || not_moved != 0) {
        g_stats.shaders_with_reads_in_branch.fetch_add(1, std::memory_order_relaxed);
        shader.applied = move;
        std::scoped_lock lock{g_shaders_mutex};
        g_shaders[shader.hash] = shader;
    }
}

} // namespace VkbShader::Optimization
