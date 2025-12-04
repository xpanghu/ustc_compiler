#include "CodeGen.hpp"

#include <cstring>

#include "ASMInstruction.hpp"
#include "BasicBlock.hpp"
#include "CodeGenUtil.hpp"
#include "Function.hpp"
#include "Instruction.hpp"
#include "Register.hpp"
#include "Type.hpp"
#include <string>


void CodeGen::allocate()
{
    unsigned offset = PROLOGUE_OFFSET_BASE;
    for (auto& arg : context.func->get_args())
    {
        auto size = arg->get_type()->get_size();
        offset = ALIGN(offset + size, size);
        context.offset_map[arg] = -static_cast<int>(offset);
    }
    for (auto& bb : context.func->get_basic_blocks())
    {
        for (auto& instr : bb->get_instructions())
        {
            if (not instr->is_void())
            {
                auto size = instr->get_type()->get_size();
                offset = ALIGN(offset + size, size > 8 ? 8 : size);
                context.offset_map[instr] = -static_cast<int>(offset);
            }
            if (instr->is_alloca())
            {
                auto* alloca_inst = dynamic_cast<AllocaInst*>(instr);
                auto alloc_size = alloca_inst->get_alloca_type()->get_size();
                offset = ALIGN(offset + alloc_size, alloc_size > 8 ? 8 : alloc_size);
            }
        }
    }
    context.frame_size = ALIGN(offset, PROLOGUE_ALIGN);
}

void CodeGen::copy_stmt()
{
    for (auto& succ : context.bb->get_succ_basic_blocks())
    {
        for (auto& inst : succ->get_instructions())
        {
            if (inst->is_phi())
            {
                for (unsigned i = 1; i < inst->get_operands().size(); i += 2)
                {
                    if (inst->get_operand(i) == context.bb)
                    {
                        auto* lvalue = inst->get_operand(i - 1);
                        if (lvalue->get_type()->is_float_type())
                        {
                            load_to_freg(lvalue, FReg::fa(0));
                            store_from_freg(inst, FReg::fa(0));
                        }
                        else
                        {
                            load_to_greg(lvalue, Reg::a(0));
                            store_from_greg(inst, Reg::a(0));
                        }
                        break;
                    }
                }
            }
            else
            {
                break;
            }
        }
    }
}

void CodeGen::load_to_greg(Value* val, const Reg& reg)
{
    assert(val->get_type()->is_integer_type() ||
        val->get_type()->is_pointer_type());

    if (auto* constant = dynamic_cast<ConstantInt*>(val))
    {
        int32_t val1 = constant->get_value();
        if (IS_IMM_12(val1))
        {
            append_inst(ADDI WORD, {reg.print(), "$zero", std::to_string(val1)});
        }
        else
        {
            load_large_int32(val1, reg);
        }
    }
    else if (auto* global = dynamic_cast<GlobalVariable*>(val))
    {
        append_inst(LOAD_ADDR, {reg.print(), global->get_name()});
    }
    else
    {
        load_from_stack_to_greg(val, reg);
    }
}

void CodeGen::load_large_int32(int32_t val, const Reg& reg)
{
    int32_t high_20 = val >> 12;
    uint32_t low_12 = val & LOW_12_MASK;
    append_inst(LU12I_W, {reg.print(), std::to_string(high_20)});
    append_inst(ORI, {reg.print(), reg.print(), std::to_string(low_12)});
}

void CodeGen::load_large_int64(int64_t val, const Reg& reg)
{
    auto low_32 = static_cast<int32_t>(val & LOW_32_MASK);
    load_large_int32(low_32, reg);

    auto high_32 = static_cast<int32_t>(val >> 32);
    int32_t high_32_low_20 = (high_32 << 12) >> 12;
    int32_t high_32_high_12 = high_32 >> 20;
    append_inst(LU32I_D, {reg.print(), std::to_string(high_32_low_20)});
    append_inst(LU52I_D,
                {reg.print(), reg.print(), std::to_string(high_32_high_12)});
}

