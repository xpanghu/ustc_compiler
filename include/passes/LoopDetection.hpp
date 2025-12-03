#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Dominators.hpp"
#include "PassManager.hpp"

class BasicBlock;
class Dominators;
class Function;
class Module;

class Loop {
  private:
    // attribute:
    // preheader, header, blocks, parent, sub_loops, latches
    BasicBlock *preheader_ = nullptr;
    BasicBlock *header_;

    Loop* parent_ = nullptr;
    std::vector<BasicBlock*> blocks_;
    std::vector<Loop*> sub_loops_;
    
    std::unordered_set<BasicBlock *> latches_;
  public:
    Loop(BasicBlock *header) : header_(header) {
        blocks_.push_back(header);
    }
    ~Loop() = default;
    void add_block(BasicBlock *bb) { blocks_.push_back(bb); }
    BasicBlock *get_header() const { return header_; }
    BasicBlock *get_preheader() const { return preheader_; }
    Loop* get_parent() const { return parent_; }
    void set_parent(Loop* parent) { parent_ = parent; }
    void set_preheader(BasicBlock *bb) { preheader_ = bb; }
    void add_sub_loop(Loop* loop) { sub_loops_.push_back(loop); }
    const std::vector<BasicBlock*>& get_blocks() { return blocks_; }
    const std::vector<Loop*>& get_sub_loops() { return sub_loops_; }
    const std::unordered_set<BasicBlock *>& get_latches() { return latches_; }
    void add_latch(BasicBlock *bb) { latches_.insert(bb); }
    std::string safe_print() const;
};

class LoopDetection : public FunctionAnalysisPass {
    Dominators* dominators_;
    std::vector<Loop*> loops_;
    // map from header to loop
    std::unordered_map<BasicBlock *, Loop*> bb_to_loop_;
    void discover_loop_and_sub_loops(BasicBlock *bb, std::set<BasicBlock*>&latches,
                                     Loop* loop);

  public:
    LoopDetection(Function *f) : FunctionAnalysisPass(f), dominators_(nullptr) { assert(!f->is_declaration() && "LoopDetection can not apply to function declaration." ); }
    ~LoopDetection() override;

    void run() override;
    void print() const;
    std::vector<Loop*> &get_loops() { return loops_; }
};
