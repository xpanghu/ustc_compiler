#pragma once

#include "BasicBlock.hpp"
#include "Type.hpp"

#include <cassert>
#include <list>
#include <memory>

#include "Names.hpp"

class Module;
class Argument;
class Type;
class FunctionType;

class Function : public Value {
  public:
    Function(const Function& other) = delete;
    Function(Function&& other) noexcept = delete;
    Function& operator=(const Function& other) = delete;
    Function& operator=(Function&& other) noexcept = delete;
    Function(FunctionType *ty, const std::string &name, Module *parent);
    ~Function() override;
    static Function *create(FunctionType *ty, const std::string &name,
                            Module *parent);

    FunctionType *get_function_type() const;
    Type *get_return_type() const;

    void add_basic_block(BasicBlock *bb);

    unsigned get_num_of_args() const;
    unsigned get_num_basic_blocks() const;

    Module *get_parent() const;

    // 此处 remove 的 BasicBlock, 需要手动 delete
    void remove(BasicBlock *bb);
    BasicBlock *get_entry_block() const { return basic_blocks_.front(); }

    std::list<BasicBlock*> &get_basic_blocks() { return basic_blocks_; }
    std::list<Argument*> &get_args() { return arguments_; }

    bool is_declaration() const { return basic_blocks_.empty(); }

    void set_instr_name();
    std::string print() override;
    // 用于检查函数的基本块是否存在问题
    void check_for_block_relation_error() const;

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;

    // 用于给 basicblock 分配名称
    Names names4blocks_;
    Names names4insts_;

  private:
    std::list<BasicBlock*> basic_blocks_;
    std::list<Argument*> arguments_;
    Module *parent_;
    unsigned seq_cnt_; // print use
};

// Argument of Function, does not contain actual value
class Argument : public Value {
  public:
    Argument(const Argument& other) = delete;
    Argument(Argument&& other) noexcept = delete;
    Argument& operator=(const Argument& other) = delete;
    Argument& operator=(Argument&& other) noexcept = delete;
    explicit Argument(Type *ty, const std::string &name = "",
                      Function *f = nullptr, unsigned arg_no = 0)
        : Value(ty, name), parent_(f), arg_no_(arg_no) {}
    ~Argument() override = default;

    const Function *get_parent() const { return parent_; }
    Function *get_parent() { return parent_; }

    /// For example in "void foo(int a, float b)" a is 0 and b is 1.
    unsigned get_arg_no() const {
        assert(parent_ && "can't get number of unparented arg");
        return arg_no_;
    }

    std::string print() override;

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;

  private:
    Function *parent_;
    unsigned arg_no_; // argument No.
};
