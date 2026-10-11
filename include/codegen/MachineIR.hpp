#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
class Value;
namespace machine {
enum class SelectionMode { Baseline, Greedy };
enum class VerifyStage { Selected, PreRA, Final };
struct Options {
    SelectionMode selection = SelectionMode::Baseline;
    bool lab4_metrics = false;
};
enum class ValueKind { I1, I32, Ptr64, F32 };
enum class RegClass { GPR, FPR, FCC };
using VReg = unsigned;
using BlockId = unsigned;
struct PReg {
    RegClass cls = RegClass::GPR;
    unsigned index = 0;
    bool operator==(const PReg &r) const { return cls == r.cls && index == r.index; }
    bool operator!=(const PReg &r) const { return !(*this == r); }
    bool operator<(const PReg &r) const { return cls != r.cls ? cls < r.cls : index < r.index; }
};
inline PReg gpr(unsigned n) { return {RegClass::GPR, n}; }
inline PReg fpr(unsigned n) { return {RegClass::FPR, n}; }
inline PReg fcc(unsigned n = 0) { return {RegClass::FCC, n}; }
enum class OperandKind { VReg, PReg, Immediate, Block, Symbol, FrameIndex };
enum class RegRole { None, Use, Def, UseDef };
inline bool is_use(RegRole role) { return role == RegRole::Use || role == RegRole::UseDef; }
inline bool is_def(RegRole role) { return role == RegRole::Def || role == RegRole::UseDef; }
struct MachineOperand {
    OperandKind kind = OperandKind::Immediate;
    RegRole role = RegRole::None;
    unsigned id = 0;
    PReg reg;
    int64_t imm = 0;
    std::string symbol;
    static MachineOperand vreg(VReg id, RegRole role = RegRole::Use);
    static MachineOperand preg(PReg reg, RegRole role = RegRole::Use);
    static MachineOperand immediate(int64_t value);
    static MachineOperand block(BlockId id);
    static MachineOperand sym(std::string name);
    static MachineOperand frame(unsigned id);
    bool is_reg() const { return kind == OperandKind::VReg || kind == OperandKind::PReg; }
    bool is_use() const { return is_reg() && machine::is_use(role); }
    bool is_def() const { return is_reg() && machine::is_def(role); }
};
enum class Opcode {
    ARG,
    PHI,
    HIGH_CALL,
    RETURN,
    COPY,
    LOAD_CONSTANT,
    SYMBOL_ADDR,
    FRAME_ADDR,
    ADD_W,
    SUB_W,
    MUL_W,
    DIV_W,
    ADD_D,
    MUL_D,
    ADDI_W,
    ADDI_D,
    ANDI,
    ORI,
    XORI,
    SLT,
    SLTU,
    SLTUI,
    XOR,
    OR,
    LU12I_W,
    LU32I_D,
    LU52I_D,
    FADD_S,
    FSUB_S,
    FMUL_S,
    FDIV_S,
    FMOV_S,
    MOVGR2FR_W,
    MOVFR2GR_S,
    FFINT_S_W,
    FTINTRZ_W_S,
    FCMP_SULT_S,
    FCMP_SULE_S,
    FCMP_SUEQ_S,
    FCMP_SUNE_S,
    MOVCF2GR,
    LD_B_U,
    LD_W,
    LD_D,
    FLD_S,
    ST_B,
    ST_W,
    ST_D,
    FST_S,
    B,
    BNEZ,
    BEQ,
    BNE,
    BLT,
    BGE,
    CALL,
    RET,
    LA_LOCAL
};
struct MachineInstr {
    Opcode opcode;
    std::vector<MachineOperand> operands;
    const Value *source = nullptr;
};
// Fixed constraints are shared by selection, verification, and later allocators.
// PHI pairs and CALL signature-dependent operands receive additional verification.
enum class OperandForm {
    Register,
    GPR,
    FPR,
    FCC,
    Immediate,
    Signed12,
    Unsigned12,
    Signed20,
    Block,
    Symbol,
    FrameIndex,
    Address
};
struct OperandConstraint {
    OperandForm form;
    RegRole role = RegRole::None;
};
struct MachineInstrDesc {
    const char *name;
    unsigned min_operands;
    unsigned max_operands;
    bool may_load = false, may_store = false, call = false, terminator = false, pseudo = false;
    std::vector<PReg> implicit_uses, implicit_defs, clobbers;
    std::string mnemonic;
    std::vector<OperandConstraint> operands;
};
const MachineInstrDesc &descriptor(Opcode opcode);
RegClass reg_class(ValueKind kind);
std::string register_name(PReg reg);
const std::vector<PReg> &reserved_scratch();
const std::vector<PReg> &caller_saved();
enum class FramePurpose { Alloca, Spill, SavedRegister };
struct FrameObject {
    ValueKind kind = ValueKind::I32;
    uint64_t size = 4;
    unsigned alignment = 4;
    FramePurpose purpose = FramePurpose::Alloca;
    int64_t offset = 0;
};
struct MachineBasicBlock {
    BlockId id;
    std::string name;
    std::vector<MachineInstr> instructions;
    std::vector<BlockId> predecessors, successors;
};
struct MachineFunction {
    std::string name;
    std::vector<MachineBasicBlock> blocks;
    std::vector<ValueKind> vreg_types;
    std::vector<FrameObject> frame_objects;
    std::vector<ValueKind> parameter_types;
    std::optional<ValueKind> return_type;
    uint64_t frame_size = 0;
    VReg new_vreg(ValueKind type) {
        vreg_types.push_back(type);
        return vreg_types.size() - 1;
    }
    MachineBasicBlock &block(BlockId id);
    const MachineBasicBlock &block(BlockId id) const;
};
struct MachineGlobal {
    std::string name;
    uint64_t size = 0;
    unsigned alignment = 1;
    bool is_constant = false;
    std::vector<uint8_t> initializer;
};
struct MachineModule {
    std::vector<MachineFunction> functions;
    std::vector<MachineGlobal> globals;
};
void rebuild_cfg(MachineFunction &function);
void verify(const MachineModule &module, VerifyStage stage);
std::string dump(const MachineModule &module);
} // namespace machine
