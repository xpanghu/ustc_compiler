#include "cminusf_builder.hpp"
#include "Constant.hpp"
#include "Type.hpp"

#define CONST_FP(num) ConstantFP::get((float)num, module.get())
#define CONST_INT(num) ConstantInt::get(num, module.get())

// types
Type* VOID_T;
Type* INT1_T;
Type* INT32_T;
Type* INT32PTR_T;
Type* FLOAT_T;
Type* FLOATPTR_T;

// op ids for arith()
enum {
    OP_ARITH_ADD,
    OP_ARITH_SUB,
    OP_ARITH_MUL,
    OP_ARITH_DIV
};

/*
 * use CMinusfBuilder::Scope to construct scopes
 * scope.enter: enter a new scope
 * scope.exit: exit current scope
 * scope.push: add a new binding to current scope
 * scope.find: find and return the value bound to the name
 */

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

BasicBlock* CminusfBuilder::new_block()
{
    return BasicBlock::create(module.get(), "", context.func);
}

// i1 -> i32, other types unchanged.
Value* CminusfBuilder::unify_int(Value* val)
{
    if (val->get_type()->is_int1_type())
        return builder->create_zext(val, module->get_int32_type());
    return val;
}

// Cast any scalar value (i1 / i32 / float) to an i32 value.
Value* CminusfBuilder::to_int32(Value* val)
{
    Type* ty = val->get_type();
    if (ty->is_int32_type())
        return val;
    if (ty->is_int1_type())
        return builder->create_zext(val, module->get_int32_type());
    // float
    return builder->create_fptosi(val, module->get_int32_type());
}

// Cast any scalar value (i1 / i32 / float) to a float value.
Value* CminusfBuilder::to_float(Value* val)
{
    Type* ty = val->get_type();
    if (ty->is_float_type())
        return val;
    if (ty->is_int1_type())
        val = builder->create_zext(val, module->get_int32_type());
    return builder->create_sitofp(val, module->get_float_type());
}

// Turn a scalar value into an i1 used as a branch condition.
Value* CminusfBuilder::to_cond(Value* val)
{
    Type* ty = val->get_type();
    if (ty->is_int1_type())
        return val;
    if (ty->is_float_type())
        return builder->create_fcmp_ne(val, CONST_FP(0));
    return builder->create_icmp_ne(val, CONST_INT(0));
}

// Cast a scalar value to an integer / float target type.
Value* CminusfBuilder::cast_value(Value* val, Type* target)
{
    if (target->is_float_type())
        return to_float(val);
    return to_int32(val);
}

// Integer or float arithmetic with implicit int->float promotion.
Value* CminusfBuilder::arith(Value* lhs, Value* rhs, int op)
{
    lhs = unify_int(lhs);
    rhs = unify_int(rhs);
    if (lhs->get_type()->is_float_type() || rhs->get_type()->is_float_type()) {
        lhs = to_float(lhs);
        rhs = to_float(rhs);
        switch (op) {
        case OP_ARITH_ADD:
            return builder->create_fadd(lhs, rhs);
        case OP_ARITH_SUB:
            return builder->create_fsub(lhs, rhs);
        case OP_ARITH_MUL:
            return builder->create_fmul(lhs, rhs);
        default:
            return builder->create_fdiv(lhs, rhs);
        }
    }
    switch (op) {
    case OP_ARITH_ADD:
        return builder->create_iadd(lhs, rhs);
    case OP_ARITH_SUB:
        return builder->create_isub(lhs, rhs);
    case OP_ARITH_MUL:
        return builder->create_imul(lhs, rhs);
    default:
        return builder->create_isdiv(lhs, rhs);
    }
}

