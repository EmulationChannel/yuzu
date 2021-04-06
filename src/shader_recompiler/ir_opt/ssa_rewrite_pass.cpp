// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// This file implements the SSA rewriting algorithm proposed in
//
//      Simple and Efficient Construction of Static Single Assignment Form.
//      Braun M., Buchwald S., Hack S., Leiba R., Mallon C., Zwinkau A. (2013)
//      In: Jhala R., De Bosschere K. (eds)
//      Compiler Construction. CC 2013.
//      Lecture Notes in Computer Science, vol 7791.
//      Springer, Berlin, Heidelberg
//
//      https://link.springer.com/chapter/10.1007/978-3-642-37051-9_6
//

#include <ranges>
#include <span>
#include <variant>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/microinstruction.h"
#include "shader_recompiler/frontend/ir/opcodes.h"
#include "shader_recompiler/frontend/ir/pred.h"
#include "shader_recompiler/frontend/ir/reg.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
struct FlagTag {
    auto operator<=>(const FlagTag&) const noexcept = default;
};
struct ZeroFlagTag : FlagTag {};
struct SignFlagTag : FlagTag {};
struct CarryFlagTag : FlagTag {};
struct OverflowFlagTag : FlagTag {};

struct GotoVariable : FlagTag {
    GotoVariable() = default;
    explicit GotoVariable(u32 index_) : index{index_} {}

    auto operator<=>(const GotoVariable&) const noexcept = default;

    u32 index;
};

struct IndirectBranchVariable {
    auto operator<=>(const IndirectBranchVariable&) const noexcept = default;
};

using Variant = std::variant<IR::Reg, IR::Pred, ZeroFlagTag, SignFlagTag, CarryFlagTag,
                             OverflowFlagTag, GotoVariable, IndirectBranchVariable>;
using ValueMap = boost::container::flat_map<IR::Block*, IR::Value, std::less<IR::Block*>>;

struct DefTable {
    [[nodiscard]] ValueMap& operator[](IR::Reg variable) noexcept {
        return regs[IR::RegIndex(variable)];
    }

    [[nodiscard]] ValueMap& operator[](IR::Pred variable) noexcept {
        return preds[IR::PredIndex(variable)];
    }

    [[nodiscard]] ValueMap& operator[](GotoVariable goto_variable) {
        return goto_vars[goto_variable.index];
    }

    [[nodiscard]] ValueMap& operator[](IndirectBranchVariable) {
        return indirect_branch_var;
    }

    [[nodiscard]] ValueMap& operator[](ZeroFlagTag) noexcept {
        return zero_flag;
    }

    [[nodiscard]] ValueMap& operator[](SignFlagTag) noexcept {
        return sign_flag;
    }

    [[nodiscard]] ValueMap& operator[](CarryFlagTag) noexcept {
        return carry_flag;
    }

    [[nodiscard]] ValueMap& operator[](OverflowFlagTag) noexcept {
        return overflow_flag;
    }

    std::array<ValueMap, IR::NUM_USER_REGS> regs;
    std::array<ValueMap, IR::NUM_USER_PREDS> preds;
    boost::container::flat_map<u32, ValueMap> goto_vars;
    ValueMap indirect_branch_var;
    ValueMap zero_flag;
    ValueMap sign_flag;
    ValueMap carry_flag;
    ValueMap overflow_flag;
};

IR::Opcode UndefOpcode(IR::Reg) noexcept {
    return IR::Opcode::UndefU32;
}

IR::Opcode UndefOpcode(IR::Pred) noexcept {
    return IR::Opcode::UndefU1;
}

IR::Opcode UndefOpcode(const FlagTag&) noexcept {
    return IR::Opcode::UndefU1;
}

IR::Opcode UndefOpcode(IndirectBranchVariable) noexcept {
    return IR::Opcode::UndefU32;
}

[[nodiscard]] bool IsPhi(const IR::Inst& inst) noexcept {
    return inst.GetOpcode() == IR::Opcode::Phi;
}

enum class Status {
    Start,
    SetValue,
    PreparePhiArgument,
    PushPhiArgument,
};

template <typename Type>
struct ReadState {
    ReadState(IR::Block* block_) : block{block_} {}
    ReadState() = default;

