#include "ConstPropagation.hpp"
#include "GVN.hpp"
#include "DeadCode.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

static Function *function(Module &m, const char *name, std::vector<Type *> args = {}) {
    return Function::create(m.get_function_type(m.get_int32_type(), args), name, &m);
}

static void test_float(Module &m) {
    ConstFolder folder(&m);
    auto one = ConstantFP::get(1.0f, &m);
    auto two = ConstantFP::get(2.0f, &m);
    auto nan = ConstantFP::get(std::numeric_limits<float>::quiet_NaN(), &m);
    for (auto op : {Instruction::feq, Instruction::fne, Instruction::fgt,
                    Instruction::fge, Instruction::flt, Instruction::fle}) {
        auto result = folder.compute(op, one, two);
        check(result && result->get_type() == m.get_int1_type(), "floating comparison must return i1");
        auto unordered = dynamic_cast<ConstantInt *>(folder.compute(op, nan, one));
        check(unordered && unordered->get_value() == 1, "unordered comparison with NaN must be true");
    }
    check(folder.compute(Instruction::fptosi, nan) == nullptr, "NaN fptosi must not fold");
    auto inf = ConstantFP::get(std::numeric_limits<float>::infinity(), &m);
    check(folder.compute(Instruction::fptosi, inf) == nullptr, "infinite fptosi must not fold");
    auto limit = ConstantFP::get(2147483648.0f, &m);
    check(folder.compute(Instruction::fptosi, limit) == nullptr, "out-of-range fptosi must not fold");
    auto valid = folder.compute(Instruction::fptosi, ConstantFP::get(-3.75f, &m));
    check(valid && valid->get_value() == -3, "fptosi truncates toward zero");
}

static void test_integer(Module &m) {
    ConstFolder folder(&m);
    auto zero = ConstantInt::get(0, &m);
    auto one = ConstantInt::get(1, &m);
    auto minus_one = ConstantInt::get(-1, &m);
    auto max = ConstantInt::get(std::numeric_limits<int>::max(), &m);
    auto min = ConstantInt::get(std::numeric_limits<int>::min(), &m);
    check(folder.compute(Instruction::add, one, one)->get_value() == 2, "integer add baseline");
    check(folder.compute(Instruction::add, max, one) == nullptr, "overflow add must not invoke C++ UB");
    check(folder.compute(Instruction::sub, min, one) == nullptr, "overflow sub must not invoke C++ UB");
    check(folder.compute(Instruction::mul, max, max) == nullptr, "overflow mul must not invoke C++ UB");
    check(folder.compute(Instruction::sdiv, one, zero) == nullptr, "division by zero must not fold");
    check(folder.compute(Instruction::sdiv, min, minus_one) == nullptr, "overflow division must not fold");
    check(folder.compute(Instruction::lt, one, max)->get_type() == m.get_int1_type(), "integer comparison result type");
}

static void test_gvn_folder(Module &m) {
    auto f = function(m, "fold");
    auto bb = BasicBlock::create(&m, "entry", f);
    auto one = ConstantInt::get(1, &m);
    auto zero = ConstantInt::get(0, &m);
    GVNExpression::ConstFolder folder(&m);
    auto div = IBinaryInst::create_sdiv(one, zero, bb);
    check(folder.compute(div, one, zero) == nullptr, "GVN folder must guard division by zero");
    auto nan = ConstantFP::get(std::numeric_limits<float>::quiet_NaN(), &m);
    auto fp_one = ConstantFP::get(1.0f, &m);
    auto cmp = FCmpInst::create_fne(nan, fp_one, bb);
    auto folded = dynamic_cast<ConstantInt *>(folder.compute(cmp, nan, fp_one));
    check(folded && folded->get_value() == 1, "GVN unordered fne NaN semantics");
    auto boolean = ConstantInt::get(true, &m);
    auto zext = ZextInst::create_zext(boolean, m.get_int32_type(), bb);
    auto widened = dynamic_cast<ConstantInt *>(folder.compute(zext, boolean));
    check(widened && widened->get_value() == 1 && widened->get_type() == m.get_int32_type(), "GVN zext remains i32");
    ReturnInst::create_ret(one, bb);
}

static void test_phi(Module &m) {
    auto f = function(m, "edge", {m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f);
    auto left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f);
    auto merge = BasicBlock::create(&m, "merge", f);
    auto arg = f->get_args().front();
    auto zero = ConstantInt::get(0, &m);
    auto leader = ConstantInt::get(7, &m);
    BranchInst::create_cond_br(ICmpInst::create_ne(arg, zero, entry), left, right, entry);
    auto local = IBinaryInst::create_add(arg, zero, left);
    BranchInst::create_br(merge, left);
    auto other = IBinaryInst::create_add(arg, zero, right);
    BranchInst::create_br(merge, right);
    auto phi = PhiInst::create_phi(m.get_int32_type(), merge);
    phi->add_phi_pair_operand(arg, left);
    phi->add_phi_pair_operand(arg, right);
    ReturnInst::create_ret(phi, merge);
    auto cc = GVN::createCongruenceClass(1);
    cc->leader_ = leader;
    cc->members_ = {arg, leader};
    GVN gvn(&m, false);
    gvn.replace_cc_members({{left, {cc}}});
    check(phi->get_operand(0) == leader, "replace PHI incoming value on the selected edge");
    check(phi->get_operand(2) == arg, "must not replace PHI value on another predecessor edge");
    check(local->get_operand(0) == leader, "replace ordinary use in selected block");
    check(other->get_operand(0) == arg, "must not replace ordinary use in another block");
    bool remaining_use = false;
    for (const auto &use : arg->get_use_list())
        if (use.val_ == phi && use.arg_no_ == 2) remaining_use = true;
    check(remaining_use, "PHI operand/use-list relationship must be preserved");
}

static void test_top(Module &m) {
    check(!GVN::isTop({}), "empty partition is not top");
    check(GVN::isTop({GVN::createCongruenceClass(0)}), "top sentinel is recognized");
}

static void test_scalar(Module &m) {
    auto f = function(m, "main", {m.get_int32_type()});
    auto bb = BasicBlock::create(&m, "entry", f);
    auto arg = f->get_args().front();
    auto one = ConstantInt::get(1, &m);
    auto first = IBinaryInst::create_add(arg, one, bb);
    auto duplicate = IBinaryInst::create_add(arg, one, bb);
    auto result = IBinaryInst::create_mul(first, duplicate, bb);
    ReturnInst::create_ret(result, bb);
    GVN(&m, false).run();
    check(result->get_operand(0) == result->get_operand(1), "GVN must reuse duplicate scalar computation");
    DeadCode(&m, false).run();
    unsigned adds = 0;
    for (auto &inst : bb->get_instructions())
        if (inst->is_add()) ++adds;
    check(adds == 1, "explicit DCE must remove the duplicate add");
}

int main(int argc, char **argv) {
    try {
        std::string group = argc > 1 ? argv[1] : "all";
        unsigned count = 0;
        Module m;
        for (const auto &[name, test] : std::vector<std::pair<std::string, void (*)(Module &)>>{
                 {"float", test_float}, {"integer", test_integer}, {"gvn-folder", test_gvn_folder},
                 {"phi", test_phi}, {"top", test_top}, {"scalar", test_scalar}}) {
            if (group == "all" || group == name) { test(m); ++count; }
        }
        check(count != 0, "unknown regression group");
        std::cout << "Lab5 scaffold regression: " << count << " groups passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Lab5 scaffold regression failed: " << e.what() << '\n';
        return 1;
    }
}