// Relational / equality compare; promotes int operands to float when needed.
Value* CminusfBuilder::compare(Value* lhs, Value* rhs, RelOp op)
{
    lhs = unify_int(lhs);
    rhs = unify_int(rhs);
    if (lhs->get_type()->is_float_type() || rhs->get_type()->is_float_type()) {
        lhs = to_float(lhs);
        rhs = to_float(rhs);
        switch (op) {
        case OP_LE:
            return builder->create_fcmp_le(lhs, rhs);
        case OP_LT:
            return builder->create_fcmp_lt(lhs, rhs);
        case OP_GT:
            return builder->create_fcmp_gt(lhs, rhs);
        case OP_GE:
            return builder->create_fcmp_ge(lhs, rhs);
        case OP_EQ:
            return builder->create_fcmp_eq(lhs, rhs);
        default:
            return builder->create_fcmp_ne(lhs, rhs);
        }
    }
    switch (op) {
    case OP_LE:
        return builder->create_icmp_le(lhs, rhs);
    case OP_LT:
        return builder->create_icmp_lt(lhs, rhs);
    case OP_GT:
        return builder->create_icmp_gt(lhs, rhs);
    case OP_GE:
        return builder->create_icmp_ge(lhs, rhs);
    case OP_EQ:
        return builder->create_icmp_eq(lhs, rhs);
    default:
        return builder->create_icmp_ne(lhs, rhs);
    }
}

// The storage address of a scalar variable, or of one array element.
// For a whole (unsubscripted) array this is only meaningful when used as a
// function argument (handled by array_decay), never as a value.
Value* CminusfBuilder::var_addr(ASTVar& node)
{
    Value* base = scope.find(node.id);
    Type* pointee = base->get_type()->get_pointer_element_type();

    if (pointee->is_array_type()) {
        // local / global array variable
        if (node.expression == nullptr)
            return base;
        Value* idx = to_int32(node.expression->accept(*this));
        idx = checked_index(idx);
        return builder->create_gep(base, {CONST_INT(0), idx});
    }
    if (pointee->is_pointer_type()) {
        // array parameter (bound to an alloca that holds the array pointer)
        Value* arr_ptr = builder->create_load(base);
        if (node.expression == nullptr)
            return arr_ptr;
        Value* idx = to_int32(node.expression->accept(*this));
        idx = checked_index(idx);
        return builder->create_gep(arr_ptr, {idx});
    }
    // scalar variable
    return base;
}

// Element-0 pointer of an array variable, used when the array is passed as a
// function argument (array-to-pointer decay).
Value* CminusfBuilder::array_decay(ASTVar& node)
{
    Value* base = scope.find(node.id);
    Type* pointee = base->get_type()->get_pointer_element_type();
    if (pointee->is_array_type())
        return builder->create_gep(base, {CONST_INT(0), CONST_INT(0)});
    // array parameter
    return builder->create_load(base);
}

// Emit a runtime check that calls neg_idx_except when idx < 0.
Value* CminusfBuilder::checked_index(Value* idx)
{
    auto negBB = new_block();
    auto okBB = new_block();
    Value* is_neg = builder->create_icmp_lt(idx, CONST_INT(0));
    builder->create_cond_br(is_neg, negBB, okBB);

    builder->set_insert_point(negBB);
    builder->create_call(scope.find("neg_idx_except"), {});
    builder->create_br(okBB);

    builder->set_insert_point(okBB);
    return idx;
}

/* ------------------------------------------------------------------------- */
/* ASTVisitor overrides                                                      */
/* ------------------------------------------------------------------------- */

Value* CminusfBuilder::visit(ASTProgram& node)
{
    VOID_T = module->get_void_type();
    INT1_T = module->get_int1_type();
    INT32_T = module->get_int32_type();
    INT32PTR_T = module->get_int32_ptr_type();
    FLOAT_T = module->get_float_type();
    FLOATPTR_T = module->get_float_ptr_type();

    Value* ret_val = nullptr;
    for (auto& decl : node.declarations) {
        ret_val = decl->accept(*this);
    }
    return ret_val;
}

Value* CminusfBuilder::visit(ASTNum& node)
{
    if (node.type == CminusType::TYPE_INT) {
        return CONST_INT(node.i_val);
    } else if (node.type == CminusType::TYPE_FLOAT) {
        return CONST_FP(node.f_val);
    }
    return nullptr;
}

