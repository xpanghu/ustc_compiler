#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Function.hpp"
#include "IRBuilder.hpp"
#include "Module.hpp"
#include "Type.hpp"

#include <iostream>

#define CONST_INT(num) ConstantInt::get(num, module)
#define CONST_FP(num) ConstantFP::get(num, module)

int main()
{
    auto module = new Module(); 
    auto builder = new IRBuilder(nullptr, module);
    Type* float_type = module->get_float_type();
    Type* int_type = module->get_int32_type();
    auto fn_type = FunctionType::get(int_type, {});
    auto mainFn = Function::create(fn_type, "main", module);
    auto bba = BasicBlock::create(module, "BBA", mainFn);
    auto bbb = BasicBlock::create(module, "BBB", mainFn);
    auto bbc = BasicBlock::create(module, "BBC", mainFn);
    builder->set_insert_point(bba);
    auto allocaA= builder->create_alloca(float_type);
    builder->create_store(CONST_FP(5.555), allocaA);
    auto valueA = builder->create_load(allocaA);
    auto condition = builder->create_fcmp_gt(valueA, CONST_FP(1));
    builder->create_cond_br(condition, bbb,  bbc);
    builder->set_insert_point(bbb);
    builder->create_ret(CONST_INT(233));
    builder->set_insert_point(bbc);
    builder->create_ret(CONST_INT(0));
    std::cout << module->print();
    delete module;
}