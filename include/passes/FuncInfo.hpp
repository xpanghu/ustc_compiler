#pragma once

#include <unordered_map>
#include <unordered_set>

#include "PassManager.hpp"

/**
 * 分析函数的信息，包括哪些函数是纯函数，每个函数存储的变量
 */
class FuncInfo : public ModuleAnalysisPass {
    // 非纯函数的 load / store 信息
    struct UseMessage
    {
        // 影响的全局变量(注意此处不包含常全局变量，目前的文法也不支持常全局变量))
        std::unordered_set<GlobalVariable*> globals_;
        // 影响的参数(第一个参数序号为 0)
        std::unordered_set<Argument*> arguments_;

        void add(Value* val);
        bool have(Value* val) const;
        bool empty() const;
    };
  public:
    FuncInfo(Module *m) : ModuleAnalysisPass(m) {}

    void run() override;

    // 函数是否是纯函数
    bool is_pure(Function *func) { return !func->is_declaration() && !use_libs[func] && loads[func].empty() && stores[func].empty(); }
    // 函数是否使用了 io
    bool use_io(Function* func) { return func->is_declaration() || use_libs[func]; }
    // 返回 StoreInst 存入的变量(全局/局部变量或函数参数)
    static Value* store_ptr(const StoreInst* st);
    // 返回 LoadInst 加载的变量(全局/局部变量或函数参数)
    static Value* load_ptr(const LoadInst* ld);
    // 返回 CallInst 代表的函数调用间接存入的变量(全局/局部变量或函数参数)
    std::unordered_set<Value*> get_stores(const CallInst* call);
    // 返回 CallInst 代表的函数调用间接加载的变量(全局/局部变量或函数参数)
    std::unordered_set<Value*> get_loads(const CallInst* call);
  private:
    // 函数存储的值
    std::unordered_map<Function*, UseMessage> stores;
    // 函数加载的值
    std::unordered_map<Function*, UseMessage> loads;
    // 函数是否因为调用库函数而变得非纯函数
    std::unordered_map<Function*, bool> use_libs;

    // 将所有由变量 var 计算出的指针的来源都设置为变量 var, 并记录在函数内直接对 var 的 load/store
    void cal_val_2_var(Value* var, std::unordered_map<Value*, Value*>& val_2_var);
    static Value* trace_ptr(Value* val);

    void log() const;
};