Value* CminusfBuilder::visit(ASTVarDeclaration& node)
{
    Type* elem_ty;
    if (node.type == TYPE_INT)
        elem_ty = module->get_int32_type();
    else if (node.type == TYPE_FLOAT) 
        elem_ty = module->get_float_type();
    
    Type* decl_ty = (node.num == nullptr) ? elem_ty : module->get_array_type(elem_ty, node.num->i_val);

    if (scope.in_global()) {
        auto gv = GlobalVariable::create(
            node.id, module.get(), decl_ty, false,
            ConstantZero::get(decl_ty, module.get()));
        scope.push(node.id, gv);
    } else {
        // hoist allocas to the function entry so that allocations inside
        // loops do not grow the runtime stack on every iteration
        auto cell = AllocaInst::create_alloca_begin(decl_ty, context.block);
        scope.push(node.id, cell);
    }
    return nullptr;
}

Value* CminusfBuilder::visit(ASTFunDeclaration& node)
{
    FunctionType* fun_type;
    Type* ret_type;
    std::vector<Type*> param_types;
    if (node.type == TYPE_INT)
        ret_type = module->get_int32_type();
    else if (node.type == TYPE_FLOAT)
        ret_type = module->get_float_type();
    else
        ret_type = module->get_void_type();

    for (auto& param : node.params) {
        Type* elem_ty;
        if (param->type == TYPE_FLOAT)
            elem_ty = module->get_float_type();
        else
            elem_ty = module->get_int32_type();
        if (param->isarray)
            param_types.push_back(module->get_pointer_type(elem_ty));
        else
            param_types.push_back(elem_ty);
    }

    fun_type = FunctionType::get(ret_type, param_types);
    auto func = Function::create(fun_type, node.id, module.get());
    scope.push(node.id, func);
    context.func = func;

    auto entry = BasicBlock::create(module.get(), "entry", func);
    context.block = entry;
    builder->set_insert_point(entry);

    scope.enter();
    std::vector<Value*> args;
    for (auto& arg : func->get_args()) {
        args.push_back(&arg);
    }
    for (unsigned int i = 0; i < node.params.size(); ++i) {
        auto& param = node.params[i];
        Type* elem_ty;
        if (param->type == TYPE_FLOAT)
            elem_ty = module->get_float_type();
        else
            elem_ty = module->get_int32_type();
        if (param->isarray) {
            // store the incoming array pointer in an alloca
            auto cell = builder->create_alloca(module->get_pointer_type(elem_ty));
            builder->create_store(args[i], cell);
            scope.push(param->id, cell);
        } else {
            auto cell = builder->create_alloca(elem_ty);
            builder->create_store(args[i], cell);
            scope.push(param->id, cell);
        }
    }

    node.compound_stmt->accept(*this);
    if (not builder->get_insert_block()->is_terminated()) {
        if (context.func->get_return_type()->is_void_type())
            builder->create_void_ret();
        else if (context.func->get_return_type()->is_float_type())
            builder->create_ret(CONST_FP(0.));
        else
            builder->create_ret(CONST_INT(0));
    }
    scope.exit();
    return nullptr;
}

Value* CminusfBuilder::visit(ASTParam& node)
{
    // parameters are handled directly in visit(ASTFunDeclaration)
    return nullptr;
}

Value* CminusfBuilder::visit(ASTCompoundStmt& node)
{
    // a new block opens a new scope (shadowing is legal, see scope.cminus)
    scope.enter();

    for (auto& decl : node.local_declarations) {
        decl->accept(*this);
    }

    for (auto& stmt : node.statement_list) {
        stmt->accept(*this);
        if (builder->get_insert_block()->is_terminated())
            break;
    }

    scope.exit();
    return nullptr;
}

Value* CminusfBuilder::visit(ASTExpressionStmt& node)
{
    if (node.expression != nullptr)
        node.expression->accept(*this);
    return nullptr;
}

Value* CminusfBuilder::visit(ASTSelectionStmt& node)
{
    Value* cond = to_cond(node.expression->accept(*this));
    auto thenBB = new_block();

    if (node.else_statement != nullptr) {
        auto elseBB = new_block();
        auto endBB = new_block();
        builder->create_cond_br(cond, thenBB, elseBB);

        builder->set_insert_point(thenBB);
        node.if_statement->accept(*this);
        if (not builder->get_insert_block()->is_terminated())
            builder->create_br(endBB);

        builder->set_insert_point(elseBB);
        node.else_statement->accept(*this);
        if (not builder->get_insert_block()->is_terminated())
            builder->create_br(endBB);

        builder->set_insert_point(endBB);
    } else {
        auto endBB = new_block();
        builder->create_cond_br(cond, thenBB, endBB);

        builder->set_insert_point(thenBB);
        node.if_statement->accept(*this);
        if (not builder->get_insert_block()->is_terminated())
            builder->create_br(endBB);

        builder->set_insert_point(endBB);
    }
    return nullptr;
}

