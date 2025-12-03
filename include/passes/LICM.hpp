#pragma once

#include "FuncInfo.hpp"
#include "LoopDetection.hpp"
#include "PassManager.hpp"

class LoopInvariantCodeMotion : public TransformPass {
  public:
    LoopInvariantCodeMotion(Module *m) : TransformPass(m), loop_detection_(nullptr), func_info_(nullptr) {}
    ~LoopInvariantCodeMotion() override = default;

    void run() override;

  private:
    LoopDetection* loop_detection_;
    FuncInfo* func_info_;
    std::unordered_set<Value*> collect_loop_store_vars(Loop* loop);
    std::vector<Instruction*> collect_insts(Loop* loop);
    void traverse_loop(Loop* loop);
    void run_on_loop(Loop* loop);
};