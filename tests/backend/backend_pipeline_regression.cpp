#include "MachineIR.hpp"
#include "StackAllocator.hpp"
#include "LoongArchFrame.hpp"
#include <stdexcept>
namespace backend_regression {
using namespace machine;
using O = MachineOperand;
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static MachineFunction fixture() {
    MachineFunction f;
    f.name = "stack_test";
    f.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32};
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(10)}},
                  {Opcode::LOAD_CONSTANT, {O::vreg(1, RegRole::Def), O::immediate(20)}},
                  {Opcode::ADD_W, {O::vreg(2, RegRole::Def), O::vreg(0), O::vreg(1)}},
                  {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(2)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}}};
    f.return_type = ValueKind::I32;
    return f;
}
void stack() {
    auto f = fixture();
    allocate_stack(f);
    auto &ins = f.blocks[0].instructions;
    for (size_t i = 0; i < ins.size(); ++i)
        if (ins[i].opcode == Opcode::ADD_W) {
            require(i >= 2 && i + 1 < ins.size(), "arithmetic lacks spills");
            require(ins[i - 1].opcode == Opcode::LD_W && ins[i - 2].opcode == Opcode::LD_W,
                    "both sources must reload before arithmetic");
            require(ins[i + 1].opcode == Opcode::ST_W, "definition must be spilled");
            require(ins[i].operands[1].reg != ins[i].operands[2].reg,
                    "distinct sources share scratch");
        }
    require(f.frame_objects.size() == 3, "one home per VReg");
    auto duplicate = fixture();
    duplicate.blocks[0].instructions[2].operands[2] = O::vreg(0);
    allocate_stack(duplicate);
    size_t reloads = 0;
    for (const auto &i : duplicate.blocks[0].instructions)
        if (i.opcode == Opcode::LD_W)
            ++reloads;
    require(reloads == 2, "duplicate uses should share reload (plus ABI result reload)");
    auto typed = fixture();
    typed.vreg_types.push_back(ValueKind::I1);
    typed.vreg_types.push_back(ValueKind::Ptr64);
    typed.frame_objects.push_back({ValueKind::I1, 1, 1, FramePurpose::Alloca});
    auto &body = typed.blocks[0].instructions;
    body.insert(body.end() - 1,
                {{Opcode::LOAD_CONSTANT, {O::vreg(3, RegRole::Def), O::immediate(1)}},
                 {Opcode::LOAD_CONSTANT, {O::vreg(4, RegRole::Def), O::immediate(0)}}});
    allocate_stack(typed);
    require(typed.frame_objects[0].size == 1, "user I1 object widened");
    require(typed.frame_objects[4].size == 4, "I1 home is not four bytes");
    require(typed.frame_objects[5].size == 8, "pointer home is not eight bytes");
    auto unused = fixture();
    unused.vreg_types.push_back(ValueKind::I32);
    allocate_stack(unused);
    require(unused.frame_objects.size() == 3, "covered unused VReg still receives home slot");
    MachineFunction rmw;
    rmw.name = "partial_constant";
    rmw.return_type = ValueKind::Ptr64;
    rmw.vreg_types = {ValueKind::Ptr64};
    rmw.blocks = {{0,
                   "entry",
                   {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(0x12345678)}},
                    {Opcode::LU32I_D, {O::vreg(0, RegRole::UseDef), O::immediate(0x12345)}},
                    {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(0)}},
                    {Opcode::RET, {O::preg(gpr(4))}}},
                   {},
                   {}}};
    allocate_stack(rmw);
    const auto &sequence = rmw.blocks[0].instructions;
    bool checked = false;
    for (size_t i = 1; i + 1 < sequence.size(); ++i)
        if (sequence[i].opcode == Opcode::LU32I_D) {
            require(sequence[i - 1].opcode == Opcode::LD_D &&
                        sequence[i + 1].opcode == Opcode::ST_D,
                    "partial constant update must reload and spill its destination");
            require(sequence[i - 1].operands[0].reg == sequence[i].operands[0].reg,
                    "partial update did not reuse reloaded destination");
            checked = true;
        }
    require(checked, "partial constant update disappeared");
}
void frame() {
    MachineFunction void_main;
    void_main.name = "main";
    void_main.blocks = {{0,
                         "entry",
                         {{Opcode::LOAD_CONSTANT, {O::preg(gpr(4), RegRole::Def), O::immediate(0)}},
                          {Opcode::RET, {O::preg(gpr(4))}}},
                         {},
                         {}}};
    lower_frame(void_main);
    require(void_main.blocks.back().instructions.back().operands.size() == 1,
            "void main epilogue lost process exit register use");
    auto wide = fixture();
    wide.vreg_types[0] = ValueKind::Ptr64;
    wide.blocks[0].instructions[0].operands[1] = O::immediate(INT64_MAX);
    wide.blocks[0].instructions.erase(wide.blocks[0].instructions.begin() + 1,
                                      wide.blocks[0].instructions.end() - 1);
    wide.blocks[0].instructions.back().operands.clear();
    wide.return_type.reset();
    allocate_stack(wide);
    lower_frame(wide);
    bool partial = false;
    for (const auto &i : wide.blocks[0].instructions)
        if (i.opcode == Opcode::LU32I_D) {
            require(i.operands[0].is_use() && i.operands[0].is_def(),
                    "wide constant omitted destination read");
            partial = true;
        }
    require(partial, "wide constant test never exercised LU32I_D");
    verify(MachineModule{{wide}, {}}, VerifyStage::Final);
    for (unsigned total : {16u, 2048u, 2064u}) {
        MachineFunction exact;
        exact.name = "boundary";
        exact.blocks = {{0, "entry", {{Opcode::RET, {}}}, {}, {}}};
        if (total > 16)
            exact.frame_objects.push_back({ValueKind::I32, total - 16, 16, FramePurpose::Alloca});
        lower_frame(exact);
        require(exact.frame_size == total, "boundary fixture has wrong frame size");
        verify(MachineModule{{exact}, {}}, VerifyStage::Final);
        bool negative_boundary = false, positive_boundary = false;
        for (const auto &i : exact.blocks[0].instructions)
            if (i.opcode == Opcode::ADDI_D) {
                negative_boundary |= i.operands[2].imm == -2048;
                positive_boundary |= i.operands[2].imm == 2048;
            }
        if (total == 2048)
            require(negative_boundary && !positive_boundary,
                    "asymmetric addi frame boundary broken");
    }
    for (unsigned bytes : {0u, 16u, 2032u, 2048u, 4096u}) {
        auto f = fixture();
        if (bytes)
            f.frame_objects.push_back({ValueKind::I32, bytes, 16, FramePurpose::Alloca});
        allocate_stack(f);
        lower_frame(f);
        require(f.frame_size % 16 == 0, "frame not aligned");
        require(f.blocks.front().instructions.front().opcode == Opcode::ADDI_D ||
                    f.blocks.front().instructions.front().opcode == Opcode::LU12I_W,
                "stack adjustment does not precede saved registers");
        MachineModule m{{f}, {}};
        verify(m, VerifyStage::Final);
        for (const auto &b : f.blocks)
            for (const auto &i : b.instructions) {
                for (const auto &o : i.operands)
                    require(o.kind != OperandKind::FrameIndex && o.kind != OperandKind::VReg,
                            "Final contains unresolved operand");
                if (i.opcode == Opcode::ADDI_D)
                    require(i.operands.back().imm >= -2048 && i.operands.back().imm <= 2047,
                            "frame uses out-of-range addi");
            }
    }
}
void printer() {
    auto f = fixture();
    allocate_stack(f);
    lower_frame(f);
    auto other = f;
    other.name = "other";
    MachineModule m{{f, other},
                    {{"nonzero", 4, 4, false, {0x78, 0x56, 0x34, 0x12}},
                     {"zeros", 8, 8, false, {0, 0, 0, 0, 0, 0, 0, 0}}}};
    auto text = print_assembly(m);
    require(text.find(".text") != std::string::npos, "missing text section");
    require(text.find(".data") != std::string::npos && text.find(".bss") != std::string::npos,
            "global sections missing");
    require(text.find("120, 86, 52, 18") != std::string::npos, "initializer bits lost");
    require(text.find(".Lmir_stack_test_0") != std::string::npos &&
                text.find(".Lmir_other_0") != std::string::npos,
            "block labels not function scoped");
}
} // namespace backend_regression
