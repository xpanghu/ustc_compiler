#pragma once

#include "Function.hpp"
#include "GlobalVariable.hpp"
#include "Type.hpp"

#include <list>
#include <map>
#include <memory>
#include <string>

class GlobalVariable;
class Function;
class Module {
  public:
    Module();
    ~Module();
    Module(const Module& other) = delete;
    Module(Module&& other) noexcept = delete;
    Module& operator=(const Module& other) = delete;
    Module& operator=(Module&& other) noexcept = delete;

    Type *get_void_type() const;
    Type *get_label_type() const;
    IntegerType *get_int1_type() const;
    IntegerType *get_int32_type() const;
    PointerType *get_int32_ptr_type();
    FloatType *get_float_type() const;
    PointerType *get_float_ptr_type();

    PointerType *get_pointer_type(Type *contained);
    ArrayType *get_array_type(Type *contained, unsigned num_elements);
    FunctionType *get_function_type(Type *retty, std::vector<Type *> &args);

    void add_function(Function *f);
    std::list<Function*> &get_functions();
    void add_global_variable(GlobalVariable *g);
    std::list<GlobalVariable*> &get_global_variable();

    void set_print_name();
    std::string print();

  private:
    // The global variables in the module
    std::list<GlobalVariable*> global_list_;
    // The functions in the module
    std::list<Function*> function_list_;

    IntegerType* int1_ty_;
    IntegerType* int32_ty_;
    Type* label_ty_;
    Type* void_ty_;
    FloatType* float32_ty_;
    std::map<Type *, PointerType*> pointer_map_;
    std::map<std::pair<Type *, int>, ArrayType*> array_map_;
    std::map<std::pair<Type *, std::vector<Type *>>,
             FunctionType*>
        function_map_;
};
