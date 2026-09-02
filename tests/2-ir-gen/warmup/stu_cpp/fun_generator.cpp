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
    Type* Int32Type = module->get_int32_type();
    auto calleFnTy = FunctionType::get(Int32Type, {Int32Type});
    auto calleeFn = Function::create(calleFnTy, "callee", module);
    auto callee_bb = BasicBlock::create(module, "entry", calleeFn);
    builder->set_insert_point(callee_bb);
    auto aAlloca = builder->create_alloca(Int32Type);
    std::vector<Value *> callee_args;
    for (auto &arg : calleeFn->get_args()) {
        callee_args.push_back(&arg);
    }
    builder->create_store(callee_args[0], aAlloca);
    auto aLoad = builder->create_load(aAlloca);
    auto mulRes = builder->create_imul(CONST_INT(2), aLoad);
    builder->create_ret(mulRes);

    auto mainFnTy = FunctionType::get(Int32Type, {});
    auto mainFn = Function::create(mainFnTy, "main", module);
    auto main_bb = BasicBlock::create(module, "entry", mainFn);
    builder->set_insert_point(main_bb);
    auto callRes = builder->create_call(calleeFn, {CONST_INT(110)});
    builder->create_ret(callRes);

    std::cout << module->print();
    delete module;
    return 0;
}