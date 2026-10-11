#include "MachineLowering.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>
namespace machine {
namespace {
using O = MachineOperand;
bool same(const O &a, const O &b) {
    return a.kind == b.kind && ((a.kind == OperandKind::VReg && a.id == b.id) ||
                                (a.kind == OperandKind::PReg && a.reg == b.reg));
}
ValueKind operand_type(const MachineFunction &f, const O &o) {
    if (o.kind == OperandKind::VReg) {
        if (o.id >= f.vreg_types.size())
            throw std::runtime_error("MIR invalid virtual register in lowering");
        return f.vreg_types[o.id];
    }
    if (o.kind == OperandKind::PReg && o.reg.cls != RegClass::FCC)
        return o.reg.cls == RegClass::FPR ? ValueKind::F32 : ValueKind::Ptr64;
    throw std::runtime_error("MIR lowering expected scalar register");
}
O as_use(O o) {
    o.role = RegRole::Use;
    return o;
}
PReg abi_register(ValueKind kind, unsigned &g, unsigned &f) {
    if (kind == ValueKind::F32) {
        if (f >= 8)
            throw std::runtime_error("MIR ABI supports at most eight float arguments");
        return fpr(f++);
    }
    if (g >= 8)
        throw std::runtime_error("MIR ABI supports at most eight integer/pointer arguments");
    return gpr(4 + g++);
}
} // namespace
std::vector<MachineInstr> parallel_copies(MachineFunction &f, std::vector<std::pair<O, O>> copies) {
    std::vector<MachineInstr> out;
    for (auto &c : copies) {
        if (!c.first.is_reg() || !c.second.is_reg())
            throw std::runtime_error("MIR parallel copy requires registers");
        if (reg_class(operand_type(f, c.first)) != reg_class(operand_type(f, c.second)))
            throw std::runtime_error("MIR parallel copy class mismatch");
        c.first.role = RegRole::Def;
        c.second.role = RegRole::Use;
    }
    copies.erase(std::remove_if(copies.begin(), copies.end(),
                                [](const auto &c) { return same(c.first, c.second); }),
                 copies.end());
    while (!copies.empty()) {
        auto safe = std::find_if(copies.begin(), copies.end(), [&](const auto &c) {
            return std::none_of(copies.begin(), copies.end(),
                                [&](const auto &other) { return same(c.first, other.second); });
        });
        if (safe != copies.end()) {
            out.push_back({Opcode::COPY, {safe->first, safe->second}});
            copies.erase(safe);
            continue;
        }
        O old = copies.front().first;
        auto temp = f.new_vreg(operand_type(f, old));
        out.push_back({Opcode::COPY, {O::vreg(temp, RegRole::Def), as_use(old)}});
        for (auto &c : copies)
            if (same(c.second, old))
                c.second = O::vreg(temp);
    }
    return out;
}
void lower(MachineModule &m, const Options &) {
    for (auto &f : m.functions) {
        std::map<std::pair<BlockId, BlockId>, std::vector<std::pair<O, O>>> edges;
        for (auto &b : f.blocks) {
            for (auto &i : b.instructions)
                if (i.opcode == Opcode::PHI) {
                    if (i.operands.size() < 3 || i.operands.size() % 2 != 1)
                        throw std::runtime_error("MIR malformed PHI in lowering");
                    for (unsigned k = 1; k < i.operands.size(); k += 2)
                        edges[{i.operands[k + 1].id, b.id}].push_back(
                            {i.operands[0], i.operands[k]});
                }
            b.instructions.erase(
                std::remove_if(b.instructions.begin(), b.instructions.end(),
                               [](const MachineInstr &i) { return i.opcode == Opcode::PHI; }),
                b.instructions.end());
        }
        BlockId next = 0;
        for (auto &b : f.blocks)
            next = std::max(next, b.id + 1);
        for (auto &e : edges) {
            auto pred = e.first.first, succ = e.first.second;
            auto copies = parallel_copies(f, std::move(e.second));
            if (copies.empty())
                continue;
            auto &b = f.block(pred);
            if (b.successors.size() > 1) {
                BlockId edge = next++;
                for (auto &i : b.instructions)
                    if (descriptor(i.opcode).terminator)
                        for (auto &o : i.operands)
                            if (o.kind == OperandKind::Block && o.id == succ)
                                o.id = edge;
                copies.push_back({Opcode::B, {O::block(succ)}});
                f.blocks.push_back({edge,
                                    "phi_edge_" + std::to_string(pred) + "_" + std::to_string(succ),
                                    std::move(copies),
                                    {},
                                    {}});
            } else {
                auto where = std::find_if(
                    b.instructions.begin(), b.instructions.end(),
                    [](const MachineInstr &i) { return descriptor(i.opcode).terminator; });
                b.instructions.insert(where, copies.begin(), copies.end());
            }
        }
        for (auto &b : f.blocks) {
            std::vector<MachineInstr> out;
            for (auto &i : b.instructions) {
                if (i.opcode == Opcode::ARG) {
                    unsigned g = 0, fp = 0;
                    auto index = i.operands.at(1).imm;
                    if (index < 0 || uint64_t(index) >= f.parameter_types.size())
                        throw std::runtime_error("MIR invalid ARG index");
                    PReg reg;
                    for (unsigned k = 0; k <= unsigned(index); ++k)
                        reg = abi_register(f.parameter_types[k], g, fp);
                    if (f.parameter_types[index] == ValueKind::I1) {
                        auto raw = f.new_vreg(ValueKind::I32);
                        out.push_back(
                            {Opcode::COPY, {O::vreg(raw, RegRole::Def), O::preg(reg)}, i.source});
                        out.push_back({Opcode::ANDI,
                                       {i.operands[0], O::vreg(raw), O::immediate(1)},
                                       i.source});
                    } else
                        out.push_back({Opcode::COPY, {i.operands[0], O::preg(reg)}, i.source});
                } else if (i.opcode == Opcode::HIGH_CALL) {
                    unsigned k = 0;
                    bool result = i.operands.at(0).is_def();
                    O dst;
                    if (result)
                        dst = i.operands[k++];
                    O symbol = i.operands.at(k++);
                    if (symbol.kind != OperandKind::Symbol)
                        throw std::runtime_error("MIR call lacks direct symbol");
                    std::vector<O> call = {symbol};
                    std::vector<std::pair<O, O>> copies;
                    unsigned g = 0, fp = 0;
                    for (; k < i.operands.size(); ++k) {
                        auto src = i.operands[k];
                        auto reg = abi_register(operand_type(f, src), g, fp);
                        copies.push_back({O::preg(reg, RegRole::Def), src});
                        call.push_back(O::preg(reg));
                    }
                    auto moves = parallel_copies(f, std::move(copies));
                    out.insert(out.end(), moves.begin(), moves.end());
                    PReg ret;
                    if (result) {
                        ret = operand_type(f, dst) == ValueKind::F32 ? fpr(0) : gpr(4);
                        call.push_back(O::preg(ret, RegRole::Def));
                    }
                    out.push_back({Opcode::CALL, std::move(call), i.source});
                    if (result && operand_type(f, dst) == ValueKind::I1) {
                        auto raw = f.new_vreg(ValueKind::I32);
                        out.push_back(
                            {Opcode::COPY, {O::vreg(raw, RegRole::Def), O::preg(ret)}, i.source});
                        out.push_back(
                            {Opcode::ANDI, {dst, O::vreg(raw), O::immediate(1)}, i.source});
                    } else if (result)
                        out.push_back({Opcode::COPY, {dst, O::preg(ret)}, i.source});
                } else if (i.opcode == Opcode::RETURN) {
                    std::vector<O> uses;
                    if (!i.operands.empty()) {
                        auto src = i.operands[0];
                        auto reg = operand_type(f, src) == ValueKind::F32 ? fpr(0) : gpr(4);
                        out.push_back({Opcode::COPY, {O::preg(reg, RegRole::Def), src}, i.source});
                        uses.push_back(O::preg(reg));
                    } else if (f.name == "main") {
                        out.push_back({Opcode::COPY,
                                       {O::preg(gpr(4), RegRole::Def), O::preg(gpr(0))},
                                       i.source});
                        uses.push_back(O::preg(gpr(4)));
                    }
                    out.push_back({Opcode::RET, std::move(uses), i.source});
                } else
                    out.push_back(i);
            }
            b.instructions = std::move(out);
        }
        rebuild_cfg(f);
    }
}
} // namespace machine