void CodeGen::load_from_stack_to_greg(Value* val, const Reg& reg)
{
    auto offset = context.offset_map.at(val);
    auto offset_str = std::to_string(offset);
    auto* type = val->get_type();
    if (IS_IMM_12(offset))
    {
        if (type->is_int1_type())
        {
            append_inst(LOAD BYTE, {reg.print(), "$fp", offset_str});
        }
        else if (type->is_int32_type())
        {
            append_inst(LOAD WORD, {reg.print(), "$fp", offset_str});
        }
        else
        {
            append_inst(LOAD DOUBLE, {reg.print(), "$fp", offset_str});
        }
    }
    else
    {
        load_large_int64(offset, reg);
        append_inst(ADD DOUBLE, {reg.print(), "$fp", reg.print()});
        if (type->is_int1_type())
        {
            append_inst(LOAD BYTE, {reg.print(), reg.print(), "0"});
        }
        else if (type->is_int32_type())
        {
            append_inst(LOAD WORD, {reg.print(), reg.print(), "0"});
        }
        else
        {
            append_inst(LOAD DOUBLE, {reg.print(), reg.print(), "0"});
        }
    }
}

void CodeGen::store_from_greg(Value* val, const Reg& reg)
{
    auto offset = context.offset_map.at(val);
    auto offset_str = std::to_string(offset);
    auto* type = val->get_type();
    if (IS_IMM_12(offset))
    {
        if (type->is_int1_type())
        {
            append_inst(STORE BYTE, {reg.print(), "$fp", offset_str});
        }
        else if (type->is_int32_type())
        {
            append_inst(STORE WORD, {reg.print(), "$fp", offset_str});
        }
        else
        {
            append_inst(STORE DOUBLE, {reg.print(), "$fp", offset_str});
        }
    }
    else
    {
        auto addr = Reg::t(8);
        load_large_int64(offset, addr);
        append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
        if (type->is_int1_type())
        {
            append_inst(STORE BYTE, {reg.print(), addr.print(), "0"});
        }
        else if (type->is_int32_type())
        {
            append_inst(STORE WORD, {reg.print(), addr.print(), "0"});
        }
        else
        {
            append_inst(STORE DOUBLE, {reg.print(), addr.print(), "0"});
        }
    }
}

void CodeGen::load_to_freg(Value* val, const FReg& freg)
{
    assert(val->get_type()->is_float_type());
    if (auto* constant = dynamic_cast<ConstantFP*>(val))
    {
        float val1 = constant->get_value();
        load_float_imm(val1, freg);
    }
    else
    {
        auto offset = context.offset_map.at(val);
        auto offset_str = std::to_string(offset);
        if (IS_IMM_12(offset))
        {
            append_inst(FLOAD SINGLE, {freg.print(), "$fp", offset_str});
        }
        else
        {
            auto addr = Reg::t(8);
            load_large_int64(offset, addr);
            append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
            append_inst(FLOAD SINGLE, {freg.print(), addr.print(), "0"});
        }
    }
}

void CodeGen::load_float_imm(float val, const FReg& r)
{
    int32_t bytes = 0;
    memcpy(&bytes, &val, sizeof(float));
    load_large_int32(bytes, Reg::t(8));
    append_inst(GR2FR WORD, {r.print(), Reg::t(8).print()});
}

void CodeGen::store_from_freg(Value* val, const FReg& r)
{
    auto offset = context.offset_map.at(val);
    if (IS_IMM_12(offset))
    {
        auto offset_str = std::to_string(offset);
        append_inst(FSTORE SINGLE, {r.print(), "$fp", offset_str});
    }
    else
    {
        auto addr = Reg::t(8);
        load_large_int64(offset, addr);
        append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
        append_inst(FSTORE SINGLE, {r.print(), addr.print(), "0"});
    }
}

void CodeGen::gen_prologue()
{
    if (IS_IMM_12(-static_cast<int>(context.frame_size)))
    {
        append_inst("st.d $ra, $sp, -8");
        append_inst("st.d $fp, $sp, -16");
        append_inst("addi.d $fp, $sp, 0");
        append_inst("addi.d $sp, $sp, " +
                    std::to_string(-static_cast<int>(context.frame_size)));
    }
    else
    {
        load_large_int64(context.frame_size, Reg::t(0));
        append_inst("st.d $ra, $sp, -8");
        append_inst("st.d $fp, $sp, -16");
        append_inst("sub.d $sp, $sp, $t0");
        append_inst("add.d $fp, $sp, $t0");
    }

    int garg_cnt = 0;
    int farg_cnt = 0;
    for (auto arg : context.func->get_args())
    {
        if (arg->get_type()->is_float_type())
        {
            store_from_freg(arg, FReg::fa(farg_cnt++));
        }
        else
        {
            store_from_greg(arg, Reg::a(garg_cnt++));
        }
    }
    
    if (context.func->get_name() == "main")
    {
        int allocate_size = 0;
        for (auto func : context.func->get_parent()->get_functions())
        {
            if (func->is_declaration()) continue;

            for (auto& bb : func->get_basic_blocks())
            {
                for (auto& instr : bb->get_instructions())
                {
                    if (instr->is_alloca())
                    {
                        auto* alloca_inst = dynamic_cast<AllocaInst*>(instr);
                        allocate_size += static_cast<int>(alloca_inst->get_alloca_type()->get_size());
                    }
                }
            }
        }

        load_to_greg(ConstantInt::get(0, m), Reg::a(0));
        load_to_greg(ConstantInt::get(allocate_size, m), Reg::a(1));
        append_inst("bl add_lab4_flag");
    }
}

