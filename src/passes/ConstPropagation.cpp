#include "ConstPropagation.hpp"

#include "Instruction.hpp"
#include "logging.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

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

void ConstPropagation::run() {
    for (auto *func : m_->get_functions()) {

        for (auto *bb : func->get_basic_blocks()) {
            wait_delete.clear();

            for (auto *instr : bb->get_instructions()) {
                // clear glbalvar_def map

                if (instr->is_add() || instr->is_sub() || instr->is_mul() || instr->is_div()) {
                    auto value1 = cast_constantint(instr->get_operand(0));
                    auto value2 = cast_constantint(instr->get_operand(1));
                    if (value1 && value2) {
                        if (auto fold_const = folder.compute(instr->get_instr_type(), value1, value2)) {
                            instr->replace_all_use_with(fold_const);
                            wait_delete.push_back(instr);
                        }
                    }
                }
                // TODO: fold other type of expression
            }
            globalvar_def.clear();
            for (auto instr : wait_delete) {
                bb->erase_instr(instr);
            }
        }
    }

    for (auto *func : m_->get_functions()) {
        for (auto *bb : func->get_basic_blocks()) {
            builder.set_insert_point(bb);
            // TODO: check if conditional branch's condition is constant
        }
        for (auto bb : delete_bb) {
            // delete unuseful basic block
            // func->remove(bb);
            clear_blocks_recs(bb);
        }
        delete_bb.clear();
    }
}

bool ConstPropagation::is_entry(BasicBlock *bb) {
    // TODO
    throw std::runtime_error("Lab5 ConstPropagation: is_entry TODO is not implemented");
}

void ConstPropagation::clear_blocks_recs(BasicBlock *start_bb) {
    // TODO: adapt unreachable-block/PHI cleanup after implementing branch folding.
    // The old intrusive-list helper cannot safely delete current pointer-list IR.
    // Use erase_instr for discarded instructions; remove_instr only detaches them.
    throw std::runtime_error("Lab5 ConstPropagation: CFG cleanup TODO is not implemented");
}
