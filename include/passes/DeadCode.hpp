#pragma once

#include <deque>

#include "PassManager.hpp"

class FuncInfo;
/**
 * 死代码消除：假设所有指令都可以去掉，然后只保留具有副作用的指令和它们所影响的指令。去掉不可达的基本块。
 *
 * 在初始化时指定一个 bool 参数 remove_unreachable_bb, 代表是否去除函数不可达基本块
 *
 * 参见 https://www.clear.rice.edu/comp512/Lectures/10Dead-Clean-SCCP.pdf
 **/
class DeadCode : public TransformPass {
  public:
    
    /**
     * 
     * @param m 所属 Module
     * @param remove_unreachable_bb 是否需要删除不可达的 BasicBlocks
     */
    DeadCode(Module *m, bool remove_unreachable_bb) : TransformPass(m), remove_bb_(remove_unreachable_bb), func_info(nullptr) {}

    void run() override;

  private:
    bool remove_bb_;
    FuncInfo* func_info;
    std::unordered_map<Instruction *, bool> marked{};
    std::deque<Instruction*> work_list{};

    // 标记函数中不可删除指令
    void mark(Function *func);
    // 标记某不可删除的指令依赖的指令
    void mark(const Instruction *ins);
    // 删除函数中无用指令
    bool sweep(Function *func);
    // 从 entry 开始对基本块进行搜索，删除不可达基本块
    static bool clear_basic_blocks(Function *func);
    // 指令是否有副作用
    bool is_critical(Instruction *ins) const;
    // 删除无用函数和全局变量
    void sweep_globally() const;
};
