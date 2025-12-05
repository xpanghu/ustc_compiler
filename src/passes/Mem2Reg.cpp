#include "Mem2Reg.hpp"

#include "IRBuilder.hpp"
#include "Value.hpp"


// ptr 是否是非数组 alloca 变量(是则转换为 AllocaInst)
static AllocaInst* is_not_array_alloca(Value* ptr)
{
    auto alloca = dynamic_cast<AllocaInst*>(ptr);
    if (alloca != nullptr && !alloca->get_alloca_type()->is_array_type()) return alloca;
    return nullptr;
}

/**
 * @brief Mem2Reg Pass的主入口函数
 *
 * 该函数执行内存到寄存器的提升过程，将栈上的局部变量提升到SSA格式。
 * 主要步骤：
 * 1. 创建并运行支配树分析
 * 2. 对每个非声明函数：
 *    - 清空相关数据结构
 *    - 插入必要的phi指令
 *    - 执行变量重命名
 *
 * 注意：函数执行后，冗余的局部变量分配指令将由后续的死代码删除Pass处理
 */
void Mem2Reg::run() {
    // 以函数为单元遍历实现 Mem2Reg 算法
    for (auto f : m_->get_functions()) {
        if (f->is_declaration())
            continue;
        func_ = f;
        // 创建 func_ 支配树
        dominators_ = new Dominators(func_);
        // 建立支配树
        dominators_->run();
        allocas_.clear();
        var_val_stack.clear();
        phi_to_alloca_.clear();
        bb_to_phi_.clear();
        if (!func_->get_basic_blocks().empty()) {
            // 对应伪代码中 phi 指令插入的阶段
            generate_phi();
            // 确保每个局部变量的栈都有初始值
            for (auto var : allocas_)
                var_val_stack[var].emplace_back(var->get_alloca_type()->is_float_type() ? static_cast<Value*>(ConstantFP::get(0, m_)) : static_cast<Value*>(ConstantInt::get(0, m_)));
            // 对应伪代码中重命名阶段
            rename(func_->get_entry_block());
        }
        delete dominators_;
        dominators_ = nullptr;
        // 后续 DeadCode 将移除冗余的局部变量的分配空间
    }
}

/**
 * @brief 在必要的位置插入phi指令
 *
 * 该函数实现了经典的phi节点插入算法：
 * 1. 收集全局活跃变量：
 *    - 扫描所有store指令
 *    - 识别在多个基本块中被赋值的变量
 *
 * 2. 插入phi指令：
 *    - 对每个全局活跃变量
 *    - 在其定值点的支配边界处插入phi指令
 *    - 使用工作表法处理迭代式的phi插入
 *
 * phi指令的插入遵循最小化原则，只在必要的位置插入phi节点
 */
void Mem2Reg::generate_phi() {
    // 步骤一：找到活跃在多个 block 的名字集合，以及它们所属的 bb 块

    // global_live_var_name 包括函数中所有非数组 alloca 变量
    std::set<AllocaInst *> not_array_allocas;
    // 每个 alloca 在什么基本块被 store (可能重复)
    std::map<AllocaInst*, std::list<BasicBlock *>> allocas_stored_bbs;
    for (auto bb : func_->get_basic_blocks()) {
        for (auto instr : bb->get_instructions()) {
            if (instr->is_store()) {
                // store i32 a, i32 *b
                // a is r_val, b is l_val
                auto l_val = dynamic_cast<StoreInst*>(instr)->get_ptr();
                if (auto lalloca = is_not_array_alloca(l_val)) {
                    if (!not_array_allocas.count(lalloca))
                    {
                        not_array_allocas.insert(lalloca);
                        allocas_.emplace_back(lalloca);
                    }
                    allocas_stored_bbs[lalloca].emplace_back(bb);
                }
            }
        }
    }

    // 步骤二：从支配树获取支配边界信息，并在对应位置插入 phi 指令

    // 基本块是否已经有了对特定 alloca 变量的 phi
    std::set<std::pair<BasicBlock *, AllocaInst *>> bb_has_var_phi;
    for (auto var : not_array_allocas) {
        std::vector<BasicBlock *> work_list;
        std::set<BasicBlock*> already_handled;
        work_list.assign(allocas_stored_bbs[var].begin(), allocas_stored_bbs[var].end());
        for (unsigned i = 0; i < work_list.size(); i++) {
            auto bb = work_list[i];
            // 防止在同一基本块重复运行
            if (already_handled.count(bb)) continue;
            already_handled.emplace(bb);
            for (auto bb_dominance_frontier_bb :
                dominators_->get_dominance_frontier(bb)) {
                if (bb_has_var_phi.find({bb_dominance_frontier_bb, var}) ==
                    bb_has_var_phi.end()) {
                    // generate phi for bb_dominance_frontier_bb & add
                    // bb_dominance_frontier_bb to work list
                    auto phi = PhiInst::create_phi(
                        var->get_type()->get_pointer_element_type(),
                        bb_dominance_frontier_bb);
                    phi_to_alloca_.emplace(phi, var);
                    bb_to_phi_[bb_dominance_frontier_bb].emplace_back(phi);
                    work_list.push_back(bb_dominance_frontier_bb);
                    bb_has_var_phi.emplace(bb_dominance_frontier_bb, var);
                }
            }
        }
    }
}

void Mem2Reg::rename(BasicBlock *bb) {
    // 可能用到的数据结构
    // list<AllocaInst*> allocas_ 所有 Mem2Reg 需要消除的局部变量，用于遍历
    // map<AllocaInst*,vector<Value *>> var_val_stack 每个局部变量的存储值栈，还未进行任何操作时已经存进去了 0，不会为空
    // map<PhiInst *, AllocaInst*> phi_to_alloca_; Phi 对应的局部变量
    // map<BasicBlock*, list<PhiInst*>> bb_to_phi_; 在某个基本块的 Phi
    // 可能用到的函数
    // Value::replace_all_use_with(Value* a) 将所有用到 this 的指令对应 this 的操作数都替换为 a
    // BasicBlock::erase_instrs(set<Instruction*>) 移除并 delete 列表中的指令
    // StoreInst / LoadInst get_ptr 这些 Inst 所操作的变量
    // is_not_array_alloca(Value* ptr) 一个变量是不是 Mem2Reg 所关心的非数组局部变量

    // TODO
    // 步骤一：对每个 alloca 非数组变量(局部变量), 在其存储值栈存入其当前的最新值(也就是目前的栈顶值)
    // 步骤二：遍历基本块所有指令，执行操作并记录需要删除的 load/store/alloca 指令(注意: 并非所有 load/store/alloca 都是 Mem2Reg 需要处理的)
    // - 步骤三: 将 store 指令存储的值，作为其对应局部变量的最新值(更新栈顶)
    // - 步骤四: 将 phi 指令作为其对应局部变量的最新值(更新栈顶)
    // - 步骤五: 将 load 指令的所有使用替换为其读取的局部变量的最新值
    // 步骤六：为所有后继块的 phi 添加参数
    // 步骤七：对 bb 在支配树上的所有后继节点，递归执行 rename 操作
    // 步骤八：pop 出所有局部变量的最新值
    // 步骤九：删除需要删除的冗余指令
}

