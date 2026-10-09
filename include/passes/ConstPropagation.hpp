#ifndef CONSTPROPAGATION_HPP
#define CONSTPROPAGATION_HPP
#include "Constant.hpp"
#include "Instruction.hpp"
#include "Module.hpp"
#include "PassManager.hpp"
#include "Value.hpp"

ConstantFP *cast_constantfp(Value *value);
ConstantInt *cast_constantint(Value *value);

class ConstFolder {
public:
    ConstFolder(Module *m) : module_(m) {}
    // cminus only support binary operations
    ConstantInt *compute(Instruction::OpID op, ConstantInt *value1, ConstantInt *value2);
    Constant *compute(Instruction::OpID op, ConstantFP *value1, ConstantFP *value2);
    // int -> float
    ConstantFP *compute(Instruction::OpID op, ConstantInt *value1);
    // float -> int
    ConstantInt *compute(Instruction::OpID op, ConstantFP *value1);

private:
    Module *module_;
};

class ConstPropagation : public TransformPass {
public:
    ConstPropagation(Module *m) : TransformPass(m) {}
    void run() override;

private:
    Value *try_fold(Instruction *inst);
    bool simplify_branch(BasicBlock *bb);
    bool remove_unreachable_blocks(Function *func);
    ConstFolder folder{m_};
};

#endif
