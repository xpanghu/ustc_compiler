#include "Instruction.hpp"
#include "BasicBlock.hpp"
#include "Function.hpp"
#include "IRprinter.hpp"
#include "Module.hpp"
#include "Type.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

Instruction::Instruction(Type *ty, OpID id, const std::string& name, BasicBlock *parent)
    : User(ty, ""), op_id_(id), parent_(parent) {
    assert(ty != nullptr && "Instruction have null type");
    assert(((!ty->is_void_type()) || name.empty()) && "Void Type Instruction should not have name");
    if (parent)
        parent->add_instruction(this);
    if (!ty->is_void_type() && parent && parent->get_parent())
        set_name(parent_->get_parent()->names4insts_.get_name(name));
}

Instruction::~Instruction() = default;

Function *Instruction::get_function() const { return parent_->get_parent(); }
Module *Instruction::get_module() const { return parent_->get_module(); }

std::string Instruction::get_instr_op_name() const {
    return print_instr_op_name(op_id_);
}

IBinaryInst::IBinaryInst(OpID id, Value *v1, Value *v2, BasicBlock *bb, const std::string& name)
    : Instruction(bb->get_module()->get_int32_type(), id, name, bb) {
    assert(v1->get_type()->is_int32_type() && v2->get_type()->is_int32_type() &&
           "IBinaryInst operands are not both i32");
    add_operand(v1);
    add_operand(v2);
}

IBinaryInst *IBinaryInst::create_add(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new IBinaryInst(add, v1, v2, bb, name);
}
IBinaryInst *IBinaryInst::create_sub(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new IBinaryInst(sub, v1, v2, bb, name);
}
IBinaryInst *IBinaryInst::create_mul(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new IBinaryInst(mul, v1, v2, bb, name);
}
IBinaryInst *IBinaryInst::create_sdiv(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new IBinaryInst(sdiv, v1, v2, bb, name);
}

FBinaryInst::FBinaryInst(OpID id, Value *v1, Value *v2, BasicBlock *bb, const std::string& name)
    : Instruction(bb->get_module()->get_float_type(), id, name, bb) {
    assert(v1->get_type()->is_float_type() && v2->get_type()->is_float_type() &&
           "FBinaryInst operands are not both float");
    add_operand(v1);
    add_operand(v2);
}

FBinaryInst *FBinaryInst::create_fadd(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FBinaryInst(fadd, v1, v2, bb, name);
}
FBinaryInst *FBinaryInst::create_fsub(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FBinaryInst(fsub, v1, v2, bb,name);
}
FBinaryInst *FBinaryInst::create_fmul(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FBinaryInst(fmul, v1, v2, bb,name);
}
FBinaryInst *FBinaryInst::create_fdiv(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FBinaryInst(fdiv, v1, v2, bb,name);
}

ICmpInst::ICmpInst(OpID id, Value *lhs, Value *rhs, BasicBlock *bb, const std::string& name)
    : Instruction(bb->get_module()->get_int1_type(), id,name, bb) {
    assert(lhs->get_type()->is_int32_type() &&
           rhs->get_type()->is_int32_type() &&
           "CmpInst operands are not both i32");
    add_operand(lhs);
    add_operand(rhs);
}

ICmpInst *ICmpInst::create_ge(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new ICmpInst(ge, v1, v2, bb, name);
}
ICmpInst *ICmpInst::create_gt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new ICmpInst(gt, v1, v2, bb, name);
}
ICmpInst *ICmpInst::create_le(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new ICmpInst(le, v1, v2, bb, name);
}
ICmpInst *ICmpInst::create_lt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new ICmpInst(lt, v1, v2, bb, name);
}
ICmpInst *ICmpInst::create_eq(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new ICmpInst(eq, v1, v2, bb, name);
}
ICmpInst *ICmpInst::create_ne(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new ICmpInst(ne, v1, v2, bb, name);
}

FCmpInst::FCmpInst(OpID id, Value *lhs, Value *rhs, BasicBlock *bb, const std::string& name)
    : Instruction(bb->get_module()->get_int1_type(), id, name, bb) {
    assert(lhs->get_type()->is_float_type() &&
           rhs->get_type()->is_float_type() &&
           "FCmpInst operands are not both float");
    add_operand(lhs);
    add_operand(rhs);
}

