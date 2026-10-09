#include "ConstPropagation.hpp"

#include "Instruction.hpp"
#include "logging.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

ConstantInt *ConstFolder::compute(Instruction::OpID op, ConstantInt *value1, ConstantInt *value2) {
    int c_value1 = value1->get_value();
    int c_value2 = value2->get_value();

    // Compute in i64 first. Refusing an overflowing i32 fold preserves the IR
    // without invoking the compiler host's signed-arithmetic undefined behavior.
    if (op == Instruction::add || op == Instruction::sub || op == Instruction::mul) {
        int64_t lhs = c_value1, rhs = c_value2;
        int64_t result = op == Instruction::add ? lhs + rhs :
                         op == Instruction::sub ? lhs - rhs : lhs * rhs;
        if (result < std::numeric_limits<int32_t>::min() || result > std::numeric_limits<int32_t>::max())
            return nullptr;
        return ConstantInt::get(static_cast<int>(result), module_);
    }
    switch (op) {
    case Instruction::sdiv:
        if (c_value2 == 0 || (c_value1 == std::numeric_limits<int32_t>::min() && c_value2 == -1))
            return nullptr;
        return ConstantInt::get(static_cast<int>(c_value1 / c_value2), module_);
        break;
    case Instruction::eq:
        return ConstantInt::get(c_value1 == c_value2, module_);
        break;
    case Instruction::ne:
        return ConstantInt::get(c_value1 != c_value2, module_);
        break;
    case Instruction::gt:
        return ConstantInt::get(c_value1 > c_value2, module_);
        break;
    case Instruction::ge:
        return ConstantInt::get(c_value1 >= c_value2, module_);
        break;
    case Instruction::lt:
        return ConstantInt::get(c_value1 < c_value2, module_);
        break;
    case Instruction::le:
        return ConstantInt::get(c_value1 <= c_value2, module_);
        break;
    default:
        return nullptr;
        break;
    }
}

Constant *ConstFolder::compute(Instruction::OpID op, ConstantFP *value1, ConstantFP *value2) {
    float c_value1 = value1->get_value();
    float c_value2 = value2->get_value();
    // LightIR emits ueq/une/ugt/uge/ult/ule (unordered predicates).
    const bool unordered = std::isnan(c_value1) || std::isnan(c_value2);
    switch (op) {
    case Instruction::fadd:
        return ConstantFP::get(c_value1 + c_value2, module_);
        break;
    case Instruction::fsub:
        return ConstantFP::get(c_value1 - c_value2, module_);
        break;
    case Instruction::fmul:
        return ConstantFP::get(c_value1 * c_value2, module_);
        break;
    case Instruction::fdiv:
        return ConstantFP::get(c_value1 / c_value2, module_);
        break;
    case Instruction::feq:
        return ConstantInt::get(unordered || c_value1 == c_value2, module_);
        break;
    case Instruction::fne:
        return ConstantInt::get(unordered || c_value1 != c_value2, module_);
        break;
    case Instruction::fgt:
        return ConstantInt::get(unordered || c_value1 > c_value2, module_);
        break;
    case Instruction::fge:
        return ConstantInt::get(unordered || c_value1 >= c_value2, module_);
        break;
    case Instruction::flt:
        return ConstantInt::get(unordered || c_value1 < c_value2, module_);
        break;
    case Instruction::fle:
        return ConstantInt::get(unordered || c_value1 <= c_value2, module_);
        break;
    default:
        return nullptr;
        break;
    }
}
ConstantFP *ConstFolder::compute(Instruction::OpID op, ConstantInt *value1) {
    int c_value1 = value1->get_value();

    switch (op) {
    case Instruction::sitofp:
        return ConstantFP::get((float) c_value1, module_);
        break;

    default:
        return nullptr;
        break;
    }
}

ConstantInt *ConstFolder::compute(Instruction::OpID op, ConstantFP *value1) {
    float c_value1 = value1->get_value();
    switch (op) {
    case Instruction::fptosi:
        if (!std::isfinite(c_value1) || std::trunc(static_cast<double>(c_value1)) < std::numeric_limits<int32_t>::min() ||
            std::trunc(static_cast<double>(c_value1)) > std::numeric_limits<int32_t>::max())
            return nullptr;
        return ConstantInt::get(static_cast<int>(c_value1), module_);
        break;

    default:
        return nullptr;
        break;
    }
}

ConstantFP *cast_constantfp(Value *value) {
    auto constant_fp_ptr = dynamic_cast<ConstantFP *>(value);
    if (constant_fp_ptr) {
        return constant_fp_ptr;
    }
    return nullptr;
}
ConstantInt *cast_constantint(Value *value) {
    auto constant_int_ptr = dynamic_cast<ConstantInt *>(value);
    if (constant_int_ptr) {
        return constant_int_ptr;
    }
    return nullptr;
}

static bool same_value(Value *lhs, Value *rhs) {
    if (lhs == rhs) return true;
    if (lhs->get_type() != rhs->get_type()) return false;
    if (auto a = cast_constantint(lhs)) {
        auto b = cast_constantint(rhs);
        return b && a->get_value() == b->get_value();
    }
    if (auto a = cast_constantfp(lhs)) {
        auto b = cast_constantfp(rhs);
        if (!b) return false;
        // Numeric == merges +0/-0 and rejects identical NaN bit patterns.
        float av = a->get_value(), bv = b->get_value();
        return std::memcmp(&av, &bv, sizeof(float)) == 0;
    }
    return false;
}

