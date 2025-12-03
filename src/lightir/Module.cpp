#include "Module.hpp"
#include "Function.hpp"
#include "GlobalVariable.hpp"

#include <memory>
#include <string>

Module::Module() {
    void_ty_ = new Type(Type::VoidTyID, this);
    label_ty_ = new Type(Type::LabelTyID, this);
    int1_ty_ = new IntegerType(1, this);
    int32_ty_ = new IntegerType(32, this);
    float32_ty_ = new FloatType(this);
}

Module::~Module()
{
    delete void_ty_;
    delete label_ty_;
    delete int1_ty_;
    delete int32_ty_;
    delete float32_ty_;
    for (auto& i : pointer_map_) delete i.second;
    for (auto& i : array_map_) delete i.second;
    for (auto& i : function_map_) delete i.second;
    for (auto i : function_list_) delete i;
    for (auto i : global_list_) delete i;
}

Type* Module::get_void_type() const { return void_ty_; }
Type* Module::get_label_type() const { return label_ty_; }
IntegerType* Module::get_int1_type() const { return int1_ty_; }
IntegerType* Module::get_int32_type() const { return int32_ty_; }
FloatType* Module::get_float_type() const { return float32_ty_; }
PointerType* Module::get_int32_ptr_type() {
    return get_pointer_type(int32_ty_);
}
PointerType* Module::get_float_ptr_type() {
    return get_pointer_type(float32_ty_);
}

PointerType* Module::get_pointer_type(Type* contained) {
    if (pointer_map_.find(contained) == pointer_map_.end()) {
        pointer_map_[contained] = new PointerType(contained);
    }
    return pointer_map_[contained];
}

ArrayType* Module::get_array_type(Type* contained, unsigned num_elements) {
    if (array_map_.find({ contained, num_elements }) == array_map_.end()) {
        array_map_[{contained, num_elements}] =
            new ArrayType(contained, num_elements);
    }
    return array_map_[{contained, num_elements}];
}

FunctionType* Module::get_function_type(Type* retty,
    std::vector<Type*>& args) {
    if (not function_map_.count({ retty, args })) {
        function_map_[{retty, args}] =
            new FunctionType(retty, args);
    }
    return function_map_[{retty, args}];
}

void Module::add_function(Function* f) { function_list_.push_back(f); }
std::list<Function*>& Module::get_functions() { return function_list_; }
void Module::add_global_variable(GlobalVariable* g) {
    global_list_.push_back(g);
}
std::list<GlobalVariable*>& Module::get_global_variable() {
    return global_list_;
}

void Module::set_print_name() {
    for (auto func : this->get_functions()) {
        func->set_instr_name();
    }
    return;
}

std::string Module::print() {
    set_print_name();
    std::string module_ir;
    for (auto global_val : this->global_list_) {
        module_ir += global_val->print();
        module_ir += "\n";
    }
    for (auto func : this->function_list_) {
        module_ir += func->print();
        module_ir += "\n";
    }
    return module_ir;
}