FCmpInst *FCmpInst::create_fge(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FCmpInst(fge, v1, v2, bb, name);
}
FCmpInst *FCmpInst::create_fgt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FCmpInst(fgt, v1, v2, bb, name);
}
FCmpInst *FCmpInst::create_fle(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FCmpInst(fle, v1, v2, bb, name);
}
FCmpInst *FCmpInst::create_flt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FCmpInst(flt, v1, v2, bb, name);
}
FCmpInst *FCmpInst::create_feq(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FCmpInst(feq, v1, v2, bb, name);
}
FCmpInst *FCmpInst::create_fne(Value *v1, Value *v2, BasicBlock *bb, const std::string& name) {
    return new FCmpInst(fne, v1, v2, bb, name);
}

CallInst::CallInst(Function *func, const std::vector<Value *>& args, BasicBlock *bb, const std::string& name)
    : Instruction(func->get_return_type(), call, name, bb) {
    assert(func->get_type()->is_function_type() && "Not a function");
    assert((func->get_num_of_args() == args.size()) && "Wrong number of args");
    add_operand(func);
    auto func_type = dynamic_cast<FunctionType *>(func->get_type());
    for (unsigned i = 0; i < args.size(); i++) {
        assert(func_type->get_param_type(i) == args[i]->get_type() &&
               "CallInst: Wrong arg type");
        add_operand(args[i]);
    }
}

CallInst *CallInst::create_call(Function *func, const std::vector<Value *>& args,
                                BasicBlock *bb, const std::string& name) {
    return new CallInst(func, args, bb, name);
}

FunctionType *CallInst::get_function_type() const {
    return dynamic_cast<FunctionType *>(get_operand(0)->get_type());
}

BranchInst::BranchInst(Value *cond, BasicBlock *if_true, BasicBlock *if_false,
                       BasicBlock *bb)
    : Instruction(bb->get_module()->get_void_type(), br, "", bb) {
    if (cond == nullptr) { // conditionless jump
        assert(if_false == nullptr && "Given false-bb on conditionless jump");
        add_operand(if_true);
        // prev/succ
        if_true->add_pre_basic_block(bb);
        bb->add_succ_basic_block(if_true);
    } else {
        assert(cond->get_type()->is_int1_type() &&
               "BranchInst condition is not i1");
        add_operand(cond);
        add_operand(if_true);
        add_operand(if_false);
        // prev/succ
        if_true->add_pre_basic_block(bb);
        if_false->add_pre_basic_block(bb);
        bb->add_succ_basic_block(if_true);
        bb->add_succ_basic_block(if_false);
    }
}

BranchInst::~BranchInst() {
    std::list<BasicBlock *> succs;
    if (is_cond_br()) {
        succs.push_back(dynamic_cast<BasicBlock *>(get_operand(1)));
        succs.push_back(dynamic_cast<BasicBlock *>(get_operand(2)));
    } else {
        succs.push_back(dynamic_cast<BasicBlock *>(get_operand(0)));
    }
    for (auto succ_bb : succs) {
        if (succ_bb) {
            succ_bb->remove_pre_basic_block(get_parent());
            get_parent()->remove_succ_basic_block(succ_bb);
        }
    }
}

BranchInst *BranchInst::create_cond_br(Value *cond, BasicBlock *if_true,
                                       BasicBlock *if_false, BasicBlock *bb) {
    return new BranchInst(cond, if_true, if_false, bb);
}

BranchInst *BranchInst::create_br(BasicBlock *if_true, BasicBlock *bb) {
    return new BranchInst(nullptr, if_true, nullptr, bb);
}

void BranchInst::replace_all_bb_match(BasicBlock* need_replace, BasicBlock* replace_to)
{
    if (need_replace == nullptr || replace_to == nullptr || need_replace == replace_to) return;
    int size = static_cast<int>(get_operands().size());
    for (int i = 0; i < size; i ++)
    {
        auto op = get_operand(i);
        if (op == need_replace) set_operand(i, replace_to);
    }
    auto parent = get_parent();
    if (parent == nullptr) return;
    parent->remove_succ_basic_block(need_replace);
    need_replace->remove_pre_basic_block(parent);
    parent->add_succ_basic_block(replace_to);
    replace_to->add_pre_basic_block(parent);
}