Value *ConstPropagation::try_fold(Instruction *inst) {
    auto op = inst->get_instr_type();
    if (inst->isBinary() || inst->is_cmp() || inst->is_fcmp()) {
        auto lhs = inst->get_operand(0), rhs = inst->get_operand(1);
        if (auto a = cast_constantint(lhs)) {
            if (auto b = cast_constantint(rhs)) return folder.compute(op, a, b);
        }
        if (auto a = cast_constantfp(lhs)) {
            if (auto b = cast_constantfp(rhs)) return folder.compute(op, a, b);
        }
    } else if (inst->is_si2fp()) {
        if (auto value = cast_constantint(inst->get_operand(0))) {
            // The folder implements signed i32, not signed i1 (true is -1).
            if (value->get_type()->is_int32_type()) return folder.compute(op, value);
        }
    } else if (inst->is_fp2si() && inst->get_type()->is_int32_type()) {
        if (auto value = cast_constantfp(inst->get_operand(0))) return folder.compute(op, value);
    } else if (inst->is_zext()) {
        if (auto value = cast_constantint(inst->get_operand(0)))
            return ConstantInt::get(value->get_value(), m_);
    } else if (inst->is_phi() && inst->get_num_operand() != 0) {
        auto candidate = inst->get_operand(0);
        if (candidate == inst) return nullptr;
        for (unsigned i = 2; i < inst->get_num_operand(); i += 2)
            if (!same_value(candidate, inst->get_operand(i))) return nullptr;
        return candidate;
    }
    return nullptr;
}

static void remove_phi_inputs(BasicBlock *bb, const std::unordered_set<BasicBlock *> &predecessors) {
    for (auto inst : bb->get_instructions()) {
        if (!inst->is_phi()) continue;
        for (int i = static_cast<int>(inst->get_num_operand()) - 1; i >= 1; i -= 2) {
            if (predecessors.count(static_cast<BasicBlock *>(inst->get_operand(i)))) {
                inst->remove_operand(i);
                inst->remove_operand(i - 1);
            }
        }
    }
}

bool ConstPropagation::simplify_branch(BasicBlock *bb) {
    auto branch = dynamic_cast<BranchInst *>(bb->get_terminator());
    if (!branch || !branch->is_cond_br()) return false;
    auto condition = cast_constantint(branch->get_condition());
    if (!condition) return false;
    auto keep = static_cast<BasicBlock *>(branch->get_operand(condition->get_value() ? 1 : 2));
    auto drop = static_cast<BasicBlock *>(branch->get_operand(condition->get_value() ? 2 : 1));
    if (keep != drop) remove_phi_inputs(drop, {bb});
    // Destruction removes CFG edges; construction adds them. This order matters.
    bb->erase_instr(branch);
    BranchInst::create_br(keep, bb);
    return true;
}

bool ConstPropagation::remove_unreachable_blocks(Function *func) {
    std::unordered_set<BasicBlock *> reachable;
    std::vector<BasicBlock *> work{func->get_entry_block()};
    while (!work.empty()) {
        auto bb = work.back();
        work.pop_back();
        if (!reachable.insert(bb).second) continue;
        for (auto succ : bb->get_succ_basic_blocks()) work.push_back(succ);
    }
    std::unordered_set<BasicBlock *> dead;
    for (auto bb : func->get_basic_blocks())
        if (!reachable.count(bb)) dead.insert(bb);
    if (dead.empty()) return false;
    for (auto bb : reachable) remove_phi_inputs(bb, dead);
    // Break every dead branch while all its source/target blocks still exist.
    // This also handles mutually unreachable cycles without dangling CFG links.
    for (auto bb : dead)
        if (auto branch = dynamic_cast<BranchInst *>(bb->get_terminator())) bb->erase_instr(branch);
    for (auto bb : dead) bb->erase_from_parent();
    for (auto bb : dead) delete bb;
    return true;
}

void ConstPropagation::run() {
    for (auto func : m_->get_functions()) {
        if (func->is_declaration()) continue;
        bool changed;
        do {
            changed = false;
            for (auto bb : func->get_basic_blocks()) {
                std::vector<Instruction *> wait_delete;
                // Facts apply only after a store in this block, never at entry.
                std::unordered_map<GlobalVariable *, Constant *> globalvar_def;
                for (auto inst : bb->get_instructions()) {
                    Value *replacement = try_fold(inst);
                    if (inst->is_store()) {
                        auto global = dynamic_cast<GlobalVariable *>(inst->get_operand(1));
                        if (!global) globalvar_def.clear(); // possible alias
                        else if (auto value = dynamic_cast<Constant *>(inst->get_operand(0)))
                            globalvar_def[global] = value;
                        else globalvar_def.erase(global);
                    } else if (inst->is_call()) {
                        globalvar_def.clear(); // unknown memory effects
                    } else if (inst->is_load()) {
                        auto global = dynamic_cast<GlobalVariable *>(inst->get_operand(0));
                        auto known = globalvar_def.find(global);
                        if (known != globalvar_def.end()) replacement = known->second;
                    }
                    if (replacement) {
                        inst->replace_all_use_with(replacement);
                        wait_delete.push_back(inst);
                        changed = true;
                    }
                }
                for (auto inst : wait_delete) bb->erase_instr(inst);
            }
            for (auto bb : func->get_basic_blocks()) changed |= simplify_branch(bb);
            changed |= remove_unreachable_blocks(func);
        } while (changed);
    }
}
