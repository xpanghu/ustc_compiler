#include "FuncInfo.hpp"

#include <deque>
#include <queue>

#include "Function.hpp"
#include "logging.hpp"

void FuncInfo::UseMessage::add(Value* val)
{
    auto g = dynamic_cast<GlobalVariable*>(val);
    if (g != nullptr)
    {
        globals_.emplace(g);
        return;
    }
    auto arg = dynamic_cast<Argument*>(val);
    arguments_.emplace(arg);
}

bool FuncInfo::UseMessage::have(Value* val) const
{
    auto g = dynamic_cast<GlobalVariable*>(val);
    if (g != nullptr) return globals_.count(g);
    return arguments_.count(dynamic_cast<Argument*>(val));
}

bool FuncInfo::UseMessage::empty() const
{
    return globals_.empty() && arguments_.empty();
}

void FuncInfo::run()
{
    std::unordered_map<Value*, Value*> val_to_var;
    // 计算对全局变量的 load/store
    for (auto glob : m_->get_global_variable())
        cal_val_2_var(glob, val_to_var);
    std::queue<Function*> worklist;
    std::unordered_set<Function*> in_worklist;
    // 计算对函数参数的 load/store
    // 本质上来说，对局部变量的 load/store 不会在函数外产生副作用，它也不会被用作 func_info 的信息
    for (auto func : m_->get_functions())
    {
        if (func->is_declaration()) continue;
        use_libs[func] = false;
        worklist.emplace(func);
        in_worklist.emplace(func);
        for (auto arg : func->get_args())
        {
            if (arg->get_type()->is_pointer_type())
            {
                cal_val_2_var(arg, val_to_var);
            }
        }
    }

    // 处理函数相互调用导致的隐式 load/store
    while (!worklist.empty())
    {
        // 被调用的函数
        auto calleeF = worklist.front();
        worklist.pop();
        in_worklist.erase(calleeF);
        for (auto& use : calleeF->get_use_list())
        {
            // 函数调用指令
            auto inst = dynamic_cast<CallInst*>(use.val_);
            // 调用 f 的函数
            auto callerF = inst->get_parent()->get_parent();
            auto& callerLoads = loads[callerF];
            auto& calleeLoads = loads[calleeF];
            auto& callerStores = stores[callerF];
            auto& calleeStores = stores[calleeF];
            // caller 的 load store 数量
            auto caller_old_load_store_count = callerLoads.globals_.size() + callerLoads.arguments_.size() +
                                               callerStores.globals_.size() + callerStores.arguments_.size();
            // caller 同时 load 了 callee load 的全局变量
            for (auto i : calleeLoads.globals_) callerLoads.globals_.emplace(i);
            // caller 同时 store 了 callee store 的全局变量
            for (auto i : calleeStores.globals_) callerStores.globals_.emplace(i);
            auto& ops = inst->get_operands();
            // 形式参数
            for (auto calleArg : calleeF->get_args())
            {
                if (calleArg->get_type()->is_pointer_type())
                {
                    // 传入的实参
                    auto trueArg = ops[calleArg->get_arg_no() + 1];
                    // 实参来自哪个临时变量
                    auto trueArgVar = val_to_var.find(trueArg);
                    // 来自局部变量，跳过，因为对局部变量的 load/store 不会在函数外产生副作用
                    if (trueArgVar == val_to_var.end()) continue;
                    auto trace = trueArgVar->second;
                    // 添加 load
                    if (calleeLoads.have(calleArg)) callerLoads.add(trace);
                    // 添加 store
                    if (calleeStores.have(calleArg)) callerStores.add(trace);
                }
            }
            // caller f 的 load/store 产生了变化，可能会影响其它调用 f 的函数的 load/store
            if (callerLoads.globals_.size() + callerLoads.arguments_.size() + callerStores.globals_.size() +
                callerStores.arguments_.size() != caller_old_load_store_count)
            {
                if (!in_worklist.count(callerF))
                {
                    in_worklist.emplace(callerF);
                    worklist.emplace(callerF);
                }
            }
        }
    }

    // 没有 load/store，但是调用了库函数的函数是非纯函数（因为库函数是 IO）
    for (auto& func : m_->get_functions())
    {
        if (func->is_declaration())
        {
            for (auto& use : func->get_use_list())
            {
                auto call = dynamic_cast<CallInst*>(use.val_);
                use_libs[call->get_parent()->get_parent()] = true;
            }
        }
        worklist.emplace(func);
        in_worklist.emplace(func);
    }
    while (!worklist.empty())
    {
        auto f = worklist.front();
        worklist.pop();
        in_worklist.erase(f);
        if (use_libs[f])
            for (auto& use : f->get_use_list())
            {
                auto inst = dynamic_cast<CallInst*>(use.val_);
                auto cf = inst->get_parent()->get_parent();
                if (!use_libs[cf])
                {
                    use_libs[cf] = true;
                    if (!in_worklist.count(cf))
                    {
                        in_worklist.emplace(cf);
                        worklist.emplace(cf);
                    }
                }
            }
    }
    log();
}

