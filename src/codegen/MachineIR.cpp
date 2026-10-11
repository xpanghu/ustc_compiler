#include "MachineIR.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
namespace machine {
MachineOperand MachineOperand::vreg(VReg n, RegRole r) {
    MachineOperand o;
    o.kind = OperandKind::VReg;
    o.id = n;
    o.role = r;
    return o;
}
MachineOperand MachineOperand::preg(PReg p, RegRole r) {
    MachineOperand o;
    o.kind = OperandKind::PReg;
    o.reg = p;
    o.role = r;
    return o;
}
MachineOperand MachineOperand::immediate(int64_t n) {
    MachineOperand o;
    o.imm = n;
    return o;
}
MachineOperand MachineOperand::block(BlockId n) {
    MachineOperand o;
    o.kind = OperandKind::Block;
    o.id = n;
    return o;
}
MachineOperand MachineOperand::frame(unsigned n) {
    MachineOperand o;
    o.kind = OperandKind::FrameIndex;
    o.id = n;
    return o;
}
MachineOperand MachineOperand::sym(std::string n) {
    MachineOperand o;
    o.kind = OperandKind::Symbol;
    o.symbol = std::move(n);
    return o;
}
RegClass reg_class(ValueKind k) { return k == ValueKind::F32 ? RegClass::FPR : RegClass::GPR; }
const std::vector<PReg> &reserved_scratch() {
    static const std::vector<PReg> r = {gpr(12), gpr(13), gpr(14), gpr(20),
                                        fpr(8),  fpr(9),  fpr(10)};
    return r;
}
const std::vector<PReg> &caller_saved() {
    static const auto r = [] {
        std::vector<PReg> v = {gpr(1)};
        for (unsigned i = 4; i <= 20; ++i)
            v.push_back(gpr(i));
        for (unsigned i = 0; i < 24; ++i)
            v.push_back(fpr(i));
        for (unsigned i = 0; i < 8; ++i)
            v.push_back(fcc(i));
        return v;
    }();
    return r;
}
std::string register_name(PReg p) {
    if (p.cls == RegClass::FCC)
        return "$fcc" + std::to_string(p.index);
    if (p.cls == RegClass::FPR)
        return "$f" + std::to_string(p.index);
    return "$r" + std::to_string(p.index);
}
const MachineInstrDesc &descriptor(Opcode op) {
    static const std::map<Opcode, MachineInstrDesc> table = [] {
        std::map<Opcode, MachineInstrDesc> t;
        auto add = [&](Opcode o, const char *n, unsigned lo, unsigned hi, bool l = false,
                       bool s = false, bool c = false, bool term = false, bool p = false) {
            t.emplace(o, MachineInstrDesc{n, lo, hi, l, s, c, term, p, {}, {}, {}});
        };
#define D(O, N) add(Opcode::O, #O, N, N)
#define P(O, L, H) add(Opcode::O, #O, L, H, false, false, false, false, true)
        P(ARG, 2, 2);
        P(PHI, 3, ~0u);
        P(HIGH_CALL, 1, ~0u);
        P(COPY, 2, 2);
        P(LOAD_CONSTANT, 2, 2);
        P(SYMBOL_ADDR, 2, 2);
        P(FRAME_ADDR, 2, 2);
        add(Opcode::RETURN, "RETURN", 0, 1, false, false, false, true, true);
        D(ADD_W, 3);
        D(SUB_W, 3);
        D(MUL_W, 3);
        D(DIV_W, 3);
        D(ADD_D, 3);
        D(MUL_D, 3);
        D(ADDI_W, 3);
        D(ADDI_D, 3);
        D(ANDI, 3);
        D(ORI, 3);
        D(XORI, 3);
        D(SLT, 3);
        D(SLTU, 3);
        D(SLTUI, 3);
        D(XOR, 3);
        D(OR, 3);
        D(LU12I_W, 2);
        D(LU32I_D, 2);
        D(LU52I_D, 3);
        D(FADD_S, 3);
        D(FSUB_S, 3);
        D(FMUL_S, 3);
        D(FDIV_S, 3);
        D(FMOV_S, 2);
        D(MOVGR2FR_W, 2);
        D(MOVFR2GR_S, 2);
        D(FFINT_S_W, 2);
        D(FTINTRZ_W_S, 2);
        D(FCMP_SULT_S, 3);
        D(FCMP_SULE_S, 3);
        D(FCMP_SUEQ_S, 3);
        D(FCMP_SUNE_S, 3);
        D(MOVCF2GR, 2);
        D(LA_LOCAL, 2);
#define M(O, L, S) add(Opcode::O, #O, 3, 3, L, S)
        M(LD_B_U, true, false);
        M(LD_W, true, false);
        M(LD_D, true, false);
        M(FLD_S, true, false);
        M(ST_B, false, true);
        M(ST_W, false, true);
        M(ST_D, false, true);
        M(FST_S, false, true);
#define B(O, N) add(Opcode::O, #O, N, N, false, false, false, true)
        B(B, 1);
        B(BNEZ, 2);
        B(BEQ, 3);
        B(BNE, 3);
        B(BLT, 3);
        B(BGE, 3);
        add(Opcode::CALL, "CALL", 1, ~0u, true, true, true);
        t.at(Opcode::CALL).implicit_defs = {gpr(1)};
        t.at(Opcode::CALL).clobbers = caller_saved();
        add(Opcode::RET, "RET", 0, 1, false, false, false, true);
        t.at(Opcode::RET).implicit_uses = {gpr(1)};
#undef D
#undef P
#undef M
#undef B
        for (auto &entry : t) {
            auto &d = entry.second;
            d.mnemonic = d.name;
            for (auto &c : d.mnemonic)
                c = c == '_' ? '.' : char(std::tolower(static_cast<unsigned char>(c)));
        }
        t.at(Opcode::LD_B_U).mnemonic = "ld.bu";
        t.at(Opcode::CALL).mnemonic = "bl";
        using F = OperandForm;
        using C = OperandConstraint;
        const C gd{F::GPR, RegRole::Def}, gu{F::GPR, RegRole::Use};
        const C fd{F::FPR, RegRole::Def}, fu{F::FPR, RegRole::Use};
        const C rd{F::Register, RegRole::Def}, ru{F::Register, RegRole::Use};
        for (auto &entry : t) {
            auto o = entry.first;
            auto &d = entry.second;
            if (d.max_operands == ~0u || o == Opcode::RETURN || o == Opcode::RET)
                continue;
            d.operands.assign(d.max_operands, gu);
            if (!d.operands.empty())
                d.operands[0] = gd;
        }
        for (auto o : {Opcode::ARG})
            t.at(o).operands = {rd, {F::Immediate}};
        t.at(Opcode::PHI).operands = {rd};
        t.at(Opcode::COPY).operands = {rd, ru};
        t.at(Opcode::LOAD_CONSTANT).operands = {gd, {F::Immediate}};
        t.at(Opcode::FRAME_ADDR).operands = {gd, {F::FrameIndex}};
        for (auto o : {Opcode::SYMBOL_ADDR, Opcode::LA_LOCAL})
            t.at(o).operands = {gd, {F::Symbol}};
        for (auto o : {Opcode::ADDI_W, Opcode::ADDI_D, Opcode::SLTUI, Opcode::LU52I_D})
            t.at(o).operands[2] = {F::Signed12};
        for (auto o : {Opcode::ANDI, Opcode::ORI, Opcode::XORI})
            t.at(o).operands[2] = {F::Unsigned12};
        t.at(Opcode::LU12I_W).operands = {gd, {F::Signed20}};
        t.at(Opcode::LU32I_D).operands = {{F::GPR, RegRole::UseDef}, {F::Signed20}};
        for (auto o : {Opcode::FADD_S, Opcode::FSUB_S, Opcode::FMUL_S, Opcode::FDIV_S})
            t.at(o).operands = {fd, fu, fu};
        for (auto o : {Opcode::FMOV_S, Opcode::FFINT_S_W, Opcode::FTINTRZ_W_S})
            t.at(o).operands = {fd, fu};
        for (auto o :
             {Opcode::FCMP_SULT_S, Opcode::FCMP_SULE_S, Opcode::FCMP_SUEQ_S, Opcode::FCMP_SUNE_S})
            t.at(o).operands = {{F::FCC, RegRole::Def}, fu, fu};
        t.at(Opcode::MOVCF2GR).operands = {gd, {F::FCC, RegRole::Use}};
        t.at(Opcode::MOVGR2FR_W).operands = {fd, gu};
        t.at(Opcode::MOVFR2GR_S).operands = {gd, fu};
        for (auto o : {Opcode::LD_B_U, Opcode::LD_W, Opcode::LD_D, Opcode::FLD_S, Opcode::ST_B,
                       Opcode::ST_W, Opcode::ST_D, Opcode::FST_S}) {
            auto &d = t.at(o);
            d.operands = {o == Opcode::FLD_S   ? fd
                          : o == Opcode::FST_S ? fu
                          : d.may_load         ? gd
                                               : gu,
                          {F::Address, RegRole::Use},
                          {F::Signed12}};
        }
        t.at(Opcode::B).operands = {{F::Block}};
        t.at(Opcode::BNEZ).operands = {gu, {F::Block}};
        for (auto o : {Opcode::BEQ, Opcode::BNE, Opcode::BLT, Opcode::BGE})
            t.at(o).operands = {gu, gu, {F::Block}};
        t.at(Opcode::CALL).operands = {{F::Symbol}};
        return t;
    }();
    auto it = table.find(op);
    if (it == table.end())
        throw std::runtime_error("unknown Machine IR opcode");
    return it->second;
}
MachineBasicBlock &MachineFunction::block(BlockId id) {
    for (auto &b : blocks)
        if (b.id == id)
            return b;
    throw std::runtime_error("missing machine block " + std::to_string(id));
}
const MachineBasicBlock &MachineFunction::block(BlockId id) const {
    for (auto &b : blocks)
        if (b.id == id)
            return b;
    throw std::runtime_error("missing machine block " + std::to_string(id));
}
void rebuild_cfg(MachineFunction &f) {
    for (auto &b : f.blocks) {
        b.predecessors.clear();
        b.successors.clear();
    }
    for (auto &b : f.blocks)
        for (auto &i : b.instructions)
            if (descriptor(i.opcode).terminator)
                for (auto &o : i.operands)
                    if (o.kind == OperandKind::Block) {
                        if (std::find(b.successors.begin(), b.successors.end(), o.id) ==
                            b.successors.end())
                            b.successors.push_back(o.id);
                        auto &p = f.block(o.id).predecessors;
                        if (std::find(p.begin(), p.end(), b.id) == p.end())
                            p.push_back(b.id);
                    }
}
static bool is_branch(Opcode o) {
    return o == Opcode::B || o == Opcode::BNEZ || o == Opcode::BEQ || o == Opcode::BNE ||
           o == Opcode::BLT || o == Opcode::BGE;
}
static std::set<BlockId> block_set(const std::vector<BlockId> &v) { return {v.begin(), v.end()}; }
void verify(const MachineModule &m, VerifyStage stage) {
    for (const auto &f : m.functions) {
        auto fail = [&](BlockId b, size_t n, const std::string &s) {
            throw std::runtime_error("MIR " + f.name + " bb" + std::to_string(b) + " instruction " +
                                     std::to_string(n) + ": " + s);
        };
        if (f.blocks.empty())
            throw std::runtime_error("MIR " + f.name + ": no entry block");
        std::set<BlockId> ids;
        for (auto &b : f.blocks)
            if (!ids.insert(b.id).second)
                fail(b.id, 0, "duplicate block id");
        std::map<VReg, std::pair<BlockId, size_t>> defs;
        for (const auto &b : f.blocks) {
            std::set<BlockId> succ;
            bool termination = false;
            if (b.instructions.empty())
                fail(b.id, 0, "empty block");
            for (size_t n = 0; n < b.instructions.size(); ++n) {
                const auto &i = b.instructions[n];
                const auto &d = descriptor(i.opcode);
                auto bad = [&](const std::string &s) { fail(b.id, n, s); };
                if (i.operands.size() < d.min_operands || i.operands.size() > d.max_operands)
                    bad("wrong operand count for " + std::string(d.name));
                if (stage == VerifyStage::Final && d.pseudo)
                    bad("unexpanded pseudo " + std::string(d.name));
                if (stage == VerifyStage::PreRA &&
                    (i.opcode == Opcode::PHI || i.opcode == Opcode::ARG ||
                     i.opcode == Opcode::HIGH_CALL || i.opcode == Opcode::RETURN))
                    bad("high-level pseudo after lowering");
                if (termination &&
                    !(is_branch(i.opcode) && is_branch(b.instructions[n - 1].opcode) &&
                      b.instructions[n - 1].opcode != Opcode::B))
                    bad("instruction after terminator");
                termination = d.terminator;
                auto cls = [&](const MachineOperand &o) {
                    if (o.kind == OperandKind::VReg) {
                        if (o.id >= f.vreg_types.size())
                            bad("unknown VReg");
                        return reg_class(f.vreg_types[o.id]);
                    }
                    return o.reg.cls;
                };
                for (const auto &o : i.operands) {
                    if (o.kind == OperandKind::VReg) {
                        if (stage == VerifyStage::Final)
                            bad("virtual register in Final");
                        if (o.id >= f.vreg_types.size())
                            bad("unknown VReg");
                        if (o.is_def() && stage == VerifyStage::Selected &&
                            !defs.emplace(o.id, std::make_pair(b.id, n)).second)
                            bad("multiple SSA definitions");
                    }
                    if (o.is_reg() && o.role == RegRole::None)
                        bad("register lacks use/def");
                    if (o.kind == OperandKind::PReg &&
                        o.reg.index >= (o.reg.cls == RegClass::FCC ? 8u : 32u))
                        bad("physical register out of range");
                    if (o.kind == OperandKind::FrameIndex &&
                        (stage == VerifyStage::Final || o.id >= f.frame_objects.size()))
                        bad("invalid or residual FrameIndex");
                    if (o.kind == OperandKind::Block) {
                        if (!ids.count(o.id))
                            bad("unknown BlockId");
                        if (d.terminator)
                            succ.insert(o.id);
                    }
                }
                auto reg = [&](unsigned k, RegRole role, RegClass c) {
                    auto &o = i.operands.at(k);
                    if (!o.is_reg() || o.role != role || cls(o) != c)
                        bad("wrong register class/role at operand " + std::to_string(k));
                };
                auto imm = [&](unsigned k, int64_t lo, int64_t hi) {
                    auto &o = i.operands.at(k);
                    if (o.kind != OperandKind::Immediate || o.imm < lo || o.imm > hi)
                        bad("immediate out of range");
                };
                auto kind = [&](unsigned k, OperandKind t) {
                    if (i.operands.at(k).kind != t)
                        bad("wrong operand kind");
                };
                for (unsigned k = 0; k < d.operands.size(); ++k) {
                    const auto &constraint = d.operands[k];
                    const auto &operand = i.operands.at(k);
                    using F = OperandForm;
                    switch (constraint.form) {
                    case F::Register:
                        if (!operand.is_reg() || operand.role != constraint.role)
                            bad("expected register with declared role");
                        break;
                    case F::GPR:
                        reg(k, constraint.role, RegClass::GPR);
                        break;
                    case F::FPR:
                        reg(k, constraint.role, RegClass::FPR);
                        break;
                    case F::FCC:
                        reg(k, constraint.role, RegClass::FCC);
                        break;
                    case F::Immediate:
                        kind(k, OperandKind::Immediate);
                        break;
                    case F::Signed12:
                        imm(k, -2048, 2047);
                        break;
                    case F::Unsigned12:
                        imm(k, 0, 4095);
                        break;
                    case F::Signed20:
                        imm(k, -524288, 524287);
                        break;
                    case F::Block:
                        kind(k, OperandKind::Block);
                        break;
                    case F::Symbol:
                        kind(k, OperandKind::Symbol);
                        break;
                    case F::FrameIndex:
                        kind(k, OperandKind::FrameIndex);
                        break;
                    case F::Address:
                        if (operand.kind != OperandKind::FrameIndex)
                            reg(k, RegRole::Use, RegClass::GPR);
                        break;
                    }
                }
                auto o = i.opcode;
                auto typed = [&](unsigned k, ValueKind expected) {
                    const auto &operand = i.operands.at(k);
                    if (operand.kind == OperandKind::VReg &&
                        f.vreg_types.at(operand.id) != expected)
                        bad("wrong virtual register value type at operand " + std::to_string(k));
                };
                if (o == Opcode::ADD_W || o == Opcode::SUB_W || o == Opcode::MUL_W ||
                    o == Opcode::DIV_W || o == Opcode::ADDI_W) {
                    typed(0, ValueKind::I32);
                    for (unsigned k = 1; k < i.operands.size(); ++k)
                        typed(k, ValueKind::I32);
                }
                if (o == Opcode::ADD_D || o == Opcode::ADDI_D || o == Opcode::MUL_D ||
                    o == Opcode::LU32I_D)
                    typed(0, ValueKind::Ptr64);
                if (o == Opcode::FRAME_ADDR || o == Opcode::SYMBOL_ADDR || o == Opcode::LA_LOCAL)
                    typed(0, ValueKind::Ptr64);
                if (o == Opcode::LOAD_CONSTANT && i.operands[0].kind == OperandKind::VReg &&
                    i.operands[1].kind == OperandKind::Immediate) {
                    auto type = f.vreg_types[i.operands[0].id];
                    auto value = i.operands[1].imm;
                    if (type == ValueKind::I1 && (value < 0 || value > 1))
                        bad("non-canonical i1 constant");
                    if (type == ValueKind::I32 && (value < INT32_MIN || value > INT32_MAX))
                        bad("i32 constant is not sign-extended");
                }
                if (o == Opcode::RETURN && !i.operands.empty() && f.return_type)
                    typed(0, *f.return_type);
                if (o == Opcode::COPY && i.operands[0].kind == OperandKind::VReg &&
                    i.operands[1].kind == OperandKind::VReg) {
                    auto dst = f.vreg_types[i.operands[0].id], src = f.vreg_types[i.operands[1].id];
                    if (dst != src && !(dst == ValueKind::I32 && src == ValueKind::I1))
                        bad("COPY changes value type without a conversion");
                }
                if (o == Opcode::LD_W)
                    typed(0, ValueKind::I32);
                if (o == Opcode::LD_B_U)
                    typed(0, ValueKind::I32);
                if (o == Opcode::LD_D)
                    typed(0, ValueKind::Ptr64);
                if (o == Opcode::FLD_S)
                    typed(0, ValueKind::F32);
                if (is_branch(o) && o != Opcode::B &&
                    (n + 1 >= b.instructions.size() || b.instructions[n + 1].opcode != Opcode::B))
                    bad("conditional branch lacks explicit unconditional false branch");

                if (o == Opcode::PHI) {
                    reg(0, RegRole::Def, cls(i.operands[0]));
                    if (i.operands.size() % 2 != 1)
                        bad("PHI missing value/edge pair");
                    std::set<BlockId> incoming;
                    for (unsigned k = 1; k < i.operands.size(); k += 2) {
                        reg(k, RegRole::Use, cls(i.operands[0]));
                        kind(k + 1, OperandKind::Block);
                        if (!incoming.insert(i.operands[k + 1].id).second)
                            bad("duplicate PHI edge");
                        if (i.operands[0].kind == OperandKind::VReg &&
                            i.operands[k].kind == OperandKind::VReg &&
                            f.vreg_types[i.operands[0].id] != f.vreg_types[i.operands[k].id])
                            bad("PHI type mismatch");
                    }
                    if (incoming != block_set(b.predecessors))
                        bad("PHI incoming edges mismatch");
                } else if (o == Opcode::ARG) {
                    reg(0, RegRole::Def, cls(i.operands[0]));
                    imm(1, 0,
                        f.parameter_types.empty()
                            ? -1
                            : static_cast<int64_t>(f.parameter_types.size() - 1));
                    if (i.operands[0].kind != OperandKind::VReg ||
                        f.vreg_types[i.operands[0].id] != f.parameter_types[i.operands[1].imm])
                        bad("ARG type mismatch");
                } else if (o == Opcode::COPY) {
                    reg(0, RegRole::Def, cls(i.operands[0]));
                    reg(1, RegRole::Use, cls(i.operands[0]));
                } else if (o == Opcode::LOAD_CONSTANT) {
                    reg(0, RegRole::Def, RegClass::GPR);
                    kind(1, OperandKind::Immediate);
                } else if (o == Opcode::SYMBOL_ADDR || o == Opcode::LA_LOCAL ||
                           o == Opcode::FRAME_ADDR) {
                    reg(0, RegRole::Def, RegClass::GPR);
                    kind(1,
                         o == Opcode::FRAME_ADDR ? OperandKind::FrameIndex : OperandKind::Symbol);
                } else if (o == Opcode::HIGH_CALL || o == Opcode::CALL) {
                    unsigned k = 0;
                    if (o == Opcode::HIGH_CALL && i.operands[0].is_def()) {
                        if (i.operands[0].is_use())
                            bad("high call result must be a pure definition");
                        ++k;
                    }
                    kind(k++, OperandKind::Symbol);
                    unsigned integer_args = 0, float_args = 0, returns = 0;
                    for (; k < i.operands.size(); ++k) {
                        const auto &operand = i.operands[k];
                        if (!operand.is_reg())
                            bad("call operand is not register");
                        if (o == Opcode::CALL && operand.kind != OperandKind::PReg)
                            bad("CALL requires fixed registers");
                        if (operand.is_use() && operand.is_def())
                            bad("call operand must have separate use and def roles");
                        if (operand.is_def()) {
                            if (o != Opcode::CALL || ++returns > 1 || k + 1 != i.operands.size() ||
                                (operand.reg != gpr(4) && operand.reg != fpr(0)))
                                bad("invalid fixed call result register");
                        } else if (operand.is_use()) {
                            auto c = cls(operand);
                            if (c == RegClass::FCC)
                                bad("FCC cannot be a call argument");
                            unsigned index = c == RegClass::FPR ? float_args++ : integer_args++;
                            if (index >= 8)
                                bad("unsupported ABI argument count");
                            if (o == Opcode::CALL &&
                                operand.reg != (c == RegClass::FPR ? fpr(index) : gpr(4 + index)))
                                bad("invalid fixed call argument register");
                        } else
                            bad("call operand lacks use/def");
                    }
                } else if (o == Opcode::RETURN || o == Opcode::RET) {
                    if (!i.operands.empty()) {
                        if (!i.operands[0].is_use() || i.operands[0].is_def())
                            bad("return requires register use");
                    }
                    if (o == Opcode::RETURN && bool(f.return_type) != !i.operands.empty())
                        bad("return arity mismatch");
                }
            }
            if (!termination)
                fail(b.id, b.instructions.size(), "missing terminator");
            if (succ != block_set(b.successors))
                fail(b.id, 0, "CFG successor mismatch");
            for (auto p : b.predecessors)
                if (!ids.count(p) || !block_set(f.block(p).successors).count(b.id))
                    fail(b.id, 0, "CFG predecessor mismatch");
            for (auto s : succ)
                if (!block_set(f.block(s).predecessors).count(b.id))
                    fail(b.id, 0, "CFG reverse edge mismatch");
        }
        if (stage == VerifyStage::Final) {
            std::map<BlockId, int64_t> offsets;
            int64_t position = 0;
            for (const auto &block : f.blocks) {
                offsets[block.id] = position;
                for (const auto &instruction : block.instructions)
                    position += instruction.opcode == Opcode::LA_LOCAL ? 8 : 4;
            }
            position = 0;
            for (const auto &block : f.blocks)
                for (size_t n = 0; n < block.instructions.size(); ++n) {
                    const auto &instruction = block.instructions[n];
                    if (is_branch(instruction.opcode)) {
                        auto target = instruction.operands.back().id;
                        int64_t displacement = offsets.at(target) - position;
                        unsigned bits = instruction.opcode == Opcode::B      ? 28
                                        : instruction.opcode == Opcode::BNEZ ? 23
                                                                             : 18;
                        int64_t limit = int64_t{1} << (bits - 1);
                        if (displacement < -limit || displacement > limit - 4)
                            fail(block.id, n,
                                 "branch target exceeds instruction encoding range; branch "
                                 "relaxation is unsupported");
                    }
                    position += instruction.opcode == Opcode::LA_LOCAL ? 8 : 4;
                }
        }
        std::set<BlockId> reachable;
        std::vector<BlockId> todo = {f.blocks.front().id};
        while (!todo.empty()) {
            auto b = todo.back();
            todo.pop_back();
            if (!reachable.insert(b).second)
                continue;
            for (auto s : f.block(b).successors)
                todo.push_back(s);
        }
        if (stage == VerifyStage::Selected) {
            std::map<BlockId, std::set<BlockId>> dom;
            for (auto b : reachable)
                dom[b] = b == f.blocks.front().id ? std::set<BlockId>{b} : reachable;
            bool change = true;
            while (change) {
                change = false;
                for (auto b : reachable) {
                    if (b == f.blocks.front().id)
                        continue;
                    auto next = reachable;
                    for (auto p : f.block(b).predecessors)
                        if (reachable.count(p)) {
                            std::set<BlockId> x;
                            std::set_intersection(next.begin(), next.end(), dom[p].begin(),
                                                  dom[p].end(), std::inserter(x, x.begin()));
                            next = std::move(x);
                        }
                    next.insert(b);
                    if (next != dom[b]) {
                        dom[b] = next;
                        change = true;
                    }
                }
            }
            for (auto &b : f.blocks)
                for (size_t n = 0; n < b.instructions.size(); ++n) {
                    auto &i = b.instructions[n];
                    for (unsigned k = 0; k < i.operands.size(); ++k) {
                        auto &o = i.operands[k];
                        if (o.kind != OperandKind::VReg || !o.is_use())
                            continue;
                        auto d = defs.find(o.id);
                        if (d == defs.end())
                            fail(b.id, n, "undefined VReg");
                        auto use_block = i.opcode == Opcode::PHI ? i.operands[k + 1].id : b.id;
                        if (reachable.count(use_block) &&
                            (!dom[use_block].count(d->second.first) ||
                             (d->second.first == use_block && i.opcode != Opcode::PHI &&
                              d->second.second >= n)))
                            fail(b.id, n, "SSA definition does not dominate use");
                    }
                }
        }
        if (stage == VerifyStage::PreRA) {
            using Set = std::set<uint64_t>;
            auto key = [](const MachineOperand &o) {
                return o.kind == OperandKind::VReg
                           ? uint64_t(o.id)
                           : (uint64_t(1) << 32) + unsigned(o.reg.cls) * 32 + o.reg.index;
            };
            auto pk = [&](PReg r) { return key(MachineOperand::preg(r)); };
            Set initial = {pk(gpr(0)), pk(gpr(1)), pk(gpr(3)), pk(gpr(22))};
            unsigned gi = 0, fi = 0;
            for (auto t : f.parameter_types)
                initial.insert(pk(t == ValueKind::F32 ? fpr(fi++) : gpr(4 + gi++)));
            Set universe = initial;
            for (auto &b : f.blocks)
                for (auto &i : b.instructions)
                    for (auto &o : i.operands)
                        if (o.is_reg())
                            universe.insert(key(o));
            auto transfer = [&](const MachineBasicBlock &b, Set state, bool check) {
                for (size_t n = 0; n < b.instructions.size(); ++n) {
                    const auto &i = b.instructions[n];
                    for (auto &o : i.operands)
                        if (o.is_use() && check && !state.count(key(o)))
                            fail(b.id, n,
                                 "register use is not defined on every incoming path (or "
                                 "call-clobbered)");
                    for (auto r : descriptor(i.opcode).implicit_uses)
                        if (check && !state.count(pk(r)))
                            fail(b.id, n, "implicit register use is undefined");
                    for (auto r : descriptor(i.opcode).clobbers)
                        state.erase(pk(r));
                    for (auto &o : i.operands)
                        if (o.is_def())
                            state.insert(key(o));
                    for (auto r : descriptor(i.opcode).implicit_defs)
                        state.insert(pk(r));
                }
                return state;
            };
            std::map<BlockId, Set> in, out;
            for (auto b : reachable) {
                in[b] = universe;
                out[b] = universe;
            }
            bool changed = true;
            while (changed) {
                changed = false;
                for (auto &b : f.blocks) {
                    if (!reachable.count(b.id))
                        continue;
                    Set next = b.id == f.blocks.front().id ? initial : universe;
                    if (b.id != f.blocks.front().id)
                        for (auto p : b.predecessors)
                            if (reachable.count(p)) {
                                Set x;
                                std::set_intersection(next.begin(), next.end(), out[p].begin(),
                                                      out[p].end(), std::inserter(x, x.begin()));
                                next = std::move(x);
                            }
                    auto end = transfer(b, next, false);
                    if (next != in[b.id] || end != out[b.id]) {
                        in[b.id] = std::move(next);
                        out[b.id] = std::move(end);
                        changed = true;
                    }
                }
            }
            for (auto &b : f.blocks)
                if (reachable.count(b.id))
                    transfer(b, in[b.id], true);
        }
    }
}
std::string dump(const MachineModule &m) {
    std::ostringstream s;
    const char *types[] = {"i1", "i32", "ptr64", "f32"};
    for (auto &g : m.globals)
        s << "global @" << g.name << " size=" << g.size << " align=" << g.alignment << "\n";
    for (auto &f : m.functions) {
        s << "function @" << f.name << " frame=" << f.frame_size << "\n";
        for (unsigned v = 0; v < f.vreg_types.size(); ++v)
            s << "  %v" << v << ": " << types[unsigned(f.vreg_types[v])] << "\n";
        for (unsigned j = 0; j < f.frame_objects.size(); ++j)
            s << "  fi" << j << " size=" << f.frame_objects[j].size
              << " offset=" << f.frame_objects[j].offset << "\n";
        for (auto &b : f.blocks) {
            s << "bb" << b.id << " (" << b.name << "):\n";
            for (auto &i : b.instructions) {
                s << "  " << descriptor(i.opcode).name;
                for (auto &o : i.operands) {
                    s << " ";
                    if (o.is_use() && o.is_def())
                        s << "use-def:";
                    else if (o.is_def())
                        s << "def:";
                    switch (o.kind) {
                    case OperandKind::VReg:
                        s << "%v" << o.id;
                        break;
                    case OperandKind::PReg:
                        s << register_name(o.reg);
                        break;
                    case OperandKind::Immediate:
                        s << o.imm;
                        break;
                    case OperandKind::Block:
                        s << "bb" << o.id;
                        break;
                    case OperandKind::Symbol:
                        s << "@" << o.symbol;
                        break;
                    case OperandKind::FrameIndex:
                        s << "fi" << o.id;
                        break;
                    }
                }
                s << "\n";
            }
        }
    }
    return s.str();
}
} // namespace machine
