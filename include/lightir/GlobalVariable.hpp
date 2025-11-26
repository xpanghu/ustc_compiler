#pragma once

#include "Constant.hpp"
#include "User.hpp"

class Module;

class GlobalVariable : public User {
  private:
    bool is_const_;
    Constant *init_val_;
    GlobalVariable(const std::string& name, Module *m, Type *ty, bool is_const,
                   Constant *init = nullptr);

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;
  public:
    GlobalVariable(const GlobalVariable& other) = delete;
    GlobalVariable(GlobalVariable&& other) noexcept = delete;
    GlobalVariable& operator=(const GlobalVariable& other) = delete;
    GlobalVariable& operator=(GlobalVariable&& other) noexcept = delete;
    static GlobalVariable *create(const std::string& name, Module *m, Type *ty,
                                  bool is_const, Constant *init);
    ~GlobalVariable() override = default;
    Constant *get_init() const { return init_val_; }
    bool is_const() const { return is_const_; }
    std::string print() override;
};
