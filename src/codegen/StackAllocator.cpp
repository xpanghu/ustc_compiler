#include "StackAllocator.hpp"
#include <map>
#include <stdexcept>

namespace machine {
namespace {
using O = MachineOperand;
Opcode load_op(ValueKind kind) {
    return kind == ValueKind::F32     ? Opcode::FLD_S
           : kind == ValueKind::Ptr64 ? Opcode::LD_D
                                      : Opcode::LD_W;
}
Opcode store_op(ValueKind kind) {
    return kind == ValueKind::F32     ? Opcode::FST_S
           : kind == ValueKind::Ptr64 ? Opcode::ST_D
                                      : Opcode::ST_W;
}
PReg scratch(ValueKind kind, unsigned index) {
    if (index >= 3)
        throw std::runtime_error("stack allocator: instruction exceeds reserved scratch registers");
    return kind == ValueKind::F32 ? fpr(8 + index) : gpr(12 + index);
}
} // namespace

void allocate_stack(MachineFunction &function) {
    std::vector<bool> retained(function.vreg_types.size(), false);
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instructions)
            for (const auto &operand : instruction.operands)
                if (operand.kind == OperandKind::VReg)
                    retained.at(operand.id) = true;
    std::vector<unsigned> homes(function.vreg_types.size());
    for (VReg value = 0; value < function.vreg_types.size(); ++value) {
        if (!retained[value])
            continue;
        auto kind = function.vreg_types[value];
        homes[value] = function.frame_objects.size();
        unsigned size = kind == ValueKind::Ptr64 ? 8 : 4;
        function.frame_objects.push_back({kind, size, size, FramePurpose::Spill});
    }
    for (auto &block : function.blocks) {
        std::vector<MachineInstr> output;
        auto reload = [&](VReg value, PReg reg, const Value *source) {
            output.push_back(
                {load_op(function.vreg_types.at(value)),
                 {O::preg(reg, RegRole::Def), O::frame(homes.at(value)), O::immediate(0)},
                 source});
        };
        auto spill = [&](VReg value, PReg reg, const Value *source) {
            output.push_back({store_op(function.vreg_types.at(value)),
                              {O::preg(reg), O::frame(homes.at(value)), O::immediate(0)},
                              source});
        };
        for (auto instruction : block.instructions) {
            if (instruction.opcode == Opcode::COPY) {
                auto dst = instruction.operands.at(0), src = instruction.operands.at(1);
                if (dst.kind == OperandKind::VReg && src.kind == OperandKind::VReg) {
                    auto reg = scratch(function.vreg_types.at(src.id), 0);
                    reload(src.id, reg, instruction.source);
                    spill(dst.id, reg, instruction.source);
                } else if (dst.kind == OperandKind::PReg && src.kind == OperandKind::VReg) {
                    reload(src.id, dst.reg, instruction.source);
                } else if (dst.kind == OperandKind::VReg && src.kind == OperandKind::PReg) {
                    spill(dst.id, src.reg, instruction.source);
                } else {
                    output.push_back(std::move(instruction));
                }
                continue;
            }
            std::map<VReg, PReg> uses;
            unsigned gpr_count = 0, fpr_count = 0;
            for (const auto &operand : instruction.operands) {
                if (operand.kind != OperandKind::VReg || !operand.is_use() ||
                    uses.count(operand.id))
                    continue;
                auto kind = function.vreg_types.at(operand.id);
                auto reg = scratch(kind, kind == ValueKind::F32 ? fpr_count++ : gpr_count++);
                uses.emplace(operand.id, reg);
                reload(operand.id, reg, instruction.source);
            }
            std::vector<std::pair<VReg, PReg>> definitions;
            unsigned gpr_defs = 0, fpr_defs = 0;
            for (auto &operand : instruction.operands) {
                if (operand.kind != OperandKind::VReg)
                    continue;
                auto value = operand.id;
                PReg reg;
                if (operand.is_use())
                    reg = uses.at(value);
                else {
                    auto kind = function.vreg_types.at(value);
                    reg = scratch(kind, kind == ValueKind::F32 ? fpr_defs++ : gpr_defs++);
                }
                if (operand.is_def())
                    definitions.emplace_back(value, reg);
                operand = O::preg(reg, operand.role);
            }
            output.push_back(std::move(instruction));
            for (const auto &definition : definitions)
                spill(definition.first, definition.second, output.back().source);
        }
        block.instructions = std::move(output);
    }
}
} // namespace machine
