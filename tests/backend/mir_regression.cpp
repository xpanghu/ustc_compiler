#include "MachineIR.hpp"
#include "LoongArchISel.hpp"
#include "MachineLowering.hpp"
#include "MachineCodeGen.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Module.hpp"
#include "Instruction.hpp"
#include "Constant.hpp"
#include <limits>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace machine;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void rejected(const std::function<void()> &fn) {
    bool failed = false;
    try {
        fn();
    } catch (const std::runtime_error &) {
        failed = true;
    }
    require(failed, "expected verifier rejection");
}
// Constant factories cache by Module address; keep independent fixtures alive.
static Module &new_test_module() {
    static std::vector<std::unique_ptr<Module>> modules;
    modules.push_back(std::make_unique<Module>());
    return *modules.back();
}
static MachineModule sample() {
    MachineModule m;
    MachineFunction f;
    f.name = "add";
    f.parameter_types = {ValueKind::I32, ValueKind::I32};
    f.return_type = ValueKind::I32;
    auto a = f.new_vreg(ValueKind::I32), b = f.new_vreg(ValueKind::I32),
         c = f.new_vreg(ValueKind::I32);
    f.blocks.push_back(
        {0,
         "entry",
         {{Opcode::ARG, {MachineOperand::vreg(a, RegRole::Def), MachineOperand::immediate(0)}},
          {Opcode::ARG, {MachineOperand::vreg(b, RegRole::Def), MachineOperand::immediate(1)}},
          {Opcode::ADD_W,
           {MachineOperand::vreg(c, RegRole::Def), MachineOperand::vreg(a),
            MachineOperand::vreg(b)}},
          {Opcode::RETURN, {MachineOperand::vreg(c)}}},
         {},
         {}});
    m.functions.push_back(f);
    return m;
}
static void representation() {
    auto m = sample();
    verify(m, VerifyStage::Selected);
    require(dump(m).find("ADD_W") != std::string::npos, "dump misses opcode");
    auto &add = m.functions[0].blocks[0].instructions[2];
    require(add.operands[0].is_def() && !add.operands[0].is_use() && add.operands[1].is_use() &&
                !add.operands[1].is_def(),
            "roles");
    add.opcode = Opcode::ADDI_W;
    add.operands[2] = MachineOperand::immediate(2048);
    rejected([&] { verify(m, VerifyStage::Selected); });
}
static void verifier() {
    auto m = sample();
    m.functions[0].blocks[0].instructions[2].operands[1].id = 99;
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].vreg_types[1] = ValueKind::F32;
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].blocks[0].instructions.back() = {Opcode::B, {MachineOperand::block(99)}};
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].blocks[0].instructions[2].operands.pop_back();
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].blocks[0].instructions[2].operands[1] = MachineOperand::frame(0);
    rejected([&] { verify(m, VerifyStage::Final); });
    m = sample();
    m.functions[0].blocks[0].instructions[1].operands[0].id = 0;
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].vreg_types[2] = ValueKind::Ptr64;
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].blocks[0].instructions[2] = {
        Opcode::LOAD_CONSTANT,
        {MachineOperand::vreg(2, RegRole::Def), MachineOperand::immediate(INT64_C(2147483648))}};
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].vreg_types[2] = ValueKind::I1;
    m.functions[0].blocks[0].instructions[2] = {
        Opcode::LOAD_CONSTANT,
        {MachineOperand::vreg(2, RegRole::Def), MachineOperand::immediate(2)}};
    rejected([&] { verify(m, VerifyStage::Selected); });
}
static void read_modify_write() {
    using O = MachineOperand;
    const auto role = RegRole::UseDef;
    require(is_use(role) && is_def(role), "UseDef role does not expose both accesses");
    const auto &description = descriptor(Opcode::LU32I_D);
    require(description.operands[0].role == role, "LU32I_D descriptor omits incoming low-bits use");
    require(description.min_operands == 2 && description.max_operands == 2,
            "LU32I_D changed assembler arity");
    MachineFunction function;
    function.name = "read_modify_write";
    function.vreg_types = {ValueKind::Ptr64};
    function.blocks = {{0,
                        "entry",
                        {{Opcode::LU32I_D, {O::vreg(0, role), O::immediate(1)}}, {Opcode::RET, {}}},
                        {},
                        {}}};
    MachineModule module{{function}, {}};
    rejected([&] { verify(module, VerifyStage::PreRA); });
    rejected([&] { verify(module, VerifyStage::Selected); });
    module.functions[0].blocks[0].instructions.insert(
        module.functions[0].blocks[0].instructions.begin(),
        {Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(7)}});
    verify(module, VerifyStage::PreRA);
    auto wrong_width = module;
    wrong_width.functions[0].vreg_types[0] = ValueKind::I32;
    rejected([&] { verify(wrong_width, VerifyStage::PreRA); });
    rejected([&] { verify(module, VerifyStage::Selected); });
    require(dump(module).find("use-def:%v0") != std::string::npos,
            "read-modify-write use is hidden in dump");
    function.vreg_types.clear();
    function.blocks[0].instructions[0].operands[0] = O::preg(gpr(12), role);
    module.functions[0] = function;
    rejected([&] { verify(module, VerifyStage::PreRA); });
    module.functions[0].blocks[0].instructions.insert(
        module.functions[0].blocks[0].instructions.begin(),
        {Opcode::OR, {O::preg(gpr(12), RegRole::Def), O::preg(gpr(0)), O::preg(gpr(0))}});
    verify(module, VerifyStage::PreRA);
}
static void branch_ranges() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "far_branch";
    f.blocks = {{0,
                 "entry",
                 {{Opcode::BEQ, {O::preg(gpr(0)), O::preg(gpr(0)), O::block(2)}},
                  {Opcode::B, {O::block(1)}}},
                 {},
                 {}},
                {1, "padding", {}, {}, {}},
                {2, "target", {{Opcode::RET, {}}}, {}, {}}};
    for (unsigned n = 0; n < 32768; ++n)
        f.blocks[1].instructions.push_back(
            {Opcode::OR, {O::preg(gpr(12), RegRole::Def), O::preg(gpr(0)), O::preg(gpr(0))}});
    f.blocks[1].instructions.push_back({Opcode::B, {O::block(2)}});
    rebuild_cfg(f);
    MachineModule m{{f}, {}};
    rejected([&] { verify(m, VerifyStage::Final); });
    m.functions[0].blocks[0].instructions[0].opcode = Opcode::BNEZ;
    m.functions[0].blocks[0].instructions[0].operands.erase(
        m.functions[0].blocks[0].instructions[0].operands.begin());
    verify(m, VerifyStage::Final);
}
static void type_contracts() {
    auto m = sample();
    m.functions[0].vreg_types[2] = ValueKind::I1;
    m.functions[0].blocks[0].instructions[2] = {
        Opcode::COPY, {MachineOperand::vreg(2, RegRole::Def), MachineOperand::vreg(0)}};
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].vreg_types[0] = ValueKind::Ptr64;
    m.functions[0].parameter_types[0] = ValueKind::Ptr64;
    m.functions[0].vreg_types[2] = ValueKind::Ptr64;
    m.functions[0].return_type = ValueKind::Ptr64;
    m.functions[0].blocks[0].instructions[2] = {Opcode::LD_W,
                                                {MachineOperand::vreg(2, RegRole::Def),
                                                 MachineOperand::vreg(0),
                                                 MachineOperand::immediate(0)}};
    rejected([&] { verify(m, VerifyStage::Selected); });
    m = sample();
    m.functions[0].blocks[0].instructions.back() = {
        Opcode::BNEZ, {MachineOperand::vreg(2), MachineOperand::block(0)}};
    rebuild_cfg(m.functions[0]);
    rejected([&] { verify(m, VerifyStage::Selected); });
    for (bool to_float : {false, true}) {
        Module &ir = new_test_module();
        auto *fn = Function::create(FunctionType::get(ir.get_void_type(), {}),
                                    "unsupported_conversion", &ir);
        auto *bb = BasicBlock::create(&ir, "entry", fn);
        if (to_float)
            SiToFpInst::create_sitofp(ConstantInt::get(true, &ir), bb);
        else
            FpToSiInst::create_fptosi(ConstantFP::get(-1.0f, &ir), ir.get_int1_type(), bb);
        ReturnInst::create_void_ret(bb);
        rejected([&] { select(ir, {}); });
    }
}
static void baseline() {
    Module &ir = new_test_module();
    auto *i32 = ir.get_int32_type();
    auto *fp = ir.get_float_type();
    auto *fn = Function::create(FunctionType::get(i32, {i32, fp}), "baseline", &ir);
    auto *entry = BasicBlock::create(&ir, "entry", fn), *yes = BasicBlock::create(&ir, "yes", fn),
         *no = BasicBlock::create(&ir, "no", fn);
    auto ai = fn->get_args().begin();
    Value *a = *ai++, *b = *ai;
    auto *array = ir.get_array_type(ir.get_array_type(i32, 3), 2);
    auto *storage = AllocaInst::create_alloca(array, entry);
    auto *address = GetElementPtrInst::create_gep(
        storage, {ConstantInt::get(0, &ir), ConstantInt::get(1, &ir), a}, entry);
    auto *sum = IBinaryInst::create_add(a, ConstantInt::get(-1, &ir), entry);
    auto *sub = IBinaryInst::create_sub(sum, a, entry);
    auto *mul = IBinaryInst::create_mul(sub, ConstantInt::get(3, &ir), entry);
    auto *div = IBinaryInst::create_sdiv(mul, ConstantInt::get(2, &ir), entry);
    StoreInst::create_store(div, address, entry);
    auto *loaded = LoadInst::create_load(address, entry);
    auto *fadd = FBinaryInst::create_fadd(b, ConstantFP::get(-0.0f, &ir), entry);
    auto *fsub = FBinaryInst::create_fsub(fadd, ConstantFP::get(0.0f, &ir), entry);
    auto *fmul = FBinaryInst::create_fmul(fsub, ConstantFP::get(2.0f, &ir), entry);
    auto *fdiv = FBinaryInst::create_fdiv(fmul, ConstantFP::get(2.0f, &ir), entry);
    FpToSiInst::create_fptosi_to_i32(fdiv, entry);
    SiToFpInst::create_sitofp(a, entry);
    FCmpInst::create_flt(b, ConstantFP::get(std::numeric_limits<float>::quiet_NaN(), &ir), entry);
    auto *cmp = ICmpInst::create_lt(loaded, a, entry);
    ZextInst::create_zext_to_i32(cmp, entry);
    auto *global =
        GlobalVariable::create("initialized", &ir, i32, false, ConstantInt::get(-7, &ir));
    LoadInst::create_load(global, entry);
    BranchInst::create_cond_br(cmp, yes, no, entry);
    ReturnInst::create_ret(loaded, yes);
    ReturnInst::create_ret(a, no);
    auto before = ir.print();
    auto m = select(ir, {});
    verify(m, VerifyStage::Selected);
    require(before == ir.print(), "selector mutated LightIR");
    auto text = dump(m);
    for (auto op : {"ADD_W", "SUB_W", "MUL_W", "DIV_W", "FADD_S", "FSUB_S", "FMUL_S", "FDIV_S",
                    "ST_W", "LD_W", "MUL_D", "ADD_D", "FCMP_SULT_S", "MOVCF2GR", "FTINTRZ_W_S",
                    "FFINT_S_W", "SYMBOL_ADDR"})
        require(text.find(op) != std::string::npos, op);
    require(text.find("-2147483648") != std::string::npos, "negative zero bits lost");
    require(m.globals[0].initializer == std::vector<uint8_t>({249, 255, 255, 255}),
            "global initializer lost");
    bool stride12 = false;
    for (auto &i : m.functions[0].blocks[0].instructions)
        if (i.opcode == Opcode::LOAD_CONSTANT && i.operands[1].imm == 12)
            stride12 = true;
    require(stride12, "multilevel GEP stride lost");
    Module &overflow = new_test_module();
    auto *huge = overflow.get_array_type(
        overflow.get_array_type(overflow.get_int32_type(), 1u << 30), 1u << 30);
    auto *badfn = Function::create(
        FunctionType::get(overflow.get_void_type(), {overflow.get_pointer_type(huge)}), "overflow",
        &overflow);
    auto *badbb = BasicBlock::create(&overflow, "entry", badfn);
    GetElementPtrInst::create_gep(badfn->get_args().front(), {ConstantInt::get(2, &overflow)},
                                  badbb);
    ReturnInst::create_void_ret(badbb);
    rejected([&] { select(overflow, {}); });
    rejected([&] { select(overflow, {SelectionMode::Greedy, false}); });
}
static void alloca_contract() {
    Module &ir = new_test_module();
    auto *i32 = ir.get_int32_type();
    auto *fn = Function::create(FunctionType::get(i32, {}), "loop_alloca", &ir);
    auto *entry = BasicBlock::create(&ir, "entry", fn);
    auto *loop = BasicBlock::create(&ir, "loop", fn);
    auto *exit = BasicBlock::create(&ir, "exit", fn);
    auto *initial = AllocaInst::create_alloca(i32, entry);
    BranchInst::create_br(loop, entry);
    auto *previous = PhiInst::create_phi(ir.get_pointer_type(i32), loop);
    auto *iteration = PhiInst::create_phi(i32, loop);
    auto *object = AllocaInst::create_alloca(i32, loop);
    previous->add_phi_pair_operand(initial, entry);
    previous->add_phi_pair_operand(object, loop);
    iteration->add_phi_pair_operand(ConstantInt::get(0, &ir), entry);
    iteration->add_phi_pair_operand(ConstantInt::get(1, &ir), loop);
    StoreInst::create_store(iteration, object, loop);
    auto *again = ICmpInst::create_eq(iteration, ConstantInt::get(0, &ir), loop);
    BranchInst::create_cond_br(again, loop, exit, loop);
    // The first allocation must retain 0 after the second allocation receives 1.
    ReturnInst::create_ret(LoadInst::create_load(previous, exit), exit);
    for (auto mode : {SelectionMode::Baseline, SelectionMode::Greedy}) {
        for (bool metrics : {false, true}) {
            bool failed = false;
            try {
                select(ir, {mode, metrics});
            } catch (const std::runtime_error &error) {
                std::string message = error.what();
                require(message.find("alloca") != std::string::npos &&
                            message.find("entry block") != std::string::npos &&
                            message.find("loop_alloca") != std::string::npos &&
                            message.find("block loop") != std::string::npos,
                        "non-entry alloca rejection lacks context");
                failed = true;
            }
            require(failed, "selector accepted loop alloca as a fixed frame object");
        }
    }
}
static void phi() {
    MachineFunction f;
    f.name = "parallel";
    for (int i = 0; i < 4; ++i)
        f.new_vreg(ValueKind::I32);
    auto check = [&](std::vector<std::pair<MachineOperand, MachineOperand>> pairs,
                     std::map<unsigned, int> expected) {
        std::map<unsigned, int> values = {{0, 10}, {1, 20}, {2, 30}, {3, 40}};
        auto sequence = parallel_copies(f, pairs);
        for (auto &i : sequence) {
            require(i.opcode == Opcode::COPY, "unexpected parallel-copy opcode");
            values[i.operands[0].id] = values.at(i.operands[1].id);
        }
        for (auto e : expected)
            require(values.at(e.first) == e.second, "parallel copy state mismatch");
    };
    auto d = [](unsigned n) { return MachineOperand::vreg(n, RegRole::Def); };
    auto u = [](unsigned n) { return MachineOperand::vreg(n); };
    check({{d(0), u(1)}}, {{0, 20}});
    check({{d(0), u(1)}, {d(1), u(0)}}, {{0, 20}, {1, 10}});
    check({{d(0), u(1)}, {d(1), u(2)}, {d(2), u(0)}}, {{0, 20}, {1, 30}, {2, 10}});
    check({{d(0), u(1)}, {d(2), u(1)}}, {{0, 20}, {2, 20}});
    Module &ir = new_test_module();
    auto *fn = Function::create(FunctionType::get(ir.get_int32_type(), {}), "loop", &ir);
    auto *entry = BasicBlock::create(&ir, "entry", fn), *loop = BasicBlock::create(&ir, "loop", fn),
         *exit = BasicBlock::create(&ir, "exit", fn);
    BranchInst::create_br(loop, entry);
    auto *a = PhiInst::create_phi(ir.get_int32_type(), loop),
         *b = PhiInst::create_phi(ir.get_int32_type(), loop);
    a->add_phi_pair_operand(ConstantInt::get(10, &ir), entry);
    a->add_phi_pair_operand(b, loop);
    b->add_phi_pair_operand(ConstantInt::get(20, &ir), entry);
    b->add_phi_pair_operand(a, loop);
    auto *cmp = ICmpInst::create_lt(a, b, loop);
    BranchInst::create_cond_br(cmp, loop, exit, loop);
    ReturnInst::create_ret(a, exit);
    auto before = ir.print();
    auto m = select(ir, {});
    verify(m, VerifyStage::Selected);
    auto malformed = m;
    malformed.functions[0].blocks[1].instructions[0].operands.resize(3);
    rejected([&] { verify(malformed, VerifyStage::Selected); });
    lower(m, {});
    verify(m, VerifyStage::PreRA);
    require(before == ir.print(), "PHI lowering mutated LightIR");
    require(m.functions[0].blocks.size() == 4, "conditional backedge not split");
    require(m.functions[0].blocks[1].successors == std::vector<BlockId>({3, 2}),
            "wrong branch redirected");
    require(m.functions[0].blocks[3].successors == std::vector<BlockId>({1}), "edge target wrong");
    for (auto &bb : m.functions[0].blocks)
        for (auto &i : bb.instructions)
            require(i.opcode != Opcode::PHI, "PHI remains");
}
static void calls() {
    Module &ir = new_test_module();
    auto *i32 = ir.get_int32_type();
    auto *fp = ir.get_float_type();
    auto *external = Function::create(FunctionType::get(fp, {i32, fp}), "external", &ir);
    auto *fn = Function::create(FunctionType::get(fp, {i32, fp}), "mixed", &ir);
    auto *bb = BasicBlock::create(&ir, "entry", fn);
    auto a = fn->get_args().begin();
    Value *x = *a++, *y = *a;
    auto *call = CallInst::create_call(external, {x, y}, bb);
    ReturnInst::create_ret(call, bb);
    auto m = select(ir, {});
    lower(m, {});
    verify(m, VerifyStage::PreRA);
    bool found = false;
    for (auto &i : m.functions[0].blocks[0].instructions)
        if (i.opcode == Opcode::CALL) {
            found = true;
            require(descriptor(i.opcode).clobbers == caller_saved(), "call clobbers missing");
            std::set<PReg> uses, defs;
            for (auto &o : i.operands)
                if (o.kind == OperandKind::PReg) {
                    if (o.is_use())
                        uses.insert(o.reg);
                    if (o.is_def())
                        defs.insert(o.reg);
                }
            require(uses.count(gpr(4)) && uses.count(fpr(0)) && defs.count(fpr(0)),
                    "fixed ABI operands missing");
        }
    require(found, "call was not lowered");
    auto invalid_abi = m;
    for (auto &instruction : invalid_abi.functions[0].blocks[0].instructions)
        if (instruction.opcode == Opcode::CALL)
            instruction.operands.push_back(MachineOperand::preg(gpr(22)));
    rejected([&] { verify(invalid_abi, VerifyStage::PreRA); });

    Module &bad = new_test_module();
    auto *large = Function::create(
        FunctionType::get(bad.get_void_type(), std::vector<Type *>(9, bad.get_int32_type())),
        "large", &bad);
    (void)large;
    rejected([&] { select(bad, {}); });
    auto corrupted = m;
    auto &seq = corrupted.functions[0].blocks[0].instructions;
    auto it = std::find_if(seq.begin(), seq.end(),
                           [](const MachineInstr &i) { return i.opcode == Opcode::CALL; });
    seq.insert(it + 1, {Opcode::COPY,
                        {MachineOperand::vreg(0, RegRole::Def), MachineOperand::preg(gpr(4))}});
    rejected([&] { verify(corrupted, VerifyStage::PreRA); });
}
static void i1_memory() {
    Module &ir = new_test_module();
    auto *fn = Function::create(
        FunctionType::get(ir.get_int32_type(), {ir.get_pointer_type(ir.get_int1_type())}),
        "readbit", &ir);
    auto *bb = BasicBlock::create(&ir, "entry", fn);
    auto *loaded = LoadInst::create_load(fn->get_args().front(), bb);
    ReturnInst::create_ret(ZextInst::create_zext_to_i32(loaded, bb), bb);
    auto m = select(ir, {});
    verify(m, VerifyStage::Selected);
    bool canonical = false;
    for (const auto &instruction : m.functions[0].blocks[0].instructions)
        if (instruction.opcode == Opcode::ANDI) {
            canonical = instruction.operands[2].imm == 1 &&
                        m.functions[0].vreg_types[instruction.operands[0].id] == ValueKind::I1 &&
                        m.functions[0].vreg_types[instruction.operands[1].id] == ValueKind::I32;
        }
    require(canonical, "i1 load does not discard upper seven memory bits");
}
static void i1_abi() {
    Module &ir = new_test_module();
    auto *i1 = ir.get_int1_type();
    auto *external = Function::create(FunctionType::get(i1, {i1}), "external_bool", &ir);
    auto *fn = Function::create(FunctionType::get(i1, {i1}), "bool_roundtrip", &ir);
    auto *bb = BasicBlock::create(&ir, "entry", fn);
    ReturnInst::create_ret(CallInst::create_call(external, {fn->get_args().front()}, bb), bb);
    auto m = select(ir, {});
    verify(m, VerifyStage::Selected);
    lower(m, {});
    verify(m, VerifyStage::PreRA);
    unsigned masks = 0;
    for (auto &instruction : m.functions[0].blocks[0].instructions)
        if (instruction.opcode == Opcode::ANDI && instruction.operands[2].imm == 1) {
            require(m.functions[0].vreg_types[instruction.operands[0].id] == ValueKind::I1 &&
                        m.functions[0].vreg_types[instruction.operands[1].id] == ValueKind::I32,
                    "ABI i1 mask must consume raw I32");
            ++masks;
        }
    require(masks == 2, "i1 ABI entry and result capture not canonicalized");
}
static void void_main() {
    Module &ir = new_test_module();
    auto *fn = Function::create(FunctionType::get(ir.get_void_type(), {}), "main", &ir);
    auto *bb = BasicBlock::create(&ir, "entry", fn);
    ReturnInst::create_void_ret(bb);
    auto m = select(ir, {});
    lower(m, {});
    verify(m, VerifyStage::PreRA);
    const auto &instructions = m.functions[0].blocks[0].instructions;
    require(instructions.size() >= 2, "void main lacks defined zero exit code");
    require(instructions[instructions.size() - 2].opcode == Opcode::COPY &&
                instructions[instructions.size() - 2].operands[0].reg == gpr(4) &&
                instructions[instructions.size() - 2].operands[1].reg == gpr(0),
            "void main does not return zero");
    require(instructions.back().operands.size() == 1 &&
                instructions.back().operands[0].reg == gpr(4),
            "void main return register use missing");
}
static void metrics() {
    Module &ir = new_test_module();
    auto *fn = Function::create(
        FunctionType::get(ir.get_int32_type(), {ir.get_int32_type(), ir.get_float_type()}), "main",
        &ir);
    auto *bb = BasicBlock::create(&ir, "entry", fn);
    auto *a = AllocaInst::create_alloca(ir.get_array_type(ir.get_int32_type(), 4), bb);
    auto *p =
        GetElementPtrInst::create_gep(a, {ConstantInt::get(0, &ir), ConstantInt::get(0, &ir)}, bb);
    auto *l = LoadInst::create_load(p, bb);
    auto *mul = IBinaryInst::create_mul(l, ConstantInt::get(2, &ir), bb);
    auto *div = IBinaryInst::create_sdiv(mul, ConstantInt::get(2, &ir), bb);
    ReturnInst::create_ret(div, bb);
    auto off = select(ir, {});
    require(dump(off).find("add_lab4_flag") == std::string::npos, "metrics enabled by default");
    Options opts;
    opts.lab4_metrics = true;
    auto on = select(ir, opts);
    verify(on, VerifyStage::Selected);
    std::map<unsigned, int64_t> constants;
    std::vector<std::pair<int64_t, int64_t>> events;
    for (auto &i : on.functions[0].blocks[0].instructions) {
        if (i.opcode == Opcode::LOAD_CONSTANT)
            constants[i.operands[0].id] = i.operands[1].imm;
        if (i.opcode == Opcode::HIGH_CALL)
            events.emplace_back(constants.at(i.operands[1].id), constants.at(i.operands[2].id));
    }
    require(events ==
                std::vector<std::pair<int64_t, int64_t>>({{0, 16}, {1, 1}, {1, 3}, {1, 1}, {1, 4}}),
            "metrics event weights/order mismatch");
    lower(on, opts);
    verify(on, VerifyStage::PreRA);
    auto &seq = on.functions[0].blocks[0].instructions;
    require(seq[0].opcode == Opcode::COPY && seq[0].operands[1].reg == gpr(4),
            "integer argument not captured first");
    require(seq[1].opcode == Opcode::COPY && seq[1].operands[1].reg == fpr(0),
            "float argument not captured before instrumentation");
    unsigned calls = 0;
    for (auto &i : seq)
        if (i.opcode == Opcode::CALL)
            ++calls;
    require(calls == 5, "metrics calls hidden");
}
static size_t instruction_count(const MachineModule &module) {
    size_t count = 0;
    for (const auto &function : module.functions)
        for (const auto &block : function.blocks)
            count += block.instructions.size();
    return count;
}
static size_t final_count(const std::string &text) {
    std::istringstream input(text);
    std::string line;
    size_t count = 0;
    while (std::getline(input, line))
        if (line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] >= 'A' && line[2] <= 'Z')
            count += line.find("LA_LOCAL") != std::string::npos ? 2 : 1;
    return count;
}
static Options greedy_options(bool metrics = false) {
    Options options;
    options.selection = SelectionMode::Greedy;
    options.lab4_metrics = metrics;
    return options;
}
static size_t opcode_count(const MachineModule &module, Opcode opcode) {
    size_t count = 0;
    for (const auto &function : module.functions)
        for (const auto &block : function.blocks)
            for (const auto &instruction : block.instructions)
                if (instruction.opcode == opcode)
                    ++count;
    return count;
}
static void immediates() {
    size_t hits = 0, misses = 0, baseline_ops = 0, greedy_ops = 0, baseline_final = 0,
           greedy_final = 0;
    for (int c : {-2049, -2048, 2047, 2048, INT32_MIN})
        for (bool subtract : {false, true})
            for (bool left : {false, true}) {
                Module &ir = new_test_module();
                auto *fn =
                    Function::create(FunctionType::get(ir.get_int32_type(), {ir.get_int32_type()}),
                                     "immediate", &ir);
                auto *bb = BasicBlock::create(&ir, "entry", fn);
                Value *a = fn->get_args().front(), *constant = ConstantInt::get(c, &ir);
                Value *lhs = left ? constant : a, *rhs = left ? a : constant;
                auto *value = subtract ? IBinaryInst::create_sub(lhs, rhs, bb)
                                       : IBinaryInst::create_add(lhs, rhs, bb);
                ReturnInst::create_ret(value, bb);
                auto baseline = select(ir, {}), greedy = select(ir, greedy_options());
                verify(greedy, VerifyStage::Selected);
                int64_t immediate = subtract ? -static_cast<int64_t>(c) : c;
                bool hit = (!subtract || !left) && immediate >= -2048 && immediate <= 2047;
                require(opcode_count(greedy, Opcode::ADDI_W) == (hit ? 1u : 0u),
                        "signed12 immediate selection mismatch");
                if (hit) {
                    ++hits;
                    require(opcode_count(greedy, Opcode::LOAD_CONSTANT) == 0,
                            "folded arithmetic still materializes constant");
                    require(instruction_count(greedy) < instruction_count(baseline),
                            "immediate rule does not reduce operations");
                } else {
                    ++misses;
                    require(opcode_count(greedy, subtract ? Opcode::SUB_W : Opcode::ADD_W) == 1,
                            "out-of-range immediate lost fallback");
                }
                baseline_ops += instruction_count(baseline);
                greedy_ops += instruction_count(greedy);
                baseline_final += final_count(generate(ir, {}).final);
                greedy_final += final_count(generate(ir, greedy_options()).final);
                auto instrumented = select(ir, greedy_options(true));
                require(opcode_count(instrumented, Opcode::ADDI_W) == (hit ? 1u : 0u),
                        "metrics incorrectly disables single-node immediate rule");
            }
    require(greedy_final < baseline_final, "immediate rule does not reduce expanded instructions");
    std::cout << "immediates hits=" << hits << " fallback=" << misses
              << " selected=" << baseline_ops << "->" << greedy_ops << " final=" << baseline_final
              << "->" << greedy_final << '\n';
}
static void branches() {
    using Compare = ICmpInst *(*)(Value *, Value *, BasicBlock *, const std::string &);
    std::vector<Compare> comparisons = {ICmpInst::create_lt, ICmpInst::create_le,
                                        ICmpInst::create_gt, ICmpInst::create_ge,
                                        ICmpInst::create_eq, ICmpInst::create_ne};
    std::vector<Opcode> targets = {Opcode::BLT, Opcode::BGE, Opcode::BLT,
                                   Opcode::BGE, Opcode::BEQ, Opcode::BNE};
    size_t hits = 0, fallback = 0, base_ops = 0, greedy_ops = 0, base_final = 0, greedy_final = 0;
    for (unsigned predicate = 0; predicate < comparisons.size(); ++predicate)
        for (unsigned scenario = 0; scenario < 10; ++scenario) {
            Module &ir = new_test_module();
            auto *i32 = ir.get_int32_type();
            auto *fn = Function::create(FunctionType::get(i32, {i32, i32}), "branches", &ir);
            auto *entry = BasicBlock::create(&ir, "entry", fn),
                 *yes = BasicBlock::create(&ir, "yes", fn), *no = BasicBlock::create(&ir, "no", fn);
            auto arg = fn->get_args().begin();
            Value *a = *arg++, *b = *arg;
            auto *cmp = comparisons[predicate](a, b, entry, "");
            Value *condition = cmp;
            if (scenario == 1 || scenario == 5 || scenario == 6) {
                auto *wide = ZextInst::create_zext_to_i32(cmp, entry);
                condition = scenario == 6
                                ? static_cast<Value *>(
                                      ICmpInst::create_ne(ConstantInt::get(0, &ir), wide, entry))
                                : static_cast<Value *>(
                                      ICmpInst::create_ne(wide, ConstantInt::get(0, &ir), entry));
                if (scenario == 5) {
                    auto *again = ZextInst::create_zext_to_i32(condition, entry);
                    condition = ICmpInst::create_ne(again, ConstantInt::get(0, &ir), entry);
                }
            }
            Value *extra = nullptr;
            if (scenario == 2)
                extra = ZextInst::create_zext_to_i32(cmp, entry);
            if (scenario == 3) {
                auto *barrier =
                    Function::create(FunctionType::get(ir.get_void_type(), {}), "barrier", &ir);
                CallInst::create_call(barrier, {}, entry);
            }
            if (scenario == 8 || scenario == 9) {
                auto *slot = AllocaInst::create_alloca(i32, entry);
                if (scenario == 8)
                    LoadInst::create_load(slot, entry);
                else
                    StoreInst::create_store(ConstantInt::get(1, &ir), slot, entry);
            }
            auto *dispatch = entry;
            if (scenario == 7) {
                dispatch = BasicBlock::create(&ir, "dispatch", fn);
                BranchInst::create_br(dispatch, entry);
            }
            BranchInst::create_cond_br(condition, yes, no, dispatch);
            ReturnInst::create_ret(extra ? extra : ConstantInt::get(-3, &ir), yes);
            ReturnInst::create_ret(ConstantInt::get(7, &ir), no);
            bool metrics = scenario == 4;
            auto options = greedy_options(metrics);
            Options baseline_options;
            baseline_options.lab4_metrics = metrics;
            auto base = select(ir, baseline_options), chosen = select(ir, options);
            verify(chosen, VerifyStage::Selected);
            bool hit = scenario < 2 || scenario == 5 || scenario == 6;
            if (hit) {
                ++hits;
                require(opcode_count(chosen, targets[predicate]) == 1,
                        "integer compare branch was not fused");
                require(opcode_count(chosen, Opcode::BNEZ) == 0,
                        "fused branch still materializes boolean");
                require(instruction_count(chosen) < instruction_count(base),
                        "branch fusion did not reduce selected operations");
                const auto &branch = chosen.functions[0].blocks[0].instructions[2];
                require(branch.opcode == targets[predicate], "branch emitted at wrong position");
                require(branch.operands[0].id == ((predicate == 1 || predicate == 2) ? 1u : 0u),
                        "branch predicate operand direction reversed");
            } else {
                ++fallback;
                require(opcode_count(chosen, Opcode::BNEZ) == 1,
                        "unsafe/shared/metrics branch failed to retain fallback");
            }
            base_ops += instruction_count(base);
            greedy_ops += instruction_count(chosen);
            base_final += final_count(generate(ir, baseline_options).final);
            greedy_final += final_count(generate(ir, options).final);
        }
    require(greedy_final < base_final, "branch fusion did not reduce expanded operations");
    std::cout << "branches hits=" << hits << " fallback=" << fallback << " selected=" << base_ops
              << "->" << greedy_ops << " final=" << base_final << "->" << greedy_final << '\n';
}
static void negative_branches() {
    using Compare = ICmpInst *(*)(Value *, Value *, BasicBlock *, const std::string &);
    std::vector<Compare> comparisons = {ICmpInst::create_lt, ICmpInst::create_le,
                                        ICmpInst::create_gt, ICmpInst::create_ge,
                                        ICmpInst::create_eq, ICmpInst::create_ne};
    for (unsigned predicate = 0; predicate < 6; ++predicate)
        for (bool left_constant : {false, true}) {
            Module &ir = new_test_module();
            auto *i32 = ir.get_int32_type();
            auto *fn = Function::create(FunctionType::get(i32, {i32}), "negative_predicate", &ir);
            auto *entry = BasicBlock::create(&ir, "entry", fn),
                 *yes = BasicBlock::create(&ir, "yes", fn), *no = BasicBlock::create(&ir, "no", fn);
            Value *variable = fn->get_args().front(), *constant = ConstantInt::get(-7, &ir);
            auto *cmp = comparisons[predicate](left_constant ? constant : variable,
                                               left_constant ? variable : constant, entry, "");
            BranchInst::create_cond_br(cmp, yes, no, entry);
            ReturnInst::create_ret(ConstantInt::get(1, &ir), yes);
            ReturnInst::create_ret(ConstantInt::get(0, &ir), no);
            auto selected = select(ir, greedy_options());
            verify(selected, VerifyStage::Selected);
            for (int input : {-9, -7, -5}) {
                std::map<unsigned, int64_t> regs = {{0, input}};
                bool found = false, taken = false;
                for (const auto &instruction : selected.functions[0].blocks[0].instructions) {
                    if (instruction.opcode == Opcode::LOAD_CONSTANT)
                        regs[instruction.operands[0].id] = instruction.operands[1].imm;
                    if (instruction.opcode == Opcode::BLT || instruction.opcode == Opcode::BGE ||
                        instruction.opcode == Opcode::BEQ || instruction.opcode == Opcode::BNE) {
                        found = true;
                        auto a = regs.at(instruction.operands[0].id),
                             b = regs.at(instruction.operands[1].id);
                        taken = instruction.opcode == Opcode::BLT   ? a < b
                                : instruction.opcode == Opcode::BGE ? a >= b
                                : instruction.opcode == Opcode::BEQ ? a == b
                                                                    : a != b;
                    }
                }
                int a = left_constant ? -7 : input, b = left_constant ? input : -7;
                bool expected = predicate == 0   ? a < b
                                : predicate == 1 ? a <= b
                                : predicate == 2 ? a > b
                                : predicate == 3 ? a >= b
                                : predicate == 4 ? a == b
                                                 : a != b;
                require(found && taken == expected,
                        "negative constant branch changes signed predicate direction");
            }
        }
    std::cout << "negative branches predicates=6 operand-orders=2 inputs=3 passed\n";
}
static void boolean_roots() {
    for (unsigned root = 0; root < 3; ++root)
        for (bool metrics : {false, true}) {
            Module &ir = new_test_module();
            auto *i32 = ir.get_int32_type();
            Type *argtype = root == 0 ? static_cast<Type *>(ir.get_float_type())
                            : root == 1
                                ? static_cast<Type *>(ir.get_int1_type())
                                : static_cast<Type *>(ir.get_pointer_type(ir.get_int1_type()));
            auto *fn = Function::create(FunctionType::get(i32, {argtype}), "boolean_root", &ir);
            auto *entry = BasicBlock::create(&ir, "entry", fn),
                 *yes = BasicBlock::create(&ir, "yes", fn), *no = BasicBlock::create(&ir, "no", fn);
            Value *value = fn->get_args().front();
            if (root == 0)
                value = FCmpInst::create_flt(value, ConstantFP::get(-0.0f, &ir), entry);
            else if (root == 2)
                value = LoadInst::create_load(value, entry);
            auto *wide = ZextInst::create_zext_to_i32(value, entry);
            auto *condition = ICmpInst::create_ne(wide, ConstantInt::get(0, &ir), entry);
            BranchInst::create_cond_br(condition, yes, no, entry);
            ReturnInst::create_ret(ConstantInt::get(1, &ir), yes);
            ReturnInst::create_ret(ConstantInt::get(0, &ir), no);
            Options baseoptions;
            baseoptions.lab4_metrics = metrics;
            auto baseline = select(ir, baseoptions), greedy = select(ir, greedy_options(metrics));
            verify(greedy, VerifyStage::Selected);
            require(opcode_count(greedy, Opcode::BNEZ) == 1, "underlying I1 branch disappeared");
            if (metrics)
                require(instruction_count(greedy) == instruction_count(baseline),
                        "metrics allowed boolean-chain fusion");
            else {
                require(opcode_count(greedy, Opcode::COPY) == 0 &&
                            opcode_count(greedy, Opcode::XOR) == 0 &&
                            opcode_count(greedy, Opcode::SLTU) == 0,
                        "general I1 boolean chain was not eliminated");
                require(instruction_count(greedy) < instruction_count(baseline),
                        "general boolean chain did not reduce instructions");
            }
            if (root == 0)
                require(opcode_count(greedy, Opcode::FCMP_SULT_S) == 1 &&
                            opcode_count(greedy, Opcode::MOVCF2GR) == 1,
                        "floating predicate producer duplicated or removed");
            if (root == 2)
                require(opcode_count(greedy, Opcode::LD_B_U) == 1 &&
                            opcode_count(greedy, Opcode::ANDI) == 1,
                        "I1 load producer duplicated or removed");
            generate(ir, greedy_options(metrics));
        }
    std::cout << "boolean roots float/i1-argument/i1-load metrics on/off passed\n";
}
static void addresses() {
    size_t hits = 0, fallback = 0, base_ops = 0, greedy_ops = 0, base_final = 0, greedy_final = 0;
    for (int index : {0, 3, -3, 511, 512, -512, -513})
        for (unsigned scenario = 0; scenario < 10; ++scenario) {
            Module &ir = new_test_module();
            auto *i32 = ir.get_int32_type();
            auto *array = ir.get_array_type(i32, 4);
            auto *fn = Function::create(FunctionType::get(i32, {ir.get_pointer_type(array), i32}),
                                        "addresses", &ir);
            auto *entry = BasicBlock::create(&ir, "entry", fn);
            auto args = fn->get_args().begin();
            Value *base = *args++, *dynamic = *args;
            Value *subscript = scenario == 1 ? dynamic : ConstantInt::get(index, &ir);
            auto *gep = GetElementPtrInst::create_gep(
                base, {ConstantInt::get(scenario == 7 ? 1 : 0, &ir), subscript}, entry);
            if (scenario == 2)
                LoadInst::create_load(gep, entry);
            if (scenario == 3) {
                auto *barrier =
                    Function::create(FunctionType::get(ir.get_void_type(), {}), "barrier", &ir);
                CallInst::create_call(barrier, {}, entry);
            }
            if (scenario == 4) {
                auto *other = AllocaInst::create_alloca(i32, entry);
                StoreInst::create_store(ConstantInt::get(8, &ir), other, entry);
            }
            if (scenario == 8) {
                auto *slot = AllocaInst::create_alloca(i32, entry);
                LoadInst::create_load(slot, entry);
            }
            auto *consumer = entry;
            if (scenario == 9) {
                consumer = BasicBlock::create(&ir, "consumer", fn);
                BranchInst::create_br(consumer, entry);
            }
            Instruction *loaded;
            if (scenario == 6) {
                loaded = StoreInst::create_store(ConstantInt::get(-5, &ir), gep, consumer);
                ReturnInst::create_ret(ConstantInt::get(0, &ir), consumer);
            } else {
                loaded = LoadInst::create_load(gep, consumer);
                ReturnInst::create_ret(loaded, consumer);
            }
            Options baseline_options;
            baseline_options.lab4_metrics = scenario == 5;
            auto options = greedy_options(scenario == 5);
            auto before = ir.print();
            auto baseline = select(ir, baseline_options), greedy = select(ir, options);
            verify(greedy, VerifyStage::Selected);
            require(ir.print() == before, "address planning mutated input IR");
            int64_t offset = int64_t(index) * 4 + (scenario == 7 ? 16 : 0);
            bool hit = (scenario == 0 || scenario == 6 || scenario == 7) && offset >= -2048 &&
                       offset <= 2047;
            if (hit) {
                ++hits;
                require(opcode_count(greedy, Opcode::MUL_D) == 0 &&
                            opcode_count(greedy, Opcode::ADD_D) == 0,
                        "constant address was not folded");
                bool found = false;
                for (const auto &instruction : greedy.functions[0].blocks[0].instructions)
                    if (instruction.opcode == (scenario == 6 ? Opcode::ST_W : Opcode::LD_W) &&
                        instruction.source == loaded) {
                        found = instruction.operands[2].imm == offset &&
                                instruction.operands[1].id == 0;
                    }
                require(found, "folded load lost typed offset/base");
            } else {
                ++fallback;
                require(opcode_count(greedy, Opcode::MUL_D) == 2,
                        "ineligible GEP did not retain complete fallback");
            }
            base_ops += instruction_count(baseline);
            greedy_ops += instruction_count(greedy);
            base_final += final_count(generate(ir, baseline_options).final);
            greedy_final += final_count(generate(ir, options).final);
        }
    require(greedy_final < base_final, "address fold did not reduce expanded operations");
    std::cout << "addresses hits=" << hits << " fallback=" << fallback << " selected=" << base_ops
              << "->" << greedy_ops << " final=" << base_final << "->" << greedy_final << '\n';
}
static void emit_interop(const std::string &directory) {
    Module &ir = new_test_module();
    auto *i32 = ir.get_int32_type();
    auto *fp = ir.get_float_type();
    auto *ptr = ir.get_int32_ptr_type();
    auto *ci = Function::create(FunctionType::get(i32, {i32, fp, ptr}), "c_mix_i", &ir);
    auto *cf = Function::create(FunctionType::get(fp, {i32, fp, ptr}), "c_mix_f", &ir);
    for (bool floating : {false, true}) {
        auto *fn = Function::create(
            FunctionType::get(floating ? static_cast<Type *>(fp) : static_cast<Type *>(i32),
                              {i32, fp, ptr}),
            floating ? "mir_call_f" : "mir_call_i", &ir);
        auto *bb = BasicBlock::create(&ir, "entry", fn);
        std::vector<Value *> args(fn->get_args().begin(), fn->get_args().end());
        auto *i = CallInst::create_call(ci, args, bb);
        auto *f = CallInst::create_call(cf, args, bb);
        if (floating)
            ReturnInst::create_ret(
                FBinaryInst::create_fadd(f, SiToFpInst::create_sitofp(i, bb), bb), bb);
        else
            ReturnInst::create_ret(
                IBinaryInst::create_add(i, FpToSiInst::create_fptosi_to_i32(f, bb), bb), bb);
    }
    auto *readbit = Function::create(
        FunctionType::get(i32, {ir.get_pointer_type(ir.get_int1_type())}), "mir_readbit", &ir);
    auto *bitblock = BasicBlock::create(&ir, "entry", readbit);
    ReturnInst::create_ret(
        ZextInst::create_zext_to_i32(LoadInst::create_load(readbit->get_args().front(), bitblock),
                                     bitblock),
        bitblock);
    auto *identity = Function::create(FunctionType::get(ir.get_int1_type(), {ir.get_int1_type()}),
                                      "mir_identity", &ir);
    auto *identity_block = BasicBlock::create(&ir, "entry", identity);
    ReturnInst::create_ret(identity->get_args().front(), identity_block);
    auto *external_bit = Function::create(
        FunctionType::get(ir.get_int1_type(), {ir.get_pointer_type(ir.get_int1_type())}),
        "c_readbit", &ir);
    auto *call_bit = Function::create(
        FunctionType::get(i32, {ir.get_pointer_type(ir.get_int1_type())}), "mir_call_bit", &ir);
    auto *call_bit_block = BasicBlock::create(&ir, "entry", call_bit);
    auto *called_bit =
        CallInst::create_call(external_bit, {call_bit->get_args().front()}, call_bit_block);
    ReturnInst::create_ret(ZextInst::create_zext_to_i32(called_bit, call_bit_block),
                           call_bit_block);
    std::filesystem::create_directories(directory);
    std::ofstream(directory + "/interop.ll") << ir.print();
    std::ofstream(directory + "/interop.baseline.s") << generate(ir, {}).assembly;
    std::ofstream(directory + "/interop.greedy.s") << generate(ir, greedy_options()).assembly;
}
namespace backend_regression {
void stack();
void frame();
void printer();
} // namespace backend_regression
int main(int argc, char **argv) {
    try {
        std::string group = argc > 1 ? argv[1] : "all";
        if (group == "emit-interop") {
            require(argc == 3, "emit-interop requires directory");
            emit_interop(argv[2]);
            return 0;
        }
        if (group == "all" || group == "representation")
            representation();
        if (group == "all" || group == "verifier") {
            verifier();
            branch_ranges();
            read_modify_write();
            type_contracts();
            i1_memory();
        }
        if (group == "all" || group == "baseline")
            baseline();
        if (group == "all" || group == "alloca")
            alloca_contract();
        if (group == "all" || group == "phi")
            phi();
        if (group == "all" || group == "calls") {
            calls();
            void_main();
            i1_abi();
        }
        if (group == "all" || group == "metrics")
            metrics();
        if (group == "all" || group == "immediates")
            immediates();
        if (group == "all" || group == "branches") {
            branches();
            negative_branches();
            boolean_roots();
        }
        if (group == "all" || group == "addresses")
            addresses();
        if (group == "all" || group == "stack")
            backend_regression::stack();
        if (group == "all" || group == "frame")
            backend_regression::frame();
        if (group == "all" || group == "printer")
            backend_regression::printer();
        const std::set<std::string> groups = {
            "all",        "representation", "verifier",  "baseline", "phi",   "calls",   "metrics",
            "immediates", "branches",       "addresses", "stack",    "frame", "printer", "alloca"};
        require(groups.count(group), "unknown machine regression group");
        std::cout << group << " passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
