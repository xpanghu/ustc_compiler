#include "LICM.hpp"

#include <memory>
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

// TODO: 收集并返回循环 store 过的变量
// 例如
// %a = alloca ...
// %b = getelementptr %a ...
// store ... %b
// 则你应该返回 %a 而非 %b
std::unordered_set<Value*> LoopInvariantCodeMotion::collect_loop_store_vars(Loop* loop)
{
    // 可能用到
    // FuncInfo::store_ptr, FuncInfo::get_stores
    throw std::runtime_error("Lab4: 你有一个TODO需要完成！");
}

// TODO: 收集并返回循环中的所有指令
std::vector<Instruction*> LoopInvariantCodeMotion::collect_insts(Loop* loop)
{
    throw std::runtime_error("Lab4: 你有一个TODO需要完成！");
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
    int insts_count = static_cast<int>(instructions.size());
    // Value* 在 map 内说明它是循环内的指令，InstructionType 指示它是循环变量（每次循环都会变）/ 循环不变量 还是 不知道
    std::unordered_map<Value*, InstructionType> inst_type;
    for (auto i : instructions) inst_type[i] = UNKNOWN;

    // 遍历后是不是还有指令不知道 InstructionType
    bool have_inst_can_not_decide;
    // 是否存在 invariant
    bool have_invariant = false;
    do
    {
        have_inst_can_not_decide = false;
        for (int i = 0; i < insts_count; i++)
        {
            Instruction* inst = instructions[i];
            InstructionType type = inst_type[inst];
            if (type != UNKNOWN) continue;
            // 可能有用的函数
            // FuncInfo::load_ptr, FuncInfo::get_stores, FuncInfo::use_io

            // TODO: 识别循环不变式指令
            // - 将 store、ret、br、phi 等指令与非纯函数调用标记为 VARIANT
            // - 如果 load 指令加载的变量是循环 store 过的变量，标记为 VARIANT
            // - 如果指令有 VARIANT 操作数，标记为 VARIANT
            // - 如果指令所有操作数都是 INVARIANT (或者不在循环内)，标记为 INVARIANT, 设置 have_invariant
            // - 否则设置 have_inst_can_not_decide

            // TODO: 外提循环不变的非纯函数调用
            // 注意: 你不应该外提使用了 io 的函数调用

            throw std::runtime_error("Lab4: 你有一个TODO需要完成！");
        }
    }
    while (have_inst_can_not_decide);

    if (!have_invariant) return;

    auto header = loop->get_header();

    if (header->get_pre_basic_blocks().size() > 1 || header->get_pre_basic_blocks().front()->get_succ_basic_blocks().size() > 1)
    {
        // 插入 preheader
        auto bb = BasicBlock::create(m_, "", loop->get_header()->get_parent());
        loop->set_preheader(bb);

        for (auto phi : loop->get_header()->get_instructions())
        {
            if (phi->get_instr_type() != Instruction::phi) break;
            // 可能有用的函数
            // PhiInst::create_phi

            // TODO: 分裂 phi 指令
            throw std::runtime_error("Lab4: 你有一个TODO需要完成！");
        }

        // 可能有用的函数
        // BranchInst::replace_all_bb_match

        // TODO: 维护 bb, header, 与 header 前驱块的基本块关系
        throw std::runtime_error("Lab4: 你有一个TODO需要完成！");

        BranchInst::create_br(header, bb);

        // 若你想维护 LoopDetection 在 LICM 后保持正确
        // auto loop2 = loop->get_parent();
        // while (loop2 != nullptr)
        // {
        //      loop2->add_block(bb);
        //     loop2 = loop2->get_parent();
        // }
    }
    else loop->set_preheader(header->get_pre_basic_blocks().front());

    // insert preheader
    auto preheader = loop->get_preheader();

    auto terminator = preheader->get_instructions().back();
    preheader->get_instructions().pop_back();

    // 可以使用 Function::check_for_block_relation_error 检查基本块间的关系是否正确维护

    // TODO: 外提循环不变指令
    throw std::runtime_error("Lab4: 你有一个TODO需要完成！");

    preheader->add_instruction(terminator);

    std::cerr << "licm done\n";
}
