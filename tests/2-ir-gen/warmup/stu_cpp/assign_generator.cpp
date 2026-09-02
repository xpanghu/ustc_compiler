#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Function.hpp"
#include "GlobalVariable.hpp"
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
    Type* Int32Type = module->get_int32_type();
    auto* arrayType = ArrayType::get(Int32Type, 10);
    auto assignFnTy = FunctionType::get(Int32Type, {});
    auto main_fun = Function::create(assignFnTy, "main", module);
    auto bb = BasicBlock::create(module, "entry", main_fun);
    builder->set_insert_point(bb);
    auto a_array = builder->create_alloca(arrayType);
    auto a0 = builder->create_gep(a_array, {CONST_INT(0), CONST_INT(0)});
    builder->create_store(CONST_INT(10), a0);
    auto a1 = builder->create_gep(a_array, {CONST_INT(0), CONST_INT(1)});
    auto a0_value = builder->create_load(a0);
    auto mul_a0 = builder->create_imul(a0_value, CONST_INT(2));
    builder->create_store(mul_a0, a1);
    auto ret_value = builder->create_load(a1);
    builder->create_ret(ret_value);
    std::cout << module->print();
    delete module;
    return 0;
}