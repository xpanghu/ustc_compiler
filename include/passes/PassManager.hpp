#pragma once

#include <memory>
#include <vector>

#include "Module.hpp"

// 转换 Pass, 例如 mem2reg, licm, deadcode
class TransformPass {
public:
    TransformPass(Module* m) : m_(m) {}
    virtual ~TransformPass();
    virtual void run() = 0;

protected:
    Module* m_;
};

// 依赖于整个 Module 进行分析的分析 Pass, 例如 funcinfo
class ModuleAnalysisPass {
  public:
      ModuleAnalysisPass(Module *m) : m_(m) {}
      virtual ~ModuleAnalysisPass();
      virtual void run() = 0;

  protected:
    Module *m_;
};

// 依赖于单个 Function 进行分析的分析 Pass, 例如 dominators, loopdetection
class FunctionAnalysisPass {
public:
    FunctionAnalysisPass(Function* f) : f_(f) {}
    virtual ~FunctionAnalysisPass();
    virtual void run() = 0;

protected:
    Function* f_;
};

class PassManager {
  public:
    PassManager(Module *m) : m_(m) {}

    // 添加一个 Transform Pass, 添加的 Pass 被顺序运行
    template <typename PassType, typename... Args>
    void add_pass(Args &&...args) {
        static_assert(std::is_base_of_v<TransformPass, PassType>, "Pass must derive from TransformPass");
        passes_.emplace_back(new PassType(m_, std::forward<Args>(args)...));
    }

    void run() {
        for (auto& pass : passes_) {
            pass->run();
            delete pass;
            pass = nullptr;
        }
    }

  private:
    // 它们会被顺序运行
    std::vector<TransformPass*> passes_;
    Module *m_;
};