Value* CminusfBuilder::visit(ASTIterationStmt& node)
{
    auto condBB = new_block();
    auto bodyBB = new_block();
    auto endBB = new_block();

    builder->create_br(condBB);

    builder->set_insert_point(condBB);
    Value* cond = to_cond(node.expression->accept(*this));
    builder->create_cond_br(cond, bodyBB, endBB);

    builder->set_insert_point(bodyBB);
    node.statement->accept(*this);
    if (not builder->get_insert_block()->is_terminated())
        builder->create_br(condBB);

    builder->set_insert_point(endBB);
    return nullptr;
}

Value* CminusfBuilder::visit(ASTReturnStmt& node)
{
    if (node.expression == nullptr) {
        builder->create_void_ret();
        return nullptr;
    }

    Value* val = node.expression->accept(*this);
    Type* ret_type = context.func->get_return_type();
    if (ret_type->is_void_type()) {
        builder->create_void_ret();
    } else if (ret_type->is_float_type()) {
        builder->create_ret(to_float(val));
    } else {
        builder->create_ret(to_int32(val));
    }
    return nullptr;
}

Value* CminusfBuilder::visit(ASTVar& node)
{
    Value* base = scope.find(node.id);
    Type* pointee = base->get_type()->get_pointer_element_type();
    bool is_array_var = node.expression == nullptr && (pointee->is_array_type() || pointee->is_pointer_type());
    if (is_array_var) {
        // a whole array used as a value (only valid as a function argument)
        // decays to a pointer to its first element
        return array_decay(node);
    }
    // a scalar variable, or an array element, used as a value
    return builder->create_load(var_addr(node));
}

Value* CminusfBuilder::visit(ASTAssignExpression& node)
{
    Value* addr = var_addr(*node.var);
    Value* val = node.expression->accept(*this);
    Type* elem_ty = addr->get_type()->get_pointer_element_type();
    val = cast_value(val, elem_ty);
    builder->create_store(val, addr);
    // returning the stored value supports chains: a = b = c = 3;
    return val;
}

Value* CminusfBuilder::visit(ASTSimpleExpression& node)
{
    if (node.additive_expression_r == nullptr)
        return node.additive_expression_l->accept(*this);

    Value* lhs = node.additive_expression_l->accept(*this);
    Value* rhs = node.additive_expression_r->accept(*this);
    return compare(lhs, rhs, node.op);
}

Value* CminusfBuilder::visit(ASTAdditiveExpression& node)
{
    if (node.additive_expression != nullptr) {
        Value* lhs = node.additive_expression->accept(*this);
        Value* rhs = node.term->accept(*this);
        return arith(lhs, rhs,
                     node.op == OP_PLUS ? OP_ARITH_ADD : OP_ARITH_SUB);
    }
    return node.term->accept(*this);
}

Value* CminusfBuilder::visit(ASTTerm& node)
{
    if (node.term != nullptr) {
        Value* lhs = node.term->accept(*this);
        Value* rhs = node.factor->accept(*this);
        return arith(lhs, rhs,
                     node.op == OP_MUL ? OP_ARITH_MUL : OP_ARITH_DIV);
    }
    return node.factor->accept(*this);
}

Value* CminusfBuilder::visit(ASTCall& node)
{
    auto callee = static_cast<Function*>(scope.find(node.id));
    auto fty = callee->get_function_type();

    std::vector<Value*> call_args;
    for (unsigned i = 0; i < node.args.size(); ++i) {
        Type* param_ty = fty->get_param_type(i);
        // whole arrays are turned into element pointers by visit(ASTVar)
        Value* arg_val = node.args[i]->accept(*this);
        if (param_ty->is_pointer_type())
            call_args.push_back(arg_val);
        else
            call_args.push_back(cast_value(arg_val, param_ty));
    }
    return builder->create_call(callee, call_args);
}
