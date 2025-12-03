#pragma once

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "IRBuilder.hpp"
#include "Module.hpp"
#include "Type.hpp"
#include "ast.hpp"

#include <map>
#include <memory>

class Scope {
  public:
    // enter a new scope
    void enter() { inner.emplace_back(); }

    // exit a scope
    void exit() { inner.pop_back(); }

    bool in_global() const { return inner.size() == 1; }

    // push a name to scope
    // return true if successful
    // return false if this name already exits
    bool push(const std::string& name, Value *val) {
        auto result = inner[inner.size() - 1].insert({name, val});
        return result.second;
    }

    Value *find(const std::string& name) {
        for (auto s = inner.rbegin(); s != inner.rend(); ++s) {
            auto iter = s->find(name);
            if (iter != s->end()) {
                return iter->second;
            }
        }

        // Name not found: handled here?
        assert(false && "Name not found in scope");
    }

  private:
    std::vector<std::map<std::string, Value *>> inner;
};

class CminusfBuilder : public ASTVisitor {
  public:
    CminusfBuilder() {
        module = new Module();
        builder = new IRBuilder(nullptr, module);
        auto *TyVoid = module->get_void_type();
        auto *TyInt32 = module->get_int32_type();
        auto *TyFloat = module->get_float_type();

        auto *input_type = FunctionType::get(TyInt32, {});
        auto *input_fun = Function::create(input_type, "input", module);

        std::vector<Type *> output_params;
        output_params.push_back(TyInt32);
        auto *output_type = FunctionType::get(TyVoid, output_params);
        auto *output_fun = Function::create(output_type, "output", module);

        std::vector<Type *> output_float_params;
        output_float_params.push_back(TyFloat);
        auto *output_float_type = FunctionType::get(TyVoid, output_float_params);
        auto *output_float_fun =
            Function::create(output_float_type, "outputFloat", module);

        scope.enter();
        scope.push("input", input_fun);
        scope.push("output", output_fun);
        scope.push("outputFloat", output_float_fun);
    }

    Module* getModule() const { return module; }

    ~CminusfBuilder() override { delete builder; }

  private:
    Value *visit(ASTProgram &) final;
    Value *visit(ASTNum &) final;
    Value *visit(ASTVarDeclaration &) final;
    Value *visit(ASTFunDeclaration &) final;
    Value *visit(ASTParam &) final;
    Value *visit(ASTCompoundStmt &) final;
    Value *visit(ASTExpressionStmt &) final;
    Value *visit(ASTSelectionStmt &) final;
    Value *visit(ASTIterationStmt &) final;
    Value *visit(ASTReturnStmt &) final;
    Value *visit(ASTAssignExpression &) final;
    Value *visit(ASTSimpleExpression &) final;
    Value *visit(ASTAdditiveExpression &) final;
    Value *visit(ASTVar &) final;
    Value *visit(ASTTerm &) final;
    Value *visit(ASTCall &) final;

    IRBuilder* builder;
    Scope scope;
    Module* module;

    struct {
        // whether require lvalue
        bool require_lvalue = false;
        // function that is being built
        Function *func = nullptr;
        // detect scope pre-enter (for elegance only)
        bool pre_enter_scope = false;
    } context;
};
