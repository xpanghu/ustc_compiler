#include "LoongArchFrame.hpp"
#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace machine {
namespace {
using O = MachineOperand;
bool signed12(int64_t value) { return value >= -2048 && value <= 2047; }
int64_t sign_bits(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}
void constant(std::vector<MachineInstr> &output, PReg dst, int64_t value,
              const Value *source = nullptr) {
    if (signed12(value)) {
        output.push_back({Opcode::ADDI_D,
                          {O::preg(dst, RegRole::Def), O::preg(gpr(0)), O::immediate(value)},
                          source});
        return;
    }
    auto bits = static_cast<uint64_t>(value);
    output.push_back(
        {Opcode::LU12I_W,
         {O::preg(dst, RegRole::Def), O::immediate(sign_bits((bits >> 12) & 0xfffff, 20))},
         source});
    output.push_back({Opcode::ORI,
                      {O::preg(dst, RegRole::Def), O::preg(dst), O::immediate(bits & 0xfff)},
                      source});
    if (value != static_cast<int64_t>(static_cast<int32_t>(bits))) {
        output.push_back(
            {Opcode::LU32I_D,
             {O::preg(dst, RegRole::UseDef), O::immediate(sign_bits((bits >> 32) & 0xfffff, 20))},
             source});
        output.push_back({Opcode::LU52I_D,
                          {O::preg(dst, RegRole::Def), O::preg(dst),
                           O::immediate(sign_bits((bits >> 52) & 0xfff, 12))},
                          source});
    }
}
void address(std::vector<MachineInstr> &out, PReg dst, PReg base, int64_t offset,
             const Value *source = nullptr) {
    if (signed12(offset))
        out.push_back({Opcode::ADDI_D,
                       {O::preg(dst, RegRole::Def), O::preg(base), O::immediate(offset)},
                       source});
    else {
        constant(out, gpr(20), offset, source);
        out.push_back(
            {Opcode::ADD_D, {O::preg(dst, RegRole::Def), O::preg(base), O::preg(gpr(20))}, source});
    }
}
void memory(std::vector<MachineInstr> &out, MachineInstr instruction) {
    if (!signed12(instruction.operands.at(2).imm)) {
        address(out, gpr(20), instruction.operands.at(1).reg, instruction.operands.at(2).imm,
                instruction.source);
        instruction.operands[1] = O::preg(gpr(20));
        instruction.operands[2] = O::immediate(0);
    }
    out.push_back(std::move(instruction));
}
std::string label(const MachineFunction &function, BlockId block) {
    return ".Lmir_" + function.name + "_" + std::to_string(block);
}
} // namespace