Value* FuncInfo::store_ptr(const StoreInst* st)
{
    return trace_ptr(st->get_operand(1));
}

Value* FuncInfo::load_ptr(const LoadInst* ld)
{
    return trace_ptr(ld->get_operand(0));
}

std::unordered_set<Value*> FuncInfo::get_stores(const CallInst* call)
{
    auto func = call->get_operand(0)->as<Function>();
    if (func->is_declaration()) return {};
    std::unordered_set<Value*> ret;
    for (auto i : stores[func].globals_) ret.emplace(i);
    for (auto arg : stores[func].arguments_)
    {
        int arg_no = static_cast<int>(arg->get_arg_no());
        auto in = call->get_operand(arg_no + 1);
        ret.emplace(trace_ptr(in));
    }
    return ret;
}

std::unordered_set<Value*> FuncInfo::get_loads(const CallInst* call)
{
    auto func = call->get_operand(0)->as<Function>();
    if (func->is_declaration()) return {};
    std::unordered_set<Value*> ret;
    for (auto i : loads[func].globals_) ret.emplace(i);
    for (auto arg : loads[func].arguments_)
    {
        int arg_no = static_cast<int>(arg->get_arg_no());
        auto in = call->get_operand(arg_no + 1);
        ret.emplace(trace_ptr(in));
    }
    return ret;
}

void FuncInfo::log() const
{
    for (auto it : use_libs)
    {
        LOG_INFO << it.first->get_name() << " is pure? " << it.second;
    }
}

void FuncInfo::cal_val_2_var(Value* var, std::unordered_map<Value*, Value*>& val_2_var)
{
    auto global = dynamic_cast<GlobalVariable*>(var);
    if (global != nullptr && global->is_const()) return;
    std::unordered_set<Value*> handled;
    std::queue<Value*> wait_to_handle;
    handled.emplace(var);
    val_2_var[var] = var;
    wait_to_handle.emplace(var);
    while (!wait_to_handle.empty())
    {
        Value* v = wait_to_handle.front();
        wait_to_handle.pop();
        for (auto& use : v->get_use_list())
        {
            auto inst = dynamic_cast<Instruction*>(use.val_);
            auto f = inst->get_parent()->get_parent();
            switch (inst->get_instr_type())
            {
                case Instruction::load:
                    {
                        loads[f].add(var);
                        break;
                    }
                case Instruction::store:
                    {
                        stores[f].add(var);
                        break;
                    }
                case Instruction::getelementptr:
                case Instruction::phi:
                    {
                        if (!handled.count(inst))
                        {
                            handled.emplace(inst);
                            val_2_var[inst] = var;
                            wait_to_handle.emplace(inst);
                        }
                        break;
                    }
                // case Instruction::call:
                // {
                // 如果遇到不能确定行为的库函数，需要指定其其对参数同时进行了读写，目前这里遇不到
                // auto callF = dynamic_cast<Function*>(inst->get_operand(0));
                // if (callF->is_declaration())
                // {
                //     stores[f].add(var);
                //     loads[f].add(var);
                // }
                // break;
                // }
                default:
                    break;
            }
        }
    }
}

Value* FuncInfo::trace_ptr(Value* val)
{
    assert(val != nullptr);
    if (dynamic_cast<GlobalVariable*>(val) != nullptr
        || dynamic_cast<Argument*>(val) != nullptr
        || dynamic_cast<AllocaInst*>(val) != nullptr
    )
        return val;
    auto inst = dynamic_cast<Instruction*>(val);
    assert(inst != nullptr);
    if (inst->is_gep()) return trace_ptr(inst->get_operand(0));
    // 这意味着栈里面存在指针，你需要运行 Mem2Reg；或者你给 trace_ptr 传入了非指针参数
    assert(!inst->is_load());
    assert(inst->is_alloca());
    return inst;
}
