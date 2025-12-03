#include "DeadCode.hpp"

#include <queue>
#include <unordered_set>
#include <vector>

#include "FuncInfo.hpp"
#include "logging.hpp"

// 处理流程：两趟处理，mark 标记有用变量，sweep 删除无用指令
void DeadCode::run() {
    bool changed;
    func_info = new FuncInfo(m_);
    func_info->run();
    do {
        changed = false;
        for (auto func : m_->get_functions()) {
            if (func->is_declaration()) continue;
            if (remove_bb_) changed |= clear_basic_blocks(func);
            mark(func);
            changed |= sweep(func);
        }
    } while (changed);
    delete func_info;
    func_info = nullptr;
}

static void remove_phi_operand_if_in(PhiInst* inst, const std::unordered_set<BasicBlock*>& in)
{
    int opc = static_cast<int>(inst->get_num_operand());
    for (int i = opc - 1; i >= 0; i -= 2)
    {
        auto bb = dynamic_cast<BasicBlock*>(inst->get_operand(i));
        if (in.count(bb))
        {
            inst->remove_operand(i);
            inst->remove_operand(i - 1);
        }
    }
}

bool DeadCode::clear_basic_blocks(Function *func) {
    // 已经访问的基本块
    std::unordered_set<BasicBlock*> visited;
    // 还未访问的基本块
    std::queue<BasicBlock*> toVisit;
    toVisit.emplace(func->get_entry_block());
    visited.emplace(func->get_entry_block());
    while (!toVisit.empty())
    {
        auto bb = toVisit.front();
        toVisit.pop();
        for (auto suc : bb->get_succ_basic_blocks())
        {
            if (!visited.count(suc))
            {
                visited.emplace(suc);
                toVisit.emplace(suc);
            }
        }
    }

    // 遍历后剩余的基本块不可达
    std::unordered_set<BasicBlock*> erase_set;
    std::list<BasicBlock*> erase_list;
    for (auto bb : func->get_basic_blocks())
    {
        if (!visited.count(bb))
        {
            erase_set.emplace(bb);
            erase_list.emplace_back(bb);
        }
    }

    // 删除可达基本块中对不可达基本块中变量的 phi 引用
    // 例如 A -> B, B 中具有 phi [val , A], 则要将这一对删除
    // 当一个 phi 因为这个原因只剩下一对操作数时, 它可以被消除, 不过这里不管它
    for (auto i : func->get_basic_blocks())
    {
        if (erase_set.count(i)) continue;
        for (auto j : i->get_instructions())
        {
            auto phi = dynamic_cast<PhiInst*>(j);
            if (phi == nullptr) break; // 假定所有 phi 都在基本块指令的最前面，见 https://ustc-compiler-2025.github.io/homepage/exp_platform_intro/TA/#%E4%BD%BF%E7%94%A8%E4%B8%A4%E6%AE%B5%E5%8C%96-instruction-list
            remove_phi_operand_if_in(phi, erase_set);
        }
    }

    for (auto bb : erase_list) {
        bb->erase_from_parent();
        delete bb;
    }
    return !erase_list.empty();
}

void DeadCode::mark(Function *func) {
    work_list.clear();
    marked.clear();

    for (auto bb : func->get_basic_blocks()) {
        for (auto ins : bb->get_instructions()) {
            if (is_critical(ins)) {
                marked[ins] = true;
                work_list.push_back(ins);
            }
        }
    }

    while (work_list.empty() == false) {
        auto now = work_list.front();
        work_list.pop_front();

        mark(now);
    }
}

void DeadCode::mark(const Instruction *ins) {
    for (auto op : ins->get_operands()) {
        auto def = dynamic_cast<Instruction *>(op);
        if (def == nullptr)
            continue;
        if (marked[def])
            continue;
        if (def->get_function() != ins->get_function())
            continue;
        marked[def] = true;
        work_list.push_back(def);
    }
}

bool DeadCode::sweep(Function *func) {
    bool rm = false; // changed
    std::unordered_set<Instruction *> wait_del;
    for (auto bb : func->get_basic_blocks()) {
        for (auto inst : bb->get_instructions()) {
            if (marked[inst]) continue; 
            wait_del.emplace(inst);
        }
        bb->get_instructions().remove_if([&wait_del](Instruction* i) -> bool {return wait_del.count(i); });
        if (!wait_del.empty()) rm = true;
        wait_del.clear();
    }
    return rm;
}

bool DeadCode::is_critical(Instruction *ins) const {
    // 对纯函数的无用调用也可以在删除之列
    if (ins->is_call()) {
        auto call_inst = dynamic_cast<CallInst *>(ins);
        auto callee = dynamic_cast<Function *>(call_inst->get_operand(0));
        if (func_info->is_pure(callee))
            return false;
        return true;
    }
    if (ins->is_br() || ins->is_ret())
        return true;
    if (ins->is_store())
        return true;
    return false;
}

void DeadCode::sweep_globally() const {
    std::vector<Function *> unused_funcs;
    std::vector<GlobalVariable *> unused_globals;
    for (auto f_r : m_->get_functions()) {
        if (f_r->get_use_list().empty() and f_r->get_name() != "main")
            unused_funcs.push_back(f_r);
    }
    for (auto glob_var_r : m_->get_global_variable()) {
        if (glob_var_r->get_use_list().empty())
            unused_globals.push_back(glob_var_r);
    }
    // changed |= unused_funcs.size() or unused_globals.size();
    for (auto func : unused_funcs)
    {
        m_->get_functions().remove(func);
        delete func;
    }
    for (auto glob : unused_globals)
    {
        m_->get_global_variable().remove(glob);
        delete glob;
    }
}