void CodeGen::gen_epilogue()
{
    append_inst(context.func->get_name() + "_exit", ASMInstruction::Label);
    if (IS_IMM_12(-static_cast<int>(context.frame_size)))
    {
        append_inst("addi.d $sp, $sp, " + std::to_string(static_cast<int>(context.frame_size)));
        append_inst("ld.d $ra, $sp, -8");
        append_inst("ld.d $fp, $sp, -16");
        append_inst("jr $ra");
    }
    else
    {
        load_large_int64(context.frame_size, Reg::t(0));
        append_inst("add.d $sp, $sp, $t0");
        append_inst("ld.d $ra, $sp, -8");
        append_inst("ld.d $fp, $sp, -16");
        append_inst("jr $ra");
    }
}


void CodeGen::gen_ret()
{
    auto* retInst = dynamic_cast<ReturnInst*>(context.inst);
    auto* retType = context.func->get_return_type();
    if (retType->is_void_type())
    {
        append_inst("addi.w  $a0, $zero, 0");
    }
    else if (retType->is_float_type())
    {
        load_to_freg(retInst->get_operand(0), FReg::fa(0));
    }
    else
    {
        load_to_greg(retInst->get_operand(0), Reg::a(0));
    }
    std::string label = context.func->get_name() + "_exit";
    append_inst("b " + label);
}

void CodeGen::gen_br()
{
    auto* branchInst = dynamic_cast<BranchInst*>(context.inst);
    if (branchInst->is_cond_br())
    {
        load_to_greg(branchInst->get_operand(0), Reg::t(0));
        auto* trueBB = dynamic_cast<BasicBlock*>(branchInst->get_operand(1));
        auto* falseBB = dynamic_cast<BasicBlock*>(branchInst->get_operand(2));
        append_inst("bnez", {Reg::t(0).print(), trueBB->get_name()});
        append_inst("b", {falseBB->get_name()});
    }
    else
    {
        auto* branchbb = dynamic_cast<BasicBlock*>(branchInst->get_operand(0));
        append_inst("b " + branchbb->get_name());
    }
}

void CodeGen::gen_binary()
{
    load_to_greg(context.inst->get_operand(0), Reg::t(0));
    load_to_greg(context.inst->get_operand(1), Reg::t(1));
    switch (context.inst->get_instr_type())
    {
        case Instruction::add:
            output.emplace_back("add.w $t2, $t0, $t1");
            break;
        case Instruction::sub:
            output.emplace_back("sub.w $t2, $t0, $t1");
            break;
        case Instruction::mul:
            output.emplace_back("mul.w $t2, $t0, $t1");
            break;
        case Instruction::sdiv:
            output.emplace_back("div.w $t2, $t0, $t1");
            break;
        default:
            assert(false);
    }
    store_from_greg(context.inst, Reg::t(2));
    if (context.inst->get_instr_type() == Instruction::mul)
    {
        load_to_greg(ConstantInt::get(1, m), Reg::a(0));
        load_to_greg(ConstantInt::get(1, m), Reg::a(1));
        append_inst("bl add_lab4_flag");
    }
    if (context.inst->get_instr_type() == Instruction::sdiv)
    {
        load_to_greg(ConstantInt::get(1, m), Reg::a(0));
        load_to_greg(ConstantInt::get(4, m), Reg::a(1));
        append_inst("bl add_lab4_flag");
    }
}