void lower_frame(MachineFunction &function) {
    uint64_t used = 16; // Saved ra and old fp, addressed relative to incoming sp.
    for (auto &object : function.frame_objects) {
        if (!object.alignment || (object.alignment & (object.alignment - 1)))
            throw std::runtime_error("frame object has invalid alignment");
        if (object.size >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - used - object.alignment)
            throw std::runtime_error("frame size exceeds representable address range");
        used = (used + object.size + object.alignment - 1) & ~uint64_t(object.alignment - 1);
        object.offset = -static_cast<int64_t>(used);
    }
    function.frame_size = (used + 15) & ~uint64_t(15);
    function.frame_objects.push_back({ValueKind::Ptr64, 8, 8, FramePurpose::SavedRegister, -8});
    function.frame_objects.push_back({ValueKind::Ptr64, 8, 8, FramePurpose::SavedRegister, -16});
    BlockId exit = 0;
    for (const auto &block : function.blocks)
        exit = std::max(exit, block.id + 1);
    for (auto &block : function.blocks) {
        std::vector<MachineInstr> output;
        for (auto instruction : block.instructions) {
            if (instruction.opcode == Opcode::RET) {
                output.push_back({Opcode::B, {O::block(exit)}, instruction.source});
            } else if (instruction.opcode == Opcode::COPY) {
                auto dst = instruction.operands.at(0), src = instruction.operands.at(1);
                if (dst.reg == src.reg)
                    continue;
                if (dst.reg.cls == RegClass::FPR)
                    output.push_back({Opcode::FMOV_S, {dst, src}, instruction.source});
                else
                    output.push_back({Opcode::OR, {dst, src, O::preg(gpr(0))}, instruction.source});
            } else if (instruction.opcode == Opcode::LOAD_CONSTANT) {
                constant(output, instruction.operands.at(0).reg, instruction.operands.at(1).imm,
                         instruction.source);
            } else if (instruction.opcode == Opcode::SYMBOL_ADDR) {
                instruction.opcode = Opcode::LA_LOCAL;
                output.push_back(std::move(instruction));
            } else if (instruction.opcode == Opcode::FRAME_ADDR) {
                auto &object = function.frame_objects.at(instruction.operands.at(1).id);
                address(output, instruction.operands.at(0).reg, gpr(22), object.offset,
                        instruction.source);
            } else if (descriptor(instruction.opcode).may_load ||
                       descriptor(instruction.opcode).may_store) {
                // CALL also carries conservative memory effects but has no address operands.
                if (instruction.opcode == Opcode::CALL) {
                    output.push_back(std::move(instruction));
                    continue;
                }
                auto &base = instruction.operands.at(1);
                if (base.kind == OperandKind::FrameIndex) {
                    auto offset = function.frame_objects.at(base.id).offset;
                    auto extra = instruction.operands.at(2).imm;
                    if ((extra < 0 && offset < std::numeric_limits<int64_t>::min() - extra) ||
                        (extra > 0 && offset > std::numeric_limits<int64_t>::max() - extra))
                        throw std::runtime_error("frame address offset overflow");
                    instruction.operands[2] = O::immediate(offset + extra);
                    base = O::preg(gpr(22));
                }
                memory(output, std::move(instruction));
            } else
                output.push_back(std::move(instruction));
        }
        block.instructions = std::move(output);
    }
    std::vector<MachineInstr> prologue;
    address(prologue, gpr(3), gpr(3), -static_cast<int64_t>(function.frame_size));
    memory(prologue, {Opcode::ST_D,
                      {O::preg(gpr(1)), O::preg(gpr(3)), O::immediate(function.frame_size - 8)}});
    memory(prologue, {Opcode::ST_D,
                      {O::preg(gpr(22)), O::preg(gpr(3)), O::immediate(function.frame_size - 16)}});
    address(prologue, gpr(22), gpr(3), static_cast<int64_t>(function.frame_size));
    auto &entry = function.blocks.front().instructions;
    entry.insert(entry.begin(), prologue.begin(), prologue.end());
    std::vector<O> return_operands;
    if (function.return_type)
        return_operands.push_back(
            O::preg(*function.return_type == ValueKind::F32 ? fpr(0) : gpr(4)));
    else if (function.name == "main")
        return_operands.push_back(O::preg(gpr(4)));
    function.blocks.push_back(
        {exit,
         "epilogue",
         {{Opcode::LD_D, {O::preg(gpr(1), RegRole::Def), O::preg(gpr(22)), O::immediate(-8)}},
          {Opcode::LD_D, {O::preg(gpr(20), RegRole::Def), O::preg(gpr(22)), O::immediate(-16)}},
          {Opcode::OR, {O::preg(gpr(3), RegRole::Def), O::preg(gpr(22)), O::preg(gpr(0))}},
          {Opcode::OR, {O::preg(gpr(22), RegRole::Def), O::preg(gpr(20)), O::preg(gpr(0))}},
          {Opcode::RET, return_operands}},
         {},
         {}});
    rebuild_cfg(function);
}

std::string print_assembly(const MachineModule &module) {
    verify(module, VerifyStage::Final);
    std::ostringstream out;
    for (const auto &global : module.globals) {
        bool zero = std::all_of(global.initializer.begin(), global.initializer.end(),
                                [](uint8_t b) { return b == 0; });
        out << (zero ? "\t.bss\n" : "\t.data\n") << "\t.globl " << global.name << "\n\t.type "
            << global.name << ", @object\n\t.size " << global.name << ", " << global.size
            << "\n\t.balign " << global.alignment << '\n'
            << global.name << ":\n";
        if (zero)
            out << "\t.space " << global.size << '\n';
        else {
            for (size_t i = 0; i < global.initializer.size(); i += 16) {
                out << "\t.byte ";
                for (size_t j = i; j < std::min(i + 16, global.initializer.size()); ++j) {
                    if (j != i)
                        out << ", ";
                    out << unsigned(global.initializer[j]);
                }
                out << '\n';
            }
            if (global.size > global.initializer.size())
                out << "\t.space " << global.size - global.initializer.size() << '\n';
        }
    }
    out << "\t.text\n";
    for (const auto &function : module.functions) {
        out << "\t.globl " << function.name << "\n\t.type " << function.name << ", @function\n"
            << function.name << ":\n";
        for (const auto &block : function.blocks) {
            out << label(function, block.id) << ":\n";
            for (const auto &instruction : block.instructions) {
                if (instruction.opcode == Opcode::CALL) {
                    out << "\tbl " << instruction.operands.at(0).symbol << '\n';
                    continue;
                }
                if (instruction.opcode == Opcode::RET) {
                    out << "\tjirl $zero, $ra, 0\n";
                    continue;
                }
                out << '\t' << descriptor(instruction.opcode).mnemonic;
                for (size_t index = 0; index < instruction.operands.size(); ++index) {
                    out << (index ? ", " : " ");
                    const auto &operand = instruction.operands[index];
                    switch (operand.kind) {
                    case OperandKind::PReg:
                        out << register_name(operand.reg);
                        break;
                    case OperandKind::Immediate:
                        out << operand.imm;
                        break;
                    case OperandKind::Block:
                        out << label(function, operand.id);
                        break;
                    case OperandKind::Symbol:
                        out << operand.symbol;
                        break;
                    default:
                        throw std::runtime_error("assembly printer received unresolved operand");
                    }
                }
                out << '\n';
            }
        }
        out << "\t.size " << function.name << ", .-" << function.name << '\n';
    }
    return out.str();
}
} // namespace machine
