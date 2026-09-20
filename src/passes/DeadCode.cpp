#include "DeadCode.hpp"

#include <queue>
#include <unordered_set>
#include <vector>

#include "FuncInfo.hpp"
#include "logging.hpp"

// 处理流程：两趟处理，mark 标记有用变量，sweep 删除无用指令
void DeadCode::run()
{
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

    // 删无用函数/全局放在最后, 而且必须放在 func_info 释放【之后】:
    //   FuncInfo 的 loads/stores 里存着 GlobalVariable* / Argument*, 全局变量一旦被删,
    //   这些指针就成了悬垂指针. 放在这里可以保证"删全局时已经不存在任何持有它们的分析对象".
    // 它也不能放进上面的 for 里 —— 遍历 m_->get_functions() 时删元素会让迭代器失效.
    delete func_info;
    func_info = nullptr;
    sweep_globally();

    // 上一句删函数时把其中的指令一并删了, 这两个容器不能留着悬垂的 Instruction*
    marked.clear();
    work_list.clear();
}

static void remove_phi_operand_if_in(PhiInst* inst, const std::unordered_set<BasicBlock*>& in)
{
    int opc = static_cast<int>(inst->get_num_operand());
    for (int i = opc - 1; i >= 0; i -= 2) {
        auto bb = dynamic_cast<BasicBlock*>(inst->get_operand(i));
        if (in.count(bb)) {
            inst->remove_operand(i);
            inst->remove_operand(i - 1);
        }
    }
}

bool DeadCode::clear_basic_blocks(Function* func)
{
    // 已经访问的基本块
    std::unordered_set<BasicBlock*> visited;
    // 还未访问的基本块
    std::queue<BasicBlock*> toVisit;
    toVisit.emplace(func->get_entry_block());
    visited.emplace(func->get_entry_block());
    while (!toVisit.empty()) {
        auto bb = toVisit.front();
        toVisit.pop();
        for (auto suc : bb->get_succ_basic_blocks()) {
            if (!visited.count(suc)) {
                visited.emplace(suc);
                toVisit.emplace(suc);
            }
        }
    }

    // 遍历后剩余的基本块不可达
    std::unordered_set<BasicBlock*> erase_set;
    std::list<BasicBlock*> erase_list;
    for (auto bb : func->get_basic_blocks()) {
        if (!visited.count(bb)) {
            erase_set.emplace(bb);
            erase_list.emplace_back(bb);
        }
    }

    // 删除可达基本块中对不可达基本块中变量的 phi 引用
    // 例如 A -> B, B 中具有 phi [val , A], 则要将这一对删除
    // 当一个 phi 因为这个原因只剩下一对操作数时, 它可以被消除, 不过这里不管它
    for (auto i : func->get_basic_blocks()) {
        if (erase_set.count(i)) continue;
        for (auto j : i->get_instructions()) {
            auto phi = dynamic_cast<PhiInst*>(j);
            if (phi == nullptr) break; 
            remove_phi_operand_if_in(phi, erase_set);
        }
    }

    for (auto bb : erase_list) {
        bb->erase_from_parent();
        delete bb;
    }
    return !erase_list.empty();
}

void DeadCode::mark(Function* func)
{
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

void DeadCode::mark(const Instruction* ins)
{
    for (auto op : ins->get_operands()) {
        auto def = dynamic_cast<Instruction*>(op);
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

bool DeadCode::sweep(Function* func)
{
    bool rm = false; // changed
    std::unordered_set<Instruction*> wait_del;
    for (auto bb : func->get_basic_blocks()) {
        for (auto inst : bb->get_instructions()) {
            if (marked[inst]) continue;
            wait_del.emplace(inst);
        }
        bb->get_instructions().remove_if([&wait_del](Instruction* i) -> bool { return wait_del.count(i); });
        if (!wait_del.empty()) rm = true;
        // remove_if 只是把指针从链表里摘掉, 并不会析构指令. 不 delete 会留下两个后果:
        //   1) 内存泄漏;
        //   2) 指令的 use_list 永远不清理 —— 于是"某函数/全局是否还有使用者"这类判断会失真,
        //      sweep_globally 就永远删不掉已经没人调用的函数.
        // 删除是安全的: 被标记的指令不可能用到未标记的指令(标记阶段沿操作数反向传播保证的);
        // 而 Value::~Value 会把所有使用者的操作数槽置空, 所以先后顺序也不影响.
        for (auto inst : wait_del) delete inst;
        wait_del.clear();
    }
    return rm;
}

bool DeadCode::is_critical(Instruction* ins) const
{
    // 对纯函数的无用调用也可以在删除之列
    if (ins->is_call()) {
        auto call_inst = dynamic_cast<CallInst*>(ins);
        auto callee = dynamic_cast<Function*>(call_inst->get_operand(0));
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

void DeadCode::sweep_globally()
{
    // 内部迭代到不动点: 删掉 f 之后, "只被 f 使用"的 g 和全局变量会变成新的无用项.
    //
    // 这也是它不必参与 run() 外层不动点的原因 —— 它删的都是"没有使用者"的函数/全局,
    // 因此不会让任何【指令】变成死代码, 与指令级的 mark/sweep 互不影响.
    // 正是这一点让它可以被放到 func_info 释放之后调用(见 run()), 从而不存在
    // "分析结果里还存着被删掉的 GlobalVariable*" 这类悬垂问题.
    bool changed;
    do {
        changed = false;
        std::vector<Function*> unused_funcs;
        std::vector<GlobalVariable*> unused_globals;
        for (auto f_r : m_->get_functions()) {
            if (f_r->get_use_list().empty() and f_r->get_name() != "main")
                unused_funcs.push_back(f_r);
        }
        for (auto glob_var_r : m_->get_global_variable()) {
            if (glob_var_r->get_use_list().empty())
                unused_globals.push_back(glob_var_r);
        }
        // 先收集再统一删: 删除会改动链表, 不能边遍历边删
        for (auto func : unused_funcs) {
            m_->get_functions().remove(func);
            delete func;
        }
        for (auto glob : unused_globals) {
            m_->get_global_variable().remove(glob);
            delete glob;
        }
        changed = !unused_funcs.empty() || !unused_globals.empty();
    } while (changed);
}
