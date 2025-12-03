#pragma once

#include "Names.hpp"
#include "Type.hpp"
#include "User.hpp"

class BasicBlock;
class Function;

class Instruction : public User {
  public:
    enum OpID : uint8_t {
        // Terminator Instructions
        ret,
        br,
        // Standard binary operators
        add,
        sub,
        mul,
        sdiv,
        // float binary operators
        fadd,
        fsub,
        fmul,
        fdiv,
        // Memory operators
        alloca,
        load,
        store,
        // Int compare operators
        ge,
        gt,
        le,
        lt,
        eq,
        ne,
        // Float compare operators
        fge,
        fgt,
        fle,
        flt,
        feq,
        fne,
        // Other operators
        phi,
        call,
        getelementptr,
        zext, // zero extend
        fptosi,
        sitofp
        // float binary operators Logical operators

    };
    /* @parent: if parent!=nullptr, auto insert to bb
     * @ty: result type */
    Instruction(Type *ty, OpID id, const std::string& name, BasicBlock *parent = nullptr);
    ~Instruction() override;

    Instruction(const Instruction& other) = delete;
    Instruction(Instruction&& other) noexcept = delete;
    Instruction& operator=(const Instruction& other) = delete;
    Instruction& operator=(Instruction&& other) noexcept = delete;

    BasicBlock *get_parent() { return parent_; }
    const BasicBlock *get_parent() const { return parent_; }
    void set_parent(BasicBlock *parent) { this->parent_ = parent; }

    // Return the function this instruction belongs to.
    Function *get_function() const;
    Module *get_module() const;

    OpID get_instr_type() const { return op_id_; }
    std::string get_instr_op_name() const;

    bool is_void() const
    {
        return ((op_id_ == ret) || (op_id_ == br) || (op_id_ == store) ||
                (op_id_ == call && this->get_type()->is_void_type()));
    }

    bool is_phi() const { return op_id_ == phi; }
    bool is_store() const { return op_id_ == store; }
    bool is_alloca() const { return op_id_ == alloca; }
    bool is_ret() const { return op_id_ == ret; }
    bool is_load() const { return op_id_ == load; }
    bool is_br() const { return op_id_ == br; }

    bool is_add() const { return op_id_ == add; }
    bool is_sub() const { return op_id_ == sub; }
    bool is_mul() const { return op_id_ == mul; }
    bool is_div() const { return op_id_ == sdiv; }

    bool is_fadd() const { return op_id_ == fadd; }
    bool is_fsub() const { return op_id_ == fsub; }
    bool is_fmul() const { return op_id_ == fmul; }
    bool is_fdiv() const { return op_id_ == fdiv; }
    bool is_fp2si() const { return op_id_ == fptosi; }
    bool is_si2fp() const { return op_id_ == sitofp; }

    bool is_cmp() const { return ge <= op_id_ and op_id_ <= ne; }
    bool is_fcmp() const { return fge <= op_id_ and op_id_ <= fne; }

    bool is_call() const { return op_id_ == call; }
    bool is_gep() const { return op_id_ == getelementptr; }
    bool is_zext() const { return op_id_ == zext; }

    bool isBinary() const {
        return (is_add() || is_sub() || is_mul() || is_div() || is_fadd() ||
                is_fsub() || is_fmul() || is_fdiv()) &&
               (get_num_operand() == 2);
    }

    bool isTerminator() const { return is_br() || is_ret(); }

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;
  private:
    OpID op_id_;
    BasicBlock *parent_;
};

class IBinaryInst : public Instruction {

  private:
    IBinaryInst(OpID id, Value *v1, Value *v2, BasicBlock *bb, const std::string& name);

  public:
    static IBinaryInst *create_add(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static IBinaryInst *create_sub(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static IBinaryInst *create_mul(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static IBinaryInst *create_sdiv(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");

    std::string print() override;
};

class FBinaryInst : public Instruction {

  private:
    FBinaryInst(OpID id, Value *v1, Value *v2, BasicBlock *bb, const std::string& name);

  public:
    static FBinaryInst *create_fadd(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FBinaryInst *create_fsub(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FBinaryInst *create_fmul(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FBinaryInst *create_fdiv(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");

    std::string print() override;
};

class ICmpInst : public Instruction {

  private:
    ICmpInst(OpID id, Value *lhs, Value *rhs, BasicBlock *bb, const std::string& name);

  public:

    static ICmpInst *create_ge(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static ICmpInst *create_gt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static ICmpInst *create_le(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static ICmpInst *create_lt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static ICmpInst *create_eq(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static ICmpInst *create_ne(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");

    std::string print() override;
};

class FCmpInst : public Instruction {

  private:
    FCmpInst(OpID id, Value *lhs, Value *rhs, BasicBlock *bb, const std::string& name);

  public:
    static FCmpInst *create_fge(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FCmpInst *create_fgt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FCmpInst *create_fle(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FCmpInst *create_flt(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FCmpInst *create_feq(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");
    static FCmpInst *create_fne(Value *v1, Value *v2, BasicBlock *bb, const std::string& name = "");

    std::string print() override;
};

class CallInst : public Instruction {

  protected:
    CallInst(Function *func, const std::vector<Value *>& args, BasicBlock *bb, const std::string& name);

  public:
    static CallInst *create_call(Function *func, const std::vector<Value *>& args,
                                 BasicBlock *bb, const std::string& name = "");
    FunctionType *get_function_type() const;

    std::string print() override;
};

class BranchInst : public Instruction {

  private:
    BranchInst(Value *cond, BasicBlock *if_true, BasicBlock *if_false,
               BasicBlock *bb);

public:
    ~BranchInst() override;
    BranchInst(const BranchInst& other) = delete;
    BranchInst(BranchInst&& other) noexcept = delete;
    BranchInst& operator=(const BranchInst& other) = delete;
    BranchInst& operator=(BranchInst&& other) noexcept = delete;

    static BranchInst *create_cond_br(Value *cond, BasicBlock *if_true,
                                      BasicBlock *if_false, BasicBlock *bb);
    static BranchInst *create_br(BasicBlock *if_true, BasicBlock *bb);

    bool is_cond_br() const { return get_num_operand() == 3; }

    Value *get_condition() const { return get_operand(0); }

    void replace_all_bb_match(BasicBlock* need_replace, BasicBlock* replace_to);

    std::string print() override;
};

class ReturnInst : public Instruction {

  private:
    ReturnInst(Value *val, BasicBlock *bb);

  public:
    static ReturnInst *create_ret(Value *val, BasicBlock *bb);
    static ReturnInst *create_void_ret(BasicBlock *bb);
    bool is_void_ret() const;

    std::string print() override;
};

class GetElementPtrInst : public Instruction {

  private:
    GetElementPtrInst(Value *ptr, const std::vector<Value *>& idxs, BasicBlock *bb, const std::string& name);

  public:
    static Type *get_element_type(const Value *ptr, const std::vector<Value *>& idxs, const std::string& name = "");
    static GetElementPtrInst *create_gep(Value *ptr, const std::vector<Value *>& idxs,
                                         BasicBlock *bb, const std::string& name = "");
    Type *get_element_type() const;

    std::string print() override;
};

class StoreInst : public Instruction {

    StoreInst(Value *val, Value *ptr, BasicBlock *bb);

  public:
    static StoreInst *create_store(Value *val, Value *ptr, BasicBlock *bb);

    Value *get_val() const { return this->get_operand(0); }
    Value *get_ptr() const { return this->get_operand(1); }

    std::string print() override;
};

class LoadInst : public Instruction {

    LoadInst(Value *ptr, BasicBlock *bb, const std::string& name);

  public:
    static LoadInst *create_load(Value *ptr, BasicBlock *bb, const std::string& name = "");

    Value *get_ptr() const { return this->get_operand(0); }
    Type *get_load_type() const { return get_type(); }

    std::string print() override;
};

class AllocaInst : public Instruction {

    AllocaInst(Type *ty, BasicBlock *bb, const std::string& name);

  public:
    // 新特性：指令表分为 {alloca, phi | other inst} 两段，创建和向基本块插入 alloca 和 phi，都只会插在第一段，它们在常规指令前面
    static AllocaInst *create_alloca(Type *ty, BasicBlock *bb, const std::string& name = "");
    
    Type *get_alloca_type() const {
        return get_type()->get_pointer_element_type();
    }

    std::string print() override;
};

class ZextInst : public Instruction {

  private:
    ZextInst(Value *val, Type *ty, BasicBlock *bb, const std::string& name);

  public:
    static ZextInst *create_zext(Value *val, Type *ty, BasicBlock *bb, const std::string& name = "");
    static ZextInst *create_zext_to_i32(Value *val, BasicBlock *bb, const std::string& name = "");

    Type *get_dest_type() const { return get_type(); }

    std::string print() override;
};

class FpToSiInst : public Instruction {

  private:
    FpToSiInst(Value *val, Type *ty, BasicBlock *bb, const std::string& name);

  public:
    static FpToSiInst *create_fptosi(Value *val, Type *ty, BasicBlock *bb, const std::string& name = "");
    static FpToSiInst *create_fptosi_to_i32(Value *val, BasicBlock *bb, const std::string& name = "");

    Type *get_dest_type() const { return get_type(); }

    std::string print() override;
};

class SiToFpInst : public Instruction {

  private:
    SiToFpInst(Value *val, Type *ty, BasicBlock *bb, const std::string& name);

  public:
    static SiToFpInst *create_sitofp(Value *val, BasicBlock *bb, const std::string& name = "");

    Type *get_dest_type() const { return get_type(); }

    std::string print() override;
};

class PhiInst : public Instruction {

  private:
    PhiInst(Type *ty, const std::vector<Value *>& vals,
            const std::vector<BasicBlock *>& val_bbs, BasicBlock *bb, const std::string& name);

  public:
    // 新特性：指令表分为 {alloca, phi | other inst} 两段，创建和向基本块插入 alloca 和 phi，都只会插在第一段，它们在常规指令前面
    static PhiInst *create_phi(Type *ty, BasicBlock *bb,
                               const std::vector<Value *>& vals = {},
                               const std::vector<BasicBlock *>& val_bbs = {}, const std::string& name = "");

    void add_phi_pair_operand(Value *val, Value *pre_bb) {
        this->add_operand(val);
        this->add_operand(pre_bb);
    }
    std::vector<std::pair<Value*, BasicBlock*>> get_phi_pairs() const;

    std::string print() override;
};

extern Names GLOBAL_INSTRUCTION_NAMES_;