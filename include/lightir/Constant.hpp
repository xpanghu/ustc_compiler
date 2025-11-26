#pragma once

#include "Type.hpp"
#include "User.hpp"
#include "Value.hpp"

class Constant : public User {
private:
    // int value;
public:
    Constant(const Constant& other) = delete;
    Constant(Constant&& other) noexcept = delete;
    Constant& operator=(const Constant& other) = delete;
    Constant& operator=(Constant&& other) noexcept = delete;

    Constant(Type *ty, const std::string &name = "") : User(ty, name) {}
    ~Constant() override;

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;
    // 用于 lldb 调试生成 summary
    std::string safe_print_help() const;
protected:
};

class ConstantInt : public Constant {
  private:
    int value_;
    ConstantInt(Type* ty, int val) : Constant(ty, ""), value_(val) {}

  public:
    int get_value() const { return value_; }
    static ConstantInt *get(int val, Module *m);
    static ConstantInt *get(bool val, Module *m);
    std::string print() override;
};

class ConstantArray : public Constant {
  private:
    std::vector<Constant *> const_array;

    ConstantArray(ArrayType *ty, const std::vector<Constant *> &val);

  public:
    ConstantArray(const ConstantArray& other) = delete;
    ConstantArray(ConstantArray&& other) noexcept = delete;
    ConstantArray& operator=(const ConstantArray& other) = delete;
    ConstantArray& operator=(ConstantArray&& other) noexcept = delete;

    ~ConstantArray() override = default;

    Constant *get_element_value(int index) const;

    int get_size_of_array() const { return static_cast<int>(const_array.size()); }

    static ConstantArray *get(ArrayType *ty,
                              const std::vector<Constant *> &val);

    std::string print() override;
};

class ConstantZero : public Constant {
  private:
    ConstantZero(Type *ty) : Constant(ty, "") {}

  public:
    static ConstantZero *get(Type *ty, Module *m);
    std::string print() override;
};

class ConstantFP : public Constant {
  private:
    float val_;
    ConstantFP(Type *ty, float val) : Constant(ty, ""), val_(val) {}

  public:
    static ConstantFP *get(float val, Module *m);
    float get_value() const { return val_; }
    std::string print() override;
};