    IR::Block* block{};
    IR::Value result{};
    IR::Inst* phi{};
    IR::Block* const* pred_it{};
    IR::Block* const* pred_end{};
    Status pc{Status::Start};
};

class Pass {
public:
    template <typename Type>
    void WriteVariable(Type variable, IR::Block* block, const IR::Value& value) {
        current_def[variable].insert_or_assign(block, value);
    }

    template <typename Type>
    IR::Value ReadVariable(Type variable, IR::Block* root_block) {
        boost::container::small_vector<ReadState<Type>, 64> stack{
            ReadState<Type>(nullptr),
            ReadState<Type>(root_block),
        };
        const auto prepare_phi_operand{[&] {
            if (stack.back().pred_it == stack.back().pred_end) {
                IR::Inst* const phi{stack.back().phi};
                IR::Block* const block{stack.back().block};
                const IR::Value result{TryRemoveTrivialPhi(*phi, block, UndefOpcode(variable))};
                stack.pop_back();
                stack.back().result = result;
                WriteVariable(variable, block, result);
            } else {
                IR::Block* const imm_pred{*stack.back().pred_it};
                stack.back().pc = Status::PushPhiArgument;
                stack.emplace_back(imm_pred);
            }
        }};
        do {
            IR::Block* const block{stack.back().block};
            switch (stack.back().pc) {
            case Status::Start: {
                const ValueMap& def{current_def[variable]};
                if (const auto it{def.find(block)}; it != def.end()) {
                    stack.back().result = it->second;
                } else if (!sealed_blocks.contains(block)) {
                    // Incomplete CFG
                    IR::Inst* phi{&*block->PrependNewInst(block->begin(), IR::Opcode::Phi)};
                    incomplete_phis[block].insert_or_assign(variable, phi);
                    stack.back().result = IR::Value{&*phi};
                } else if (const std::span imm_preds{block->ImmediatePredecessors()};
                           imm_preds.size() == 1) {
                    // Optimize the common case of one predecessor: no phi needed
                    stack.back().pc = Status::SetValue;
                    stack.emplace_back(imm_preds.front());
                    break;
                } else {
                    // Break potential cycles with operandless phi
                    IR::Inst* const phi{&*block->PrependNewInst(block->begin(), IR::Opcode::Phi)};
                    WriteVariable(variable, block, IR::Value{phi});

                    stack.back().phi = phi;
                    stack.back().pred_it = imm_preds.data();
                    stack.back().pred_end = imm_preds.data() + imm_preds.size();
                    prepare_phi_operand();
                    break;
                }
            }
                [[fallthrough]];
            case Status::SetValue: {
                const IR::Value result{stack.back().result};
                WriteVariable(variable, block, result);
                stack.pop_back();
                stack.back().result = result;
                break;
            }
            case Status::PushPhiArgument: {
                IR::Inst* const phi{stack.back().phi};
                phi->AddPhiOperand(*stack.back().pred_it, stack.back().result);
                ++stack.back().pred_it;
            }
                [[fallthrough]];
            case Status::PreparePhiArgument:
                prepare_phi_operand();
                break;
            }
        } while (stack.size() > 1);
        return stack.back().result;
    }

    void SealBlock(IR::Block* block) {
        const auto it{incomplete_phis.find(block)};
        if (it != incomplete_phis.end()) {
            for (auto& [variant, phi] : it->second) {
                std::visit([&](auto& variable) { AddPhiOperands(variable, *phi, block); }, variant);
            }
        }
        sealed_blocks.insert(block);
    }

private:
    template <typename Type>
    IR::Value AddPhiOperands(Type variable, IR::Inst& phi, IR::Block* block) {
        for (IR::Block* const imm_pred : block->ImmediatePredecessors()) {
            phi.AddPhiOperand(imm_pred, ReadVariable(variable, imm_pred));
        }
        return TryRemoveTrivialPhi(phi, block, UndefOpcode(variable));
    }

