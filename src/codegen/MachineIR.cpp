#include "MachineIR.hpp"
#include <algorithm>
#include <cctype>
#include <map>
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
const std::vector<PReg> &allocatable_registers(RegClass cls) {
    static const auto gprs = [] {
        std::vector<PReg> regs;
        for (unsigned i = 15; i <= 19; ++i)
            regs.push_back(gpr(i));
        for (unsigned i = 4; i <= 11; ++i)
            regs.push_back(gpr(i));
        for (unsigned i = 23; i <= 31; ++i)
            regs.push_back(gpr(i));
        return regs;
    }();
    static const auto fprs = [] {
        std::vector<PReg> regs;
        for (unsigned i = 11; i <= 23; ++i)
            regs.push_back(fpr(i));
        for (unsigned i = 0; i <= 7; ++i)
            regs.push_back(fpr(i));
        for (unsigned i = 24; i <= 31; ++i)
            regs.push_back(fpr(i));
        return regs;
    }();
    static const std::vector<PReg> empty;
    return cls == RegClass::GPR ? gprs : cls == RegClass::FPR ? fprs : empty;
}
bool is_callee_saved(PReg reg) {
    return reg.index <= 31 && ((reg.cls == RegClass::GPR && reg.index >= 23) ||
                               (reg.cls == RegClass::FPR && reg.index >= 24));
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
        auto add = [&](Opcode o, const char *n, bool l = false, bool s = false,
                       bool c = false, bool term = false) {
            t.emplace(o, MachineInstrDesc{n, l, s, c, term, {}, {}, {}});
        };
#define D(O) add(Opcode::O, #O)
        D(ARG);
        D(PHI);
        D(HIGH_CALL);
        D(COPY);
        D(LOAD_CONSTANT);
        D(SYMBOL_ADDR);
        D(FRAME_ADDR);
        add(Opcode::RETURN, "RETURN", false, false, false, true);
        D(ADD_W);
        D(SUB_W);
        D(MUL_W);
        D(DIV_W);
        D(ADD_D);
        D(MUL_D);
        D(ADDI_W);
        D(ADDI_D);
        D(ANDI);
        D(ORI);
        D(XORI);
        D(SLT);
        D(SLTU);
        D(SLTUI);
        D(XOR);
        D(OR);
        D(LU12I_W);
        D(LU32I_D);
        D(LU52I_D);
        D(FADD_S);
        D(FSUB_S);
        D(FMUL_S);
        D(FDIV_S);
        D(FMOV_S);
        D(MOVGR2FR_W);
        D(MOVFR2GR_S);
        D(FFINT_S_W);
        D(FTINTRZ_W_S);
        D(FCMP_SULT_S);
        D(FCMP_SULE_S);
        D(FCMP_SUEQ_S);
        D(FCMP_SUNE_S);
        D(MOVCF2GR);
        D(LA_LOCAL);
#define M(O, L, S) add(Opcode::O, #O, L, S)
        M(LD_B_U, true, false);
        M(LD_W, true, false);
        M(LD_D, true, false);
        M(FLD_S, true, false);
        M(FLD_D, true, false);
        M(ST_B, false, true);
        M(ST_W, false, true);
        M(ST_D, false, true);
        M(FST_S, false, true);
        M(FST_D, false, true);
#define B(O) add(Opcode::O, #O, false, false, false, true)
        B(B);
        B(BNEZ);
        B(BEQ);
        B(BNE);
        B(BLT);
        B(BGE);
        add(Opcode::CALL, "CALL", true, true, true);
        t.at(Opcode::CALL).implicit_defs = {gpr(1)};
        t.at(Opcode::CALL).clobbers = caller_saved();
        add(Opcode::RET, "RET", false, false, false, true);
        t.at(Opcode::RET).implicit_uses = {gpr(1)};
#undef D
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