void CodeGen::gen_float_binary()
{
    auto* floatInst = dynamic_cast<FBinaryInst*>(context.inst);
    auto op = floatInst->get_instr_type();
    auto firstNum = floatInst->get_operand(0);
    load_to_freg(firstNum, FReg::ft(1));
    auto secondNum = floatInst->get_operand(1);
    load_to_freg(secondNum, FReg::ft(2));
    switch (op)
    {
        case Instruction::fadd:
            append_inst("fadd.s", {FReg::ft(0).print(), FReg::ft(1).print(), FReg::ft(2).print()});
            break;
        case Instruction::fsub:
            append_inst("fsub.s", {FReg::ft(0).print(), FReg::ft(1).print(), FReg::ft(2).print()});
            break;
        case Instruction::fmul:
            append_inst("fmul.s", {FReg::ft(0).print(), FReg::ft(1).print(), FReg::ft(2).print()});
            break;
        case Instruction::fdiv:
            append_inst("fdiv.s", {FReg::ft(0).print(), FReg::ft(1).print(), FReg::ft(2).print()});
            break;
        default:
            std::cout << "wrong gen_float_binary\n";
            break;
    }
    store_from_freg(context.inst, FReg::ft(0));
    if (context.inst->get_instr_type() == Instruction::fmul)
    {
        load_to_greg(ConstantInt::get(1, m), Reg::a(0));
        load_to_greg(ConstantInt::get(1, m), Reg::a(1));
        append_inst("bl add_lab4_flag");
    }
    if (context.inst->get_instr_type() == Instruction::fdiv)
    {
        load_to_greg(ConstantInt::get(1, m), Reg::a(0));
        load_to_greg(ConstantInt::get(4, m), Reg::a(1));
        append_inst("bl add_lab4_flag");
    }
}

void CodeGen::gen_alloca()
{
    auto* allocaInst = dynamic_cast<AllocaInst*>(context.inst);
    auto offset = context.offset_map[allocaInst];
    auto trueOffset = offset - static_cast<int>(allocaInst->get_alloca_type()->get_size());
    if (IS_IMM_12(trueOffset))
        append_inst("addi.d", {Reg::t(0).print(), "$fp", std::to_string(trueOffset)});
    else
    {
        load_to_greg(ConstantInt::get(trueOffset, m), Reg::t(1));
        append_inst("add.d", {Reg::t(0).print(), "$fp", Reg::t(1).print()});
    }
    store_from_greg(allocaInst, Reg::t(0));
}

void CodeGen::gen_load()
{
    auto* ptr = context.inst->get_operand(0);
    auto* type = context.inst->get_type();
    load_to_greg(ptr, Reg::t(0));

    if (type->is_float_type())
    {
        append_inst("fld.s $ft0, $t0, 0");
        store_from_freg(context.inst, FReg::ft(0));
    }
    else if (type->is_int32_type())
    {
        append_inst("ld.w $t0, $t0, 0");
        store_from_greg(context.inst, Reg::t(0));
    }
    else if (type->is_int1_type())
    {
        append_inst("ld.b $t0, $t0, 0");
        store_from_greg(context.inst, Reg::t(0));
    }
    else
    {
        append_inst("ld.d $t0, $t0, 0");
        store_from_greg(context.inst, Reg::t(0));
    }
    if (ptr->is<AllocaInst>() && !((ptr->as<AllocaInst>())->get_alloca_type()->is_array_type())) return;
    load_to_greg(ConstantInt::get(1, m), Reg::a(0));
    load_to_greg(ConstantInt::get(3, m), Reg::a(1));
    append_inst("bl add_lab4_flag");
}

void CodeGen::gen_store()
{
    auto* storeInst = dynamic_cast<StoreInst*>(context.inst);
    auto addr = storeInst->get_operand(1);
    auto value = storeInst->get_operand(0);
    load_to_greg(addr, Reg::t(0));
    if (value->get_type()->is_float_type())
    {
        load_to_freg(value, FReg::ft(0));
        append_inst("fst.s $ft0, $t0, 0");
    }
    else if (value->get_type()->is_int32_type())
    {
        load_to_greg(value, Reg::t(1));
        append_inst("st.w $t1, $t0, 0");
    }
    else if (value->get_type()->is_int1_type())
    {
        load_to_greg(value, Reg::t(1));
        append_inst("st.b $t1, $t0, 0");
    }
    else
    {
        load_to_greg(value, Reg::t(1));
        append_inst("st.d $t1, $t0, 0");
    }
}

