#include "Constant.hpp"

#include <cstring>

#include "Module.hpp"

#include <memory>
#include <sstream>
#include <unordered_map>

struct pair_hash {
    template <typename T>
    std::size_t operator()(const std::pair<T, Module *> val) const {
        auto lhs = std::hash<T>()(val.first);
        auto rhs =
            std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(val.second));
        return lhs ^ rhs;
    }
};

static std::unordered_map<std::pair<int, Module *>,
                          std::unique_ptr<ConstantInt>, pair_hash>
    cached_int;
static std::unordered_map<std::pair<bool, Module *>,
                          std::unique_ptr<ConstantInt>, pair_hash>
    cached_bool;
static std::unordered_map<std::pair<float, Module *>,
                          std::unique_ptr<ConstantFP>, pair_hash>
    cached_float;
static std::unordered_map<Type *, std::unique_ptr<ConstantZero>> cached_zero;

Constant::~Constant() = default;

std::string Constant::safe_print() const
{
    Type* ty = this->get_type();
    return (ty == nullptr ? "<null>" : get_type()->safe_print()) + " " + safe_print_help();
}

std::string Constant::safe_print_help() const
{
    if (dynamic_cast<const ConstantZero*>(this))
    {
        return "zeroinitializer";
    }
    if (auto constant = dynamic_cast<const ConstantInt*>(this))
    {
        Type* ty = this->get_type();
        if (ty != nullptr && ty->is_int1_type()) {
            return constant->get_value() == 0 ? "false" : "true";
        }
        return std::to_string(constant->get_value());
    }
    if (auto constant = dynamic_cast<const ConstantFP*>(this))
    {
        std::stringstream fp_ir_ss;
        std::string fp_ir;
        double val = constant->get_value();
        uint64_t uval;
        memcpy(&uval, &val, sizeof(double));
        fp_ir_ss << "0x" << std::hex << uval << '\n';
        fp_ir_ss >> fp_ir;
        return std::to_string(val) + "(" + fp_ir + ")";
    }
    if (auto constant = dynamic_cast<const ConstantArray*>(this))
    {
        std::string const_ir = "[";
        for (int i = 0; i < constant->get_size_of_array(); i++) {
            Constant* element = constant->get_element_value(i);
            const_ir += (element == nullptr ? "<null>" : element->safe_print_help());
            if (i < constant->get_size_of_array()) {
                const_ir += ", ";
            }
        }
        const_ir += "]";
        return const_ir;
    }
    return "<error>";
}

ConstantInt *ConstantInt::get(int val, Module *m) {
    if (cached_int.find(std::make_pair(val, m)) != cached_int.end())
        return cached_int[std::make_pair(val, m)].get();
    ConstantInt* ret = (cached_int[std::make_pair(val, m)] = std::unique_ptr<ConstantInt>(
                new ConstantInt(m->get_int32_type(), val)))
        .get();
    return ret;
}
ConstantInt *ConstantInt::get(bool val, Module *m) {
    if (cached_bool.find(std::make_pair(val, m)) != cached_bool.end())
        return cached_bool[std::make_pair(val, m)].get();
    auto ret = (cached_bool[std::make_pair(val, m)] = std::unique_ptr<ConstantInt>(
                new ConstantInt(m->get_int1_type(), val ? 1 : 0)))
        .get();
    return ret;
}
std::string ConstantInt::print() {
    std::string const_ir;
    Type *ty = this->get_type();
    if (ty->is_integer_type() &&
        dynamic_cast<IntegerType *>(ty)->get_num_bits() == 1) {
        // int1
        const_ir += (this->get_value() == 0) ? "false" : "true";
    } else {
        // int32
        const_ir += std::to_string(this->get_value());
    }
    return const_ir;
}

ConstantArray::ConstantArray(ArrayType *ty, const std::vector<Constant *> &val)
    : Constant(ty, "") {
    for (unsigned i = 0; i < val.size(); i++)
        set_operand(i, val[i]);
    this->const_array.assign(val.begin(), val.end());
}

Constant *ConstantArray::get_element_value(int index) const
{
    return this->const_array[index];
}

ConstantArray *ConstantArray::get(ArrayType *ty,
                                  const std::vector<Constant *> &val) {
    auto ret =  new ConstantArray(ty, val);
    return ret;
}

std::string ConstantArray::print() {
    std::string const_ir;
    const_ir += this->get_type()->print();
    const_ir += " ";
    const_ir += "[";
    for (int i = 0; i < this->get_size_of_array(); i++) {
        Constant *element = get_element_value(i);
        if (!dynamic_cast<ConstantArray *>(get_element_value(i))) {
            const_ir += element->get_type()->print();
        }
        const_ir += element->print();
        if (i < this->get_size_of_array()) {
            const_ir += ", ";
        }
    }
    const_ir += "]";
    return const_ir;
}

ConstantFP *ConstantFP::get(float val, Module *m) {
    if (cached_float.find(std::make_pair(val, m)) != cached_float.end())
        return cached_float[std::make_pair(val, m)].get();
    auto ret = (cached_float[std::make_pair(val, m)] = std::unique_ptr<ConstantFP>(
                new ConstantFP(m->get_float_type(), val)))
        .get();
    return ret;
}

std::string ConstantFP::print() {
    std::stringstream fp_ir_ss;
    std::string fp_ir;
    double val = this->get_value();
    uint64_t uval;
    memcpy(&uval, &val, sizeof(double));
    fp_ir_ss << "0x" << std::hex << uval << '\n';
    fp_ir_ss >> fp_ir;
    return fp_ir;
}

ConstantZero *ConstantZero::get(Type *ty, Module *m) {
    if (not cached_zero[ty])
    {
        cached_zero[ty] = std::unique_ptr<ConstantZero>(new ConstantZero(ty));
    }
    return cached_zero[ty].get();
}

std::string ConstantZero::print() { return "zeroinitializer"; }
