#include "LICM.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "Instruction.hpp"
#include "PassManager.hpp"

/**
 * @brief 循环不变式外提Pass的主入口函数
 * 
 */
void LoopInvariantCodeMotion::run()
{
    func_info_ = new FuncInfo(m_);
    func_info_->run();
    for (auto func : m_->get_functions())
    {
        if (func->is_declaration()) continue;
        loop_detection_ = new LoopDetection(func);
        loop_detection_->run();
        for (auto loop : loop_detection_->get_loops())
        {
            // 遍历处理顶层循环
            if (loop->get_parent() == nullptr) traverse_loop(loop);
        }
        delete loop_detection_;
        loop_detection_ = nullptr;
    }
    delete func_info_;
    func_info_ = nullptr;
}

/**
 * @brief 遍历循环及其子循环
 * @param loop 当前要处理的循环
 * 
 */
void LoopInvariantCodeMotion::traverse_loop(Loop* loop)
{
    // 先外层再内层，这样不用在插入 preheader 后更改循环
    run_on_loop(loop);
    for (auto sub_loop : loop->get_sub_loops())
    {
        traverse_loop(sub_loop);
    }
}

// 收集并返回循环 store 过的变量
// 例如
// %a = alloca ...
// %b = getelementptr %a ...
// store ... %b
// 则你应该返回 %a 而非 %b
std::unordered_set<Value*> LoopInvariantCodeMotion::collect_loop_store_vars(Loop* loop)
{
    // 可能用到
    // FuncInfo::store_ptr, FuncInfo::get_stores
    std::unordered_set<Value*> collect;
    for (auto bb : loop->get_blocks()) {
        for (auto inst : bb->get_instructions()) {
            if (inst->is_store())
                collect.insert(func_info_->store_ptr(dynamic_cast<StoreInst*>(inst)));
            if (inst->is_call()) {
                for (auto v : func_info_->get_stores(dynamic_cast<CallInst*>(inst))) {
                    collect.insert(v);
                }
            }
        }
    }
    return collect;
}

// 收集并返回循环中的所有指令
std::vector<Instruction*> LoopInvariantCodeMotion::collect_insts(Loop* loop)
{
    std::vector<Instruction*> collect;
    for (auto bb : loop->get_blocks()) {
        for (auto inst : bb->get_instructions()) {
            collect.push_back(inst);
        }
    }
    return collect;
}

enum InstructionType: std::uint8_t
{
    UNKNOWN, VARIANT, INVARIANT
};

/**
 * @brief 对单个循环执行不变式外提优化
 * @param loop 要优化的循环
 * 
 */
void LoopInvariantCodeMotion::run_on_loop(Loop* loop)
{
    // 循环 store 过的变量
    std::unordered_set<Value*> loop_stores_var = collect_loop_store_vars(loop);
    // 循环中的所有指令
    std::vector<Instruction*> instructions = collect_insts(loop);
    // Value* 在 map 内说明它是循环内的指令，InstructionType 指示它是循环变量（每次循环都会变）/ 循环不变量 还是 不知道
    std::unordered_map<Value*, InstructionType> inst_type;
    for (auto i : instructions) inst_type[i] = UNKNOWN;

    // 不同指针形参可能指向同一数组，也可能指向全局数组；局部 alloca 则是独立对象。
    auto may_be_modified = [&](Value* root)
    {
        for (auto stored : loop_stores_var)
        {
            if (root == stored) return true;
            if ((dynamic_cast<Argument*>(root) != nullptr && dynamic_cast<AllocaInst*>(stored) == nullptr)
                || (dynamic_cast<Argument*>(stored) != nullptr && dynamic_cast<GlobalVariable*>(root) != nullptr))
                return true;
        }
        return false;
    };

    // 按被判定为不变量的顺序记录，保证定义先于依赖它的指令外提。
    std::vector<Instruction*> invariants;
    bool changed;
    do
    {
        changed = false;
        for (auto inst : instructions)
        {
            if (inst_type[inst] != UNKNOWN) continue;
            InstructionType type = INVARIANT;
            if (inst->is_store() || inst->is_ret() || inst->is_br() ||
                inst->is_phi() || inst->is_alloca())
            {
                type = VARIANT;
            }
            else if (inst->is_load())
            {
                if (may_be_modified(func_info_->load_ptr(static_cast<LoadInst*>(inst))))
                    type = VARIANT;
            }
            else if (inst->is_call())
            {
                auto call = dynamic_cast<CallInst*>(inst);
                auto callee = call->get_operand(0)->as<Function>();
                if (func_info_->use_io(callee) || !func_info_->get_stores(call).empty())
                    type = VARIANT;
                // 只读的非纯函数也可外提，但它读取的内存必须在整个循环中不变。
                for (auto loaded : func_info_->get_loads(call))
                    if (may_be_modified(loaded)) type = VARIANT;
            }

            if (type != VARIANT)
            {
                for (auto operand : inst->get_operands())
                {
                    auto found = inst_type.find(operand);
                    if (found == inst_type.end()) continue; // 循环外的值
                    if (found->second == VARIANT)
                    {
                        type = VARIANT;
                        break;
                    }
                    if (found->second == UNKNOWN) type = UNKNOWN;
                }
            }
            if (type == UNKNOWN) continue;
            inst_type[inst] = type;
            changed = true;
            if (type == INVARIANT) invariants.push_back(inst);
        }
    }
    while (changed);

    if (invariants.empty()) return;

    auto header = loop->get_header();
    std::unordered_set<BasicBlock*> blocks(loop->get_blocks().begin(), loop->get_blocks().end());
    std::vector<BasicBlock*> outside_preds;
    for (auto pred : header->get_pre_basic_blocks())
        if (!blocks.count(pred)) outside_preds.push_back(pred);
    if (outside_preds.empty()) return;

    if (outside_preds.size() > 1 || outside_preds.front()->get_succ_basic_blocks().size() > 1)
    {
        // 插入 preheader
        auto bb = BasicBlock::create(m_, "", loop->get_header()->get_parent());
        loop->set_preheader(bb);

        for (auto inst : header->get_instructions())
        {
            if (!inst->is_phi()) break;
            auto phi = static_cast<PhiInst*>(inst);
            std::vector<Value*> values;
            std::vector<BasicBlock*> predecessors;
            auto pairs = phi->get_phi_pairs();
            for (auto pair : pairs)
            {
                if (blocks.count(pair.second)) continue;
                values.push_back(pair.first);
                predecessors.push_back(pair.second);
            }
            // 只有一条入口边时直接转接；多条入口边时在 preheader 合并。
            Value* incoming = values.front();
            if (values.size() > 1)
                incoming = PhiInst::create_phi(phi->get_type(), bb, values, predecessors);
            phi->remove_all_operands();
            for (auto pair : pairs)
                if (blocks.count(pair.second)) phi->add_phi_pair_operand(pair.first, pair.second);
            phi->add_phi_pair_operand(incoming, bb);
        }

        // 仅转接循环外的入口边，回边仍然直接跳到 header。
        for (auto pred : outside_preds)
            static_cast<BranchInst*>(pred->get_terminator())->replace_all_bb_match(header, bb);
        BranchInst::create_br(header, bb);
    }
    else loop->set_preheader(outside_preds.front());

    auto preheader = loop->get_preheader();
    for (auto inst : invariants)
    {
        inst->get_parent()->remove_instr(inst); // 移动而非删除，保留 use-def 关系
        inst->set_parent(preheader);
        preheader->add_instr_before_terminator(inst);
    }
}