ReturnInst::ReturnInst(Value *val, BasicBlock *bb)
    : Instruction(bb->get_module()->get_void_type(), ret, "", bb) {
    if (val == nullptr) {
        assert(bb->get_parent()->get_return_type()->is_void_type());
    } else {
        assert(!bb->get_parent()->get_return_type()->is_void_type() &&
               "Void function returning a value");
        assert(bb->get_parent()->get_return_type() == val->get_type() &&
               "ReturnInst type is different from function return type");
        add_operand(val);
    }
}

ReturnInst *ReturnInst::create_ret(Value *val, BasicBlock *bb) {
    return new ReturnInst(val, bb);
}
ReturnInst *ReturnInst::create_void_ret(BasicBlock *bb) {
    return new ReturnInst(nullptr, bb);
}

bool ReturnInst::is_void_ret() const { return get_num_operand() == 0; }

GetElementPtrInst::GetElementPtrInst(Value *ptr, const std::vector<Value *>& idxs,
                                     BasicBlock *bb, const std::string& name)
    : Instruction(PointerType::get(get_element_type(ptr, idxs)),
                                  getelementptr, name, bb) {
    add_operand(ptr);
    for (auto idx : idxs)
    {
        assert(idx->get_type()->is_integer_type() && "Index is not integer");
        add_operand(idx);
    }
}

Type *GetElementPtrInst::get_element_type(const Value *ptr,
                                          const std::vector<Value *>& idxs, const std::string& name) {
    assert(ptr->get_type()->is_pointer_type() &&
           "GetElementPtrInst ptr is not a pointer");

    Type *ty = ptr->get_type()->get_pointer_element_type();
    assert(
        "GetElementPtrInst ptr is wrong type" &&
        (ty->is_array_type() || ty->is_integer_type() || ty->is_float_type()));
    if (ty->is_array_type()) {
        ArrayType *arr_ty = dynamic_cast<ArrayType *>(ty);
        for (unsigned i = 1; i < idxs.size(); i++) {
            ty = arr_ty->get_element_type();
            if (i < idxs.size() - 1) {
                assert(ty->is_array_type() && "Index error!");
            }
            if (ty->is_array_type()) {
                arr_ty = dynamic_cast<ArrayType *>(ty);
            }
        }
    }
    return ty;
}

Type *GetElementPtrInst::get_element_type() const {
    return get_type()->get_pointer_element_type();
}

GetElementPtrInst *GetElementPtrInst::create_gep(Value *ptr,
                                                 const std::vector<Value *>& idxs,
                                                 BasicBlock *bb, const std::string& name) {
    return new GetElementPtrInst(ptr, idxs, bb, name);
}

StoreInst::StoreInst(Value *val, Value *ptr, BasicBlock *bb)
    : Instruction(bb->get_module()->get_void_type(), store, "", bb) {
    assert((ptr->get_type()->get_pointer_element_type() == val->get_type()) &&
           "StoreInst ptr is not a pointer to val type");
    add_operand(val);
    add_operand(ptr);
}

StoreInst *StoreInst::create_store(Value *val, Value *ptr, BasicBlock *bb) {
    return new StoreInst(val, ptr, bb);
}

LoadInst::LoadInst(Value *ptr, BasicBlock *bb, const std::string& name)
    : Instruction(ptr->get_type()->get_pointer_element_type(), load, name,
                         bb) {
    assert((get_type()->is_integer_type() or get_type()->is_float_type() or
            get_type()->is_pointer_type()) &&
           "Should not load value with type except int/float");
    add_operand(ptr);
}

LoadInst *LoadInst::create_load(Value *ptr, BasicBlock *bb, const std::string& name) {
    return new LoadInst(ptr, bb, name);
}

