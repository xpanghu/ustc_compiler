#pragma once

#include <map>
#include <set>

#include "BasicBlock.hpp"
#include "PassManager.hpp"


/**
 * 分析 Pass, 获得某函数的支配树信息
 *
 * 由于它是针对某一特定 Function 的分析 Pass, 你无法通过 m_ 获取 Module, 但可以通过 f_ 获取 Function 
 */
class Dominators : public FunctionAnalysisPass {
  public:

    explicit Dominators(Function* f) : FunctionAnalysisPass(f) { assert(!f->is_declaration() && "Dominators can not apply to function declaration."); }
    ~Dominators() override = default;
    void run() override;

    // 获取基本块的直接支配节点
    BasicBlock *get_idom(BasicBlock *bb) const { return idom_.at(bb); }
    const std::set<BasicBlock*> &get_dominance_frontier(BasicBlock *bb) {
        return dom_frontier_.at(bb);
    }
    const std::set<BasicBlock*> &get_dom_tree_succ_blocks(BasicBlock *bb) {
        return dom_tree_succ_blocks_.at(bb);
    }

    // print cfg or dominance tree
    void dump_cfg() const;
    void dump_dominator_tree();

    // functions for dominance tree
    bool is_dominate(BasicBlock *bb1, BasicBlock *bb2) const {
        return dom_tree_L_.at(bb1) <= dom_tree_L_.at(bb2) &&
               dom_tree_R_.at(bb1) >= dom_tree_L_.at(bb2);
    }

    const std::vector<BasicBlock *> &get_dom_dfs_order() {
        return dom_dfs_order_;
    }

    const std::vector<BasicBlock *> &get_dom_post_order() {
        return dom_post_order_;
    }

  private:

    void dfs(BasicBlock *bb, std::set<BasicBlock *> &visited);
    void create_idom();
    void create_dominance_frontier();
    void create_dom_tree_succ();
    void create_dom_dfs_order();

    BasicBlock * intersect(BasicBlock *b1, BasicBlock *b2) const;

    void create_reverse_post_order();
    void set_idom(BasicBlock *bb, BasicBlock *idom) { idom_[bb] = idom; }
    void set_dominance_frontier(BasicBlock *bb, std::set<BasicBlock*>&df) {
        dom_frontier_[bb].clear();
        dom_frontier_[bb].insert(df.begin(), df.end());
    }
    void add_dom_tree_succ_block(BasicBlock *bb, BasicBlock *dom_tree_succ_bb) {
        dom_tree_succ_blocks_[bb].insert(dom_tree_succ_bb);
    }
    unsigned int get_reversed_post_order(BasicBlock *bb) const {
        return reversed_post_order_.at(bb);
    }
    // for debug
    void print_idom() const;
    void print_dominance_frontier();

    std::vector<BasicBlock *> reversed_post_order_vec_{}; // 逆后序
    std::map<BasicBlock *, unsigned int> reversed_post_order_{}; // 逆后序索引
    std::map<BasicBlock *, BasicBlock *> idom_{};  // 直接支配
    std::map<BasicBlock *, std::set<BasicBlock*>> dom_frontier_{}; // 支配边界集合
    std::map<BasicBlock *, std::set<BasicBlock*>> dom_tree_succ_blocks_{}; // 支配树中的后继节点

    // 支配树上的dfs序L,R
    std::map<BasicBlock *, unsigned int> dom_tree_L_;
    std::map<BasicBlock *, unsigned int> dom_tree_R_;

    std::vector<BasicBlock *> dom_dfs_order_;
    std::vector<BasicBlock *> dom_post_order_;

};