void CodeGen::gen_icmp()
{
    auto* icmpInst = dynamic_cast<ICmpInst*>(context.inst);
    auto op = icmpInst->get_instr_type();
    load_to_greg(icmpInst->get_operand(0), Reg::t(0));
    load_to_greg(icmpInst->get_operand(1), Reg::t(1));
    switch (op)
    {
        case Instruction::ge:
            append_inst("slt $t0, $t0, $t1");
            append_inst("addi.d $t1, $zero, 1");
            append_inst("xor $t0, $t0, $t1");
            break;
        case Instruction::gt:
            append_inst("slt $t0, $t1, $t0");
            break;
        case Instruction::le:
            append_inst("slt $t0, $t1, $t0");
            append_inst("addi.d $t1, $zero, 1");
            append_inst("xor $t0, $t0, $t1");
            break;
        case Instruction::lt:
            append_inst("slt $t0, $t0, $t1");
            break;
        case Instruction::eq:
            append_inst("xor $t0, $t0, $t1");
            append_inst("sltu $t0, $zero, $t0");
            append_inst("addi.d $t1, $zero, 1");
            append_inst("xor $t0, $t0, $t1");
            break;
        case Instruction::ne:
            append_inst("xor $t0, $t0, $t1");
            append_inst("sltu $t0, $zero, $t0");
            break;
        default:
            std::cout << "wrong icmp\n";
            break;
    }
    store_from_greg(icmpInst, Reg::t(0));
}

