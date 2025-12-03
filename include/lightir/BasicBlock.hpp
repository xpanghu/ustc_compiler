#pragma once

#include "Instruction.hpp"
#include "Value.hpp"

#include <list>
#include <set>
#include <string>

class Function;
class Instruction;
class Module;

class BasicBlock : public Value {
  public:
    BasicBlock(const BasicBlock& other) = delete;
    BasicBlock(BasicBlock&& other) noexcept = delete;
    BasicBlock& operator=(const BasicBlock& other) = delete;
    BasicBlock& operator=(BasicBlock&& other) noexcept = delete;
    ~BasicBlock() override;
    static BasicBlock *create(Module *m, const std::string &name,
                              Function *parent) {
        return new BasicBlock(m, name, parent);
    }

    /****************api about cfg****************/
    std::list<BasicBlock *> &get_pre_basic_blocks() { return pre_bbs_; }
    std::list<BasicBlock *> &get_succ_basic_blocks() { return succ_bbs_; }

    // 自动去重
    void add_pre_basic_block(BasicBlock *bb)
    {
        for (auto i : pre_bbs_) if (i == bb) return;
        pre_bbs_.push_back(bb);
    }
    // 自动去重
    void add_succ_basic_block(BasicBlock *bb)
    {
        for (auto i : succ_bbs_) if (i == bb) return;
        succ_bbs_.push_back(bb);
    }
    void remove_pre_basic_block(BasicBlock *bb) { pre_bbs_.remove(bb); }
    // 若你将 br label0, label0 的其中一个 label0 改为 label1，并且调用 remove_suc label0，那 suc 集合中也将不再包含 label0
    void remove_succ_basic_block(BasicBlock *bb) { succ_bbs_.remove(bb); }
    BasicBlock* get_entry_block_of_same_function() const;

    // If the Block is terminated by ret/br
    bool is_terminated() const;
    // Get terminator, only accept valid case use
    Instruction *get_terminator() const;

    /****************api about Instruction****************/

    // 在指令表最后插入指令
    // 新特性：指令表分为 {alloca, phi | other inst} 两段，创建和向基本块插入 alloca 和 phi，都只会插在第一段，它们在常规指令前面。
    // 因此，即使终止基本块也能插入 alloca 和 phi
    void add_instruction(Instruction *instr);
    // 在指令链表最前面插入指令
    // 新特性：指令表分为 {alloca, phi | other inst} 两段，创建和向基本块插入 alloca 和 phi，都只会插在第一段，它们在常规指令前面。
    void add_instr_begin(Instruction* instr);
    // 绕过终止指令插入指令
    // 新特性：指令表分为 {alloca, phi | other inst} 两段，创建和向基本块插入 alloca 和 phi，都只会插在第一段，它们在常规指令前面。
    void add_instr_before_terminator(Instruction* instr);

    // 从 BasicBlock 移除 Instruction，并 delete 这个 Instruction
    void erase_instr(Instruction* instr) { instr_list_.remove(instr); delete instr; }
    // 从 BasicBlock 移除集合中的 Instruction，并 delete 这些 Instruction
    void erase_instrs(const std::set<Instruction*>& instr);
    // 从 BasicBlock 移除 Instruction，你需要自己 delete 它
    void remove_instr(Instruction *instr) { instr_list_.remove(instr); }
    // 从 BasicBlock 移除集合中的 Instruction，你需要自己 delete 它们
    void remove_instrs(const std::set<Instruction*>& instr);

    // 移除的 Instruction 需要自己 delete
    std::list<Instruction*> &get_instructions() { return instr_list_; }
    bool empty() const { return instr_list_.empty(); }
    int get_num_of_instr() const { return static_cast<int>(instr_list_.size()); }

    /****************api about accessing parent****************/
    Function *get_parent() const { return parent_; }
    Module *get_module() const;
    void erase_from_parent();

    std::string print() override;

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;

  private:
    explicit BasicBlock(const Module *m, const std::string &name, Function *parent);

    std::list<BasicBlock *> pre_bbs_;
    std::list<BasicBlock *> succ_bbs_;
    std::list<Instruction*> instr_list_;
    Function *parent_;
};

extern Names GLOBAL_BASICBLOCK_NAMES_;