#include "LoopDetection.hpp"

#include "Dominators.hpp"

using std::set;
using std::vector;
using std::map;

LoopDetection::~LoopDetection()
{
    for (auto loop : loops_) delete loop;
}

 /**
  * @brief 对单个函数执行循环检测
  *
  * 该函数通过以下步骤检测循环：
  * 1. 创建支配树分析实例
  * 2. 运行支配树分析
  * 3. 按支配树后序遍历所有基本块
  * 4. 对每个块，检查其前驱是否存在回边
  * 5. 如果存在回边，创建新的循环并：
  *    - 设置循环header
  *    - 添加latch节点
  *    - 发现循环体和子循环
  * 6. 最后打印检测结果
  */
void LoopDetection::run() {
    dominators_ = new Dominators(f_);
    dominators_->run();
    for (auto bb : dominators_->get_dom_post_order()) {
        std::set<BasicBlock*> latches;
        for (auto pred : bb->get_pre_basic_blocks()) {
            if (dominators_->is_dominate(bb, pred)) {
                // pred is a back edge
                // pred -> bb , pred is the latch node
                latches.insert(pred);
            }
        }
        if (latches.empty()) {
            continue;
        }
        // create loop
        auto loop = new Loop(bb);
        bb_to_loop_[bb] = loop;
        // add latch nodes
        for (auto latch : latches) {
            loop->add_latch(latch);
        }
        loops_.push_back(loop);
        discover_loop_and_sub_loops(bb, latches, loop);
    }
    print();
    delete dominators_;
}

std::string Loop::safe_print() const
{
    std::string ret;
    if (header_ == nullptr) ret += "b<null>";
    else ret += header_->get_name();
    ret += " ";
    ret += std::to_string(blocks_.size());
    ret += "b ";
    ret += std::to_string(latches_.size());
    ret += "latch ";
    ret += std::to_string(sub_loops_.size());
    ret += "sub";
    return ret;
}

/**
 * @brief 发现循环及其子循环
 * @param bb 循环的header块
 * @param latches 循环的回边终点(latch)集合
 * @param loop 当前正在处理的循环对象
 */
void LoopDetection::discover_loop_and_sub_loops(BasicBlock *bb, std::set<BasicBlock*>&latches, Loop* loop) {
    // TODO List:
    // 1. 初始化工作表，将所有latch块加入
    // 2. 实现主循环逻辑
    // 3. 处理未分配给任何循环的节点
    // 4. 处理已属于其他循环的节点
    // 5. 建立正确的循环嵌套关系

    std::vector<BasicBlock*> work_list = {latches.begin(), latches.end()}; // 初始化工作表
    std::unordered_set<BasicBlock*> already_in_work_list = { latches.begin(), latches.end() }; // 已经在工作表，防止重复加入

    while (!work_list.empty()) { // 当工作表非空时继续处理
        auto bb2 = work_list.back();
        work_list.pop_back();
        already_in_work_list.erase(bb2);

        // TODO-1: 处理未分配给任何循环的节点
        if (bb_to_loop_.find(bb2) == bb_to_loop_.end()) {
            /* 在此添加代码：
             * 1. 使用loop->add_block将bb加入当前循环
             * 2. 更新bb_to_loop_映射
             * 3. 将bb的所有前驱加入工作表
             */
        throw std::runtime_error("Lab4: 你有一个TODO需要完成！");
        
        }
        // TODO-2: 处理已属于其他循环的节点
        if (bb_to_loop_[bb2] != loop) {
            /* 在此添加代码：
             * 1. 获取bb当前所属的循环sub_loop
             * 2. 找到sub_loop的最顶层父循环
             * 3. 检查是否需要继续处理
             * 4. 建立循环嵌套关系：
             *    - 设置父循环
             *    - 添加子循环
             * 5. 将子循环的所有基本块加入到父循环中
             * 6. 将子循环header的前驱加入工作表
             */

        throw std::runtime_error("Lab4: 你有一个TODO需要完成！");

        }
    }
}


/**
 * @brief 打印循环检测的结果
 *
 * 为每个检测到的循环打印：
 * 1. 循环的header块
 * 2. 循环包含的所有基本块
 * 3. 循环的所有子循环
 */
void LoopDetection::print() const {
    f_->get_parent()->set_print_name();
    std::cerr << "Loop Detection Result:\n";
    for (auto &loop : loops_) {
        std::cerr << "Loop header: " << loop->get_header()->get_name()
                  << '\n';
        std::cerr << "Loop blocks: ";
        for (auto bb : loop->get_blocks()) {
            std::cerr << bb->get_name() << " ";
        }
        std::cerr << '\n';
        std::cerr << "Sub loops: ";
        for (auto &sub_loop : loop->get_sub_loops()) {
            std::cerr << sub_loop->get_header()->get_name() << " ";
        }
        std::cerr << '\n';
    }
}