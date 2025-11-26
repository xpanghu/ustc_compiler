#pragma once

#include <iostream>
#include <vector>
#include <cstdint>

class Module;
class IntegerType;
class FunctionType;
class ArrayType;
class PointerType;
class FloatType;

class Type {
  public:
    Type(const Type& other) = delete;
    Type(Type&& other) noexcept = delete;
    Type& operator=(const Type& other) = delete;
    Type& operator=(Type&& other) noexcept = delete;

    enum TypeID: uint8_t {
        VoidTyID,     // Void
        LabelTyID,    // Labels, e.g., BasicBlock
        IntegerTyID,  // Integers, include 32 bits and 1 bit
        FunctionTyID, // Functions
        ArrayTyID,    // Arrays
        PointerTyID,  // Pointer
        FloatTyID     // float
    };

    explicit Type(TypeID tid, Module *m);

// 生成 vptr, 使调试时能够显示 IntegerType 等 Type 的信息
    virtual ~Type();

    TypeID get_type_id() const { return tid_; }

    bool is_void_type() const { return get_type_id() == VoidTyID; }
    bool is_label_type() const { return get_type_id() == LabelTyID; }
    bool is_integer_type() const { return get_type_id() == IntegerTyID; }
    bool is_function_type() const { return get_type_id() == FunctionTyID; }
    bool is_array_type() const { return get_type_id() == ArrayTyID; }
    bool is_pointer_type() const { return get_type_id() == PointerTyID; }
    bool is_float_type() const { return get_type_id() == FloatTyID; }
    bool is_int32_type() const;
    bool is_int1_type() const;

    // Return related data member if is the required type, else throw error
    Type *get_pointer_element_type() const;
    Type *get_array_element_type() const;

    Module *get_module() const { return m_; }
    unsigned get_size() const;

    std::string print() const;

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;

  private:
    TypeID tid_;
    Module *m_;
};

class IntegerType : public Type {
  public:
    IntegerType(const IntegerType& other) = delete;
    IntegerType(IntegerType&& other) noexcept = delete;
    IntegerType& operator=(const IntegerType& other) = delete;
    IntegerType& operator=(IntegerType&& other) noexcept = delete;

    explicit IntegerType(unsigned num_bits, Module *m);
    ~IntegerType() override;

    unsigned get_num_bits() const;

  private:
    unsigned num_bits_;
};

class FunctionType : public Type {
  public:
    FunctionType(const FunctionType& other) = delete;
    FunctionType(FunctionType&& other) noexcept = delete;
    FunctionType& operator=(const FunctionType& other) = delete;
    FunctionType& operator=(FunctionType&& other) noexcept = delete;

    FunctionType(Type *result, const std::vector<Type *>& params);
    ~FunctionType() override;

    static bool is_valid_return_type(const Type *ty);
    static bool is_valid_argument_type(const Type *ty);

    static FunctionType *get(Type *result, std::vector<Type *> params);

    unsigned get_num_of_args() const;

    Type *get_param_type(unsigned i) const;
    std::vector<Type *>::iterator param_begin() { return args_.begin(); }
    std::vector<Type *>::iterator param_end() { return args_.end(); }
    Type *get_return_type() const;

  private:
    Type *result_;
    std::vector<Type *> args_;
};

class ArrayType : public Type {
  public:
    ArrayType(const ArrayType& other) = delete;
    ArrayType(ArrayType&& other) noexcept = delete;
    ArrayType& operator=(const ArrayType& other) = delete;
    ArrayType& operator=(ArrayType&& other) noexcept = delete;

    ArrayType(Type *contained, unsigned num_elements);
    ~ArrayType() override;

    static bool is_valid_element_type(const Type *ty);

    static ArrayType *get(Type *contained, unsigned num_elements);

    Type *get_element_type() const { return contained_; }
    unsigned get_num_of_elements() const { return num_elements_; }

  private:
    Type *contained_;       // The element type of the array.
    unsigned num_elements_; // Number of elements in the array.
};

class PointerType : public Type {
  public:
    PointerType(const PointerType& other) = delete;
    PointerType(PointerType&& other) noexcept = delete;
    PointerType& operator=(const PointerType& other) = delete;
    PointerType& operator=(PointerType&& other) noexcept = delete;

    PointerType(Type *contained);
    ~PointerType() override;
    Type *get_element_type() const { return contained_; }

    static PointerType *get(Type *contained);

  private:
    Type *contained_; // The element type of the ptr.
};

class FloatType : public Type {
  public:
    FloatType(const FloatType& other) = delete;
    FloatType(FloatType&& other) noexcept = delete;
    FloatType& operator=(const FloatType& other) = delete;
    FloatType& operator=(FloatType&& other) noexcept = delete;

    FloatType(Module *m);
    ~FloatType() override;
    static FloatType *get(Module *m);
};