AllocaInst::AllocaInst(Type *ty, BasicBlock *bb, const std::string& name)
    : Instruction(PointerType::get(ty), alloca, name, bb) {
    static constexpr std::array<Type::TypeID, 4> allowed_alloc_type = {
        Type::IntegerTyID, Type::FloatTyID, Type::ArrayTyID, Type::PointerTyID};
    assert(std::find(allowed_alloc_type.begin(), allowed_alloc_type.end(),
                     ty->get_type_id()) != allowed_alloc_type.end() &&
           "Not allowed type for alloca");
}

AllocaInst *AllocaInst::create_alloca(Type *ty, BasicBlock *bb, const std::string& name) {
    return new AllocaInst(ty, bb, name);
}

ZextInst::ZextInst(Value *val, Type *ty, BasicBlock *bb, const std::string& name)
    : Instruction(ty, zext, name, bb) {
    assert(val->get_type()->is_integer_type() &&
           "ZextInst operand is not integer");
    assert(ty->is_integer_type() && "ZextInst destination type is not integer");
    assert((dynamic_cast<IntegerType *>(val->get_type())->get_num_bits() <
            dynamic_cast<IntegerType *>(ty)->get_num_bits()) &&
           "ZextInst operand bit size is not smaller than destination type bit "
           "size");
    add_operand(val);
}

ZextInst *ZextInst::create_zext(Value *val, Type *ty, BasicBlock *bb, const std::string& name) {
    return new ZextInst(val, ty, bb, name);
}
ZextInst *ZextInst::create_zext_to_i32(Value *val, BasicBlock *bb, const std::string& name) {
    return new ZextInst(val, bb->get_module()->get_int32_type(), bb, name);
}

FpToSiInst::FpToSiInst(Value *val, Type *ty, BasicBlock *bb, const std::string& name)
    : Instruction(ty, fptosi, name, bb) {
    assert(val->get_type()->is_float_type() &&
           "FpToSiInst operand is not float");
    assert(ty->is_integer_type() &&
           "FpToSiInst destination type is not integer");
    add_operand(val);
}

FpToSiInst *FpToSiInst::create_fptosi(Value *val, Type *ty, BasicBlock *bb, const std::string& name) {
    return new FpToSiInst(val, ty, bb, name);
}
FpToSiInst *FpToSiInst::create_fptosi_to_i32(Value *val, BasicBlock *bb, const std::string& name) {
    return new FpToSiInst(val, bb->get_module()->get_int32_type(), bb, name);
}

SiToFpInst::SiToFpInst(Value *val, Type *ty, BasicBlock *bb, const std::string& name)
    : Instruction(ty, sitofp, name, bb) {
    assert(val->get_type()->is_integer_type() &&
           "SiToFpInst operand is not integer");
    assert(ty->is_float_type() && "SiToFpInst destination type is not float");
    add_operand(val);
}

SiToFpInst *SiToFpInst::create_sitofp(Value *val, BasicBlock *bb, const std::string& name) {
    return new SiToFpInst(val, bb->get_module()->get_float_type(), bb, name);
}

PhiInst::PhiInst(Type *ty, const std::vector<Value *>& vals,
                 const std::vector<BasicBlock *>& val_bbs, BasicBlock *bb, const std::string& name)
    : Instruction(ty, phi, name, bb) {
    assert(vals.size() == val_bbs.size() && "Unmatched vals and bbs");
    for (unsigned i = 0; i < vals.size(); i++) {
        assert(ty == vals[i]->get_type() && "Bad type for phi");
        add_operand(vals[i]);
        add_operand(val_bbs[i]);
    }
}

PhiInst *PhiInst::create_phi(Type *ty, BasicBlock *bb,
                             const std::vector<Value *>& vals,
                             const std::vector<BasicBlock *>& val_bbs, const std::string& name) {
    return new PhiInst(ty, vals, val_bbs, bb, name);
}

std::vector<std::pair<Value*, BasicBlock*>> PhiInst::get_phi_pairs() const
{
    std::vector<std::pair<Value*, BasicBlock*>> res;
    int ops = static_cast<int>(get_num_operand());
    for (int i = 0; i < ops; i += 2) {
        auto bb = dynamic_cast<BasicBlock*>(this->get_operand(i + 1));
        res.emplace_back(this->get_operand(i), bb);
    }
    return res;
}

Names GLOBAL_INSTRUCTION_NAMES_{"op", "_" };