void CodeGen::gen_fcmp()
{
    auto* fcmpInst = dynamic_cast<FCmpInst*>(context.inst);
    auto op = fcmpInst->get_instr_type();
    load_to_freg(fcmpInst->get_operand(0), FReg::ft(0));
    load_to_freg(fcmpInst->get_operand(1), FReg::ft(1));
    switch (op)
    {
        case Instruction::fge:
            append_inst("fcmp.sle.s $fcc0, $ft1, $ft0");
            break;
        case Instruction::fgt:
            append_inst("fcmp.slt.s $fcc0, $ft1, $ft0");
            break;
        case Instruction::fle:
            append_inst("fcmp.sle.s $fcc0, $ft0, $ft1");
            break;
        case Instruction::flt:
            append_inst("fcmp.slt.s $fcc0, $ft0, $ft1");
            break;
        case Instruction::feq:
            append_inst("fcmp.seq.s $fcc0, $ft0, $ft1");
            break;
        case Instruction::fne:
            append_inst("fcmp.sne.s $fcc0, $ft0, $ft1");
            break;
        default:
            break;
    }
    append_inst("bceqz", {"$fcc0", "0XC"});
    append_inst("addi.w $t0, $zero, 1");
    append_inst("b 0x8");
    append_inst("addi.w $t0, $zero, 0");
    store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::gen_zext()
{
    auto* zextInst = dynamic_cast<ZextInst*>(context.inst);
    load_to_greg(zextInst->get_operand(0), Reg::t(0));
    append_inst("bstrpick.w $t0, $t0, 7, 0");
    store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::gen_call()
{
    auto* callInst = dynamic_cast<CallInst*>(context.inst);
    auto* functionType = static_cast<FunctionType*>(callInst->get_function_type());
    auto argsNum = functionType->get_num_of_args();
    unsigned int j = 0;
    unsigned int k = 0;
    for (unsigned int i = 0; i < argsNum; i++)
    {
        if (functionType->get_param_type(i)->is_float_type())
        {
            load_to_freg(callInst->get_operand(i + 1), FReg::fa(k));
            k++;
        }
        else
        {
            load_to_greg(callInst->get_operand(i + 1), Reg::a(j));
            j++;
        }
    }
    auto* func = dynamic_cast<Function*>(callInst->get_operand(0));
    append_inst("bl", {func->get_name()});
    auto retType = functionType->get_return_type();
    if (retType->is_integer_type())
    {
        store_from_greg(context.inst, Reg::a(0));
    }
    else if (retType->is_float_type())
    {
        store_from_freg(context.inst, FReg::fa(0));
    }
}

void CodeGen::gen_gep()
{
    auto* getElementPtrInst = dynamic_cast<GetElementPtrInst*>(context.inst);
    unsigned int num = getElementPtrInst->get_num_operand();
    load_to_greg(getElementPtrInst->get_operand(0), Reg::t(0));
    load_to_greg(getElementPtrInst->get_operand(num - 1), Reg::t(1));
    auto elementType = getElementPtrInst->get_element_type();
    append_inst("addi.d $t2, $zero, 4");
    if (elementType->is_float_type() || elementType->is_int32_type())
    {
        append_inst("mul.d $t1, $t1, $t2");
    }
    else
    {
        append_inst("addi.d $t2, $zero, 8");
        append_inst("mul.d $t1, $t1, $t2");
    }
    append_inst("add.d $t2, $t1, $t0");
    store_from_greg(context.inst, Reg::t(2));
    load_to_greg(ConstantInt::get(1, m), Reg::a(0));
    load_to_greg(ConstantInt::get(1, m), Reg::a(1));
    append_inst("bl add_lab4_flag");
}

void CodeGen::gen_sitofp()
{
    auto* sitofpInst = dynamic_cast<SiToFpInst*>(context.inst);
    load_to_greg(sitofpInst->get_operand(0), Reg::t(0));
    append_inst("movgr2fr.w $ft0, $t0");
    append_inst("ffint.s.w $ft1, $ft0");
    store_from_freg(context.inst, FReg::ft(1));
}

void CodeGen::gen_fptosi()
{
    auto* fptosiInst = dynamic_cast<FpToSiInst*>(context.inst);
    load_to_freg(fptosiInst->get_operand(0), FReg::ft(0));
    append_inst("ftintrz.w.s $ft1, $ft0");
    append_inst("movfr2gr.s $t0, $ft1");
    store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::run()
{
    m->set_print_name();
    if (!m->get_global_variable().empty())
    {
        append_inst("Global variables", ASMInstruction::Comment);
        append_inst(".text", ASMInstruction::Attribute);
        append_inst(".section", {".bss", "\"aw\"", "@nobits"},
                    ASMInstruction::Attribute);
        for (auto global : m->get_global_variable())
        {
            auto size = global->get_type()->get_pointer_element_type()->get_size();
            append_inst(".globl", {global->get_name()},
                        ASMInstruction::Attribute);
            append_inst(".type", {global->get_name(), "@object"},
                        ASMInstruction::Attribute);
            append_inst(".size", {global->get_name(), std::to_string(size)},
                        ASMInstruction::Attribute);
            append_inst(global->get_name(), ASMInstruction::Label);
            append_inst(".space", {std::to_string(size)},
                        ASMInstruction::Attribute);
        }
    }

    output.emplace_back(".text", ASMInstruction::Attribute);
    for (auto func : m->get_functions())
    {
        if (not func->is_declaration())
        {
            context.clear();
            context.func = func;

            append_inst(".globl", {func->get_name()}, ASMInstruction::Attribute);
            append_inst(".type", {func->get_name(), "@function"},
                        ASMInstruction::Attribute);
            append_inst(func->get_name(), ASMInstruction::Label);

            allocate();
            gen_prologue();

            for (auto& bb : func->get_basic_blocks())
            {
                context.bb = bb;
                append_inst(context.bb->get_name(), ASMInstruction::Label);
                for (auto& instr : bb->get_instructions())
                {
                    append_inst(instr->print(), ASMInstruction::Comment);
                    context.inst = instr;
                    switch (instr->get_instr_type())
                    {
                        case Instruction::ret:
                            gen_ret();
                            break;
                        case Instruction::br:
                            copy_stmt();
                            gen_br();
                            break;
                        case Instruction::add:
                        case Instruction::sub:
                        case Instruction::mul:
                        case Instruction::sdiv:
                            gen_binary();
                            break;
                        case Instruction::fadd:
                        case Instruction::fsub:
                        case Instruction::fmul:
                        case Instruction::fdiv:
                            gen_float_binary();
                            break;
                        case Instruction::alloca:
                            gen_alloca();
                            break;
                        case Instruction::load:
                            gen_load();
                            break;
                        case Instruction::store:
                            gen_store();
                            break;
                        case Instruction::ge:
                        case Instruction::gt:
                        case Instruction::le:
                        case Instruction::lt:
                        case Instruction::eq:
                        case Instruction::ne:
                            gen_icmp();
                            break;
                        case Instruction::fge:
                        case Instruction::fgt:
                        case Instruction::fle:
                        case Instruction::flt:
                        case Instruction::feq:
                        case Instruction::fne:
                            gen_fcmp();
                            break;
                        case Instruction::phi:
                            break;
                        case Instruction::call:
                            gen_call();
                            break;
                        case Instruction::getelementptr:
                            gen_gep();
                            break;
                        case Instruction::zext:
                            gen_zext();
                            break;
                        case Instruction::fptosi:
                            gen_fptosi();
                            break;
                        case Instruction::sitofp:
                            gen_sitofp();
                            break;
                    }
                }
            }
            gen_epilogue();
        }
    }
}

std::string CodeGen::print() const
{
    std::string result;
    for (const auto& inst : output)
    {
        result += inst.format();
    }
    return result;
}