    IR::Value TryRemoveTrivialPhi(IR::Inst& phi, IR::Block* block, IR::Opcode undef_opcode) {
        IR::Value same;
        const size_t num_args{phi.NumArgs()};
        for (size_t arg_index = 0; arg_index < num_args; ++arg_index) {
            const IR::Value& op{phi.Arg(arg_index)};
            if (op.Resolve() == same.Resolve() || op == IR::Value{&phi}) {
                // Unique value or self-reference
                continue;
            }
            if (!same.IsEmpty()) {
                // The phi merges at least two values: not trivial
                return IR::Value{&phi};
            }
            same = op;
        }
        if (same.IsEmpty()) {
            // The phi is unreachable or in the start block
            // First remove the phi node from the block, it will be reinserted
            IR::Block::InstructionList& list{block->Instructions()};
            list.erase(IR::Block::InstructionList::s_iterator_to(phi));

            // Insert an undef instruction after all phi nodes (to keep phi instructions on top)
            const auto first_not_phi{std::ranges::find_if_not(list, IsPhi)};
            same = IR::Value{&*block->PrependNewInst(first_not_phi, undef_opcode)};

            // Insert the phi node after the undef opcode, this will be replaced with an identity
            list.insert(first_not_phi, phi);
        }
        // Reroute all uses of phi to same and remove phi
        phi.ReplaceUsesWith(same);
        // TODO: Try to recursively remove all phi users, which might have become trivial
        return same;
    }

    boost::container::flat_set<IR::Block*> sealed_blocks;
    boost::container::flat_map<IR::Block*, boost::container::flat_map<Variant, IR::Inst*>>
        incomplete_phis;
    DefTable current_def;
};

void VisitInst(Pass& pass, IR::Block* block, IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::SetRegister:
        if (const IR::Reg reg{inst.Arg(0).Reg()}; reg != IR::Reg::RZ) {
            pass.WriteVariable(reg, block, inst.Arg(1));
        }
        break;
    case IR::Opcode::SetPred:
        if (const IR::Pred pred{inst.Arg(0).Pred()}; pred != IR::Pred::PT) {
            pass.WriteVariable(pred, block, inst.Arg(1));
        }
        break;
    case IR::Opcode::SetGotoVariable:
        pass.WriteVariable(GotoVariable{inst.Arg(0).U32()}, block, inst.Arg(1));
        break;
    case IR::Opcode::SetIndirectBranchVariable:
        pass.WriteVariable(IndirectBranchVariable{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetZFlag:
        pass.WriteVariable(ZeroFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetSFlag:
        pass.WriteVariable(SignFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetCFlag:
        pass.WriteVariable(CarryFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::SetOFlag:
        pass.WriteVariable(OverflowFlagTag{}, block, inst.Arg(0));
        break;
    case IR::Opcode::GetRegister:
        if (const IR::Reg reg{inst.Arg(0).Reg()}; reg != IR::Reg::RZ) {
            inst.ReplaceUsesWith(pass.ReadVariable(reg, block));
        }
        break;
    case IR::Opcode::GetPred:
        if (const IR::Pred pred{inst.Arg(0).Pred()}; pred != IR::Pred::PT) {
            inst.ReplaceUsesWith(pass.ReadVariable(pred, block));
        }
        break;
    case IR::Opcode::GetGotoVariable:
        inst.ReplaceUsesWith(pass.ReadVariable(GotoVariable{inst.Arg(0).U32()}, block));
        break;
    case IR::Opcode::GetIndirectBranchVariable:
        inst.ReplaceUsesWith(pass.ReadVariable(IndirectBranchVariable{}, block));
        break;
    case IR::Opcode::GetZFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(ZeroFlagTag{}, block));
        break;
    case IR::Opcode::GetSFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(SignFlagTag{}, block));
        break;
    case IR::Opcode::GetCFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(CarryFlagTag{}, block));
        break;
    case IR::Opcode::GetOFlag:
        inst.ReplaceUsesWith(pass.ReadVariable(OverflowFlagTag{}, block));
        break;
    default:
        break;
    }
}

void VisitBlock(Pass& pass, IR::Block* block) {
    for (IR::Inst& inst : block->Instructions()) {
        VisitInst(pass, block, inst);
    }
    pass.SealBlock(block);
}
} // Anonymous namespace

void SsaRewritePass(IR::Program& program) {
    Pass pass;
    for (IR::Block* const block : program.post_order_blocks | std::views::reverse) {
        VisitBlock(pass, block);
    }
}

} // namespace Shader::Optimization
