#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Function.hpp"
#include "IRBuilder.hpp"
#include "Module.hpp"
#include "Type.hpp"

#include <iostream>

#define CONST_INT(num) ConstantInt::get(num, module)

int main()
{
    auto module = new Module();
    auto builder = new IRBuilder(nullptr, module);
    Type* int_type = module->get_int32_type(); 
    auto fn_type = FunctionType::get(int_type, {});
    auto fn_main = Function::create(fn_type, "main", module);
    auto bb = BasicBlock::create(module, "bb", fn_main);
    builder->set_insert_point(bb);
    auto allocaA = builder->create_alloca(int_type);
    auto allocaI = builder->create_alloca(int_type);
    builder->create_store(CONST_INT(10), allocaA);
    builder->create_store(CONST_INT(0), allocaI);
    auto bb1 = BasicBlock::create(module, "bb1", fn_main);
    builder->create_br(bb1);
    builder->set_insert_point(bb1); 
    auto valueI1 = builder->create_load(allocaI); 
    auto bb2 = BasicBlock::create(module, "bb2", fn_main);
    auto bb3 = BasicBlock::create(module, "bb3", fn_main);
    auto condition = builder->create_icmp_lt(valueI1, CONST_INT(10));
    builder->create_cond_br(condition, bb2, bb3);
    builder->set_insert_point(bb2); 
    auto valueI2 = builder->create_load(allocaI);
    auto valueI3 = builder->create_iadd(valueI2, CONST_INT(1));
    builder->create_store(valueI3, allocaI);
    auto valueA = builder->create_load(allocaA);
    auto valueA1 = builder->create_iadd(valueA, valueI3);
    builder->create_store(valueA1, allocaA);
    builder->create_br(bb1);
    builder->set_insert_point(bb3);
    auto ret_value = builder->create_load(allocaA);
    builder->create_ret(ret_value); 

    std::cout << module->print();
    delete module;
}
