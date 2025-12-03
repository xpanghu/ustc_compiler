#pragma once

#include <map>

#include "Dominators.hpp"
#include "Instruction.hpp"
#include "Value.hpp"

class Mem2Reg : public TransformPass {
  private:
    // 当前函数
    Function *func_;
    // 当前函数对应的支配树
    Dominators* dominators_;
    // 所有需要处理的变量
    std::list<AllocaInst*> allocas_;
    // 变量定值栈
    std::map<AllocaInst*, std::vector<Value *>> var_val_stack;
    // Phi 对应的局部变量
    std::map<PhiInst *, AllocaInst*> phi_to_alloca_;
    // 在某个基本块的 Phi
    std::map<BasicBlock*, std::list<PhiInst*>> bb_to_phi_;
  public:
    Mem2Reg(Module *m) : TransformPass(m), func_(nullptr), dominators_(nullptr) {}
    ~Mem2Reg() override = default;

    void run() override;

    void generate_phi();
    void rename(BasicBlock *bb);
};
