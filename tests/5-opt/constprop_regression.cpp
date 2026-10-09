#include "ConstPropagation.hpp"
#include "Dominators.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

static Function *function(Module &m, const char *name, std::vector<Type *> args = {}) {
    return Function::create(m.get_function_type(m.get_int32_type(), args), name, &m);
}

static void verify(Function *f) {
    f->check_for_block_relation_error();
    Dominators dom(f);
    dom.run();
    std::unordered_set<User *> users;
    std::unordered_set<Value *> values;
    for (auto bb : f->get_basic_blocks()) {
        check(bb->is_terminated(), "unterminated basic block");
        std::unordered_set<Instruction *> seen;
        for (auto inst : bb->get_instructions()) {
            users.insert(inst);
            values.insert(inst);
            check(inst->get_parent() == bb, "instruction parent mismatch");
            if (inst->is_phi()) {
                std::unordered_set<BasicBlock *> incoming;
                for (auto [value, pred] : static_cast<PhiInst *>(inst)->get_phi_pairs()) {
                    incoming.insert(pred);
                    check(value && value->get_type() == inst->get_type(), "PHI value/type mismatch");
                    if (auto def = dynamic_cast<Instruction *>(value))
                        check(dom.is_dominate(def->get_parent(), pred), "PHI dominance broken");
                }
                std::unordered_set<BasicBlock *> preds(bb->get_pre_basic_blocks().begin(), bb->get_pre_basic_blocks().end());
                check(incoming == preds, "PHI incoming blocks differ from predecessors");
                check(inst->get_num_operand() == preds.size() * 2, "duplicate PHI predecessor");
            }
            for (unsigned i = 0; i < inst->get_num_operand(); ++i) {
                auto value = inst->get_operand(i);
                check(value != nullptr, "null operand left after deletion");
                values.insert(value);
                unsigned count = 0;
                for (auto use : value->get_use_list())
                    if (use.val_ == inst && use.arg_no_ == i) ++count;
                check(count == 1, "operand use-list mismatch");
                auto def = dynamic_cast<Instruction *>(value);
                if (!def || inst->is_phi()) continue;
                if (def->get_parent() == bb) check(seen.count(def), "use precedes its definition");
                else check(dom.is_dominate(def->get_parent(), bb), "operand dominance broken");
            }
            seen.insert(inst);
        }
    }
    for (auto value : values)
        for (auto use : value->get_use_list())
            if (users.count(use.val_))
                check(use.arg_no_ < use.val_->get_num_operand() && use.val_->get_operand(use.arg_no_) == value,
                      "stale use-list entry");
}

static void run(Module &m, Function *f) {
    ConstPropagation pass(&m);
    pass.run();
    verify(f);
    auto before = f->print();
    pass.run();
    verify(f);
    check(before == f->print(), "ConstPropagation is not idempotent");
}

static void returned(ReturnInst *ret, int expected) {
    auto value = dynamic_cast<ConstantInt *>(ret->get_operand(0));
    check(value && value->get_value() == expected, "return value was not propagated to expected constant");
}

static void scalar(Module &m) {
    auto f = function(m, "scalar");
    auto bb = BasicBlock::create(&m, "entry", f);
    auto two = ConstantInt::get(2, &m), three = ConstantInt::get(3, &m);
    auto add = IBinaryInst::create_add(two, three, bb);
    auto mul = IBinaryInst::create_mul(add, ConstantInt::get(4, &m), bb);
    auto cmp = ICmpInst::create_eq(mul, ConstantInt::get(20, &m), bb);
    auto zext = ZextInst::create_zext_to_i32(cmp, bb);
    auto fp = SiToFpInst::create_sitofp(zext, bb);
    auto sum = FBinaryInst::create_fadd(fp, ConstantFP::get(2.75f, &m), bb);
    auto trunc = FpToSiInst::create_fptosi_to_i32(sum, bb);
    auto ret = ReturnInst::create_ret(trunc, bb);
    run(m, f);
    returned(ret, 3);
    check(bb->get_num_of_instr() == 1, "folded scalar instructions were not erased");
}

static void floating(Module &m) {
    auto nan = ConstantFP::get(std::numeric_limits<float>::quiet_NaN(), &m);
    unsigned index = 0;
    for (auto make : {FCmpInst::create_fge, FCmpInst::create_fgt, FCmpInst::create_fle,
                      FCmpInst::create_flt, FCmpInst::create_feq, FCmpInst::create_fne}) {
        auto f = function(m, ("nan" + std::to_string(index++)).c_str());
        auto bb = BasicBlock::create(&m, "entry", f);
        auto cmp = make(nan, ConstantFP::get(1.0f, &m), bb, "");
        auto ret = ReturnInst::create_ret(ZextInst::create_zext_to_i32(cmp, bb), bb);
        run(m, f);
        returned(ret, 1);
        check(bb->get_num_of_instr() == 1, "floating comparison not erased");
    }
    auto f = function(m, "float_chain");
    auto bb = BasicBlock::create(&m, "entry", f);
    auto a = FBinaryInst::create_fsub(ConstantFP::get(9.0f, &m), ConstantFP::get(1.0f, &m), bb);
    auto b = FBinaryInst::create_fmul(a, ConstantFP::get(2.0f, &m), bb);
    auto c = FBinaryInst::create_fdiv(b, ConstantFP::get(4.0f, &m), bb);
    auto ret = ReturnInst::create_ret(FpToSiInst::create_fptosi_to_i32(c, bb), bb);
    run(m, f);
    returned(ret, 4);
}

static void operators(Module &m) {
    auto a = ConstantInt::get(-9, &m), b = ConstantInt::get(2, &m);
    unsigned index = 0;
    for (auto [make, expected] : std::vector<std::pair<IBinaryInst *(*)(Value *, Value *, BasicBlock *, const std::string &), int>>{
             {IBinaryInst::create_add, -7}, {IBinaryInst::create_sub, -11},
             {IBinaryInst::create_mul, -18}, {IBinaryInst::create_sdiv, -4}}) {
        auto f = function(m, ("integer_op" + std::to_string(index++)).c_str());
        auto bb = BasicBlock::create(&m, "entry", f);
        auto ret = ReturnInst::create_ret(make(a, b, bb, ""), bb);
        run(m, f);
        returned(ret, expected);
        check(bb->get_num_of_instr() == 1, "integer operation not erased");
    }
    for (auto [make, expected] : std::vector<std::pair<ICmpInst *(*)(Value *, Value *, BasicBlock *, const std::string &), int>>{
             {ICmpInst::create_ge, 0}, {ICmpInst::create_gt, 0}, {ICmpInst::create_le, 1},
             {ICmpInst::create_lt, 1}, {ICmpInst::create_eq, 0}, {ICmpInst::create_ne, 1}}) {
        auto f = function(m, ("integer_cmp" + std::to_string(index++)).c_str());
        auto bb = BasicBlock::create(&m, "entry", f);
        auto ret = ReturnInst::create_ret(ZextInst::create_zext_to_i32(make(a, b, bb, ""), bb), bb);
        run(m, f);
        returned(ret, expected);
        check(bb->get_num_of_instr() == 1, "integer comparison not erased");
    }
    for (auto [make, expected] : std::vector<std::pair<FCmpInst *(*)(Value *, Value *, BasicBlock *, const std::string &), int>>{
             {FCmpInst::create_fge, 0}, {FCmpInst::create_fgt, 0}, {FCmpInst::create_fle, 1},
             {FCmpInst::create_flt, 1}, {FCmpInst::create_feq, 0}, {FCmpInst::create_fne, 1}}) {
        auto f = function(m, ("float_cmp" + std::to_string(index++)).c_str());
        auto bb = BasicBlock::create(&m, "entry", f);
        auto cmp = make(ConstantFP::get(-9.0f, &m), ConstantFP::get(2.0f, &m), bb, "");
        auto ret = ReturnInst::create_ret(ZextInst::create_zext_to_i32(cmp, bb), bb);
        run(m, f);
        returned(ret, expected);
        check(bb->get_num_of_instr() == 1, "finite floating comparison not erased");
    }
}

static void unsafe(Module &m) {
    auto f = function(m, "unsafe");
    auto bb = BasicBlock::create(&m, "entry", f);
    auto zero = ConstantInt::get(0, &m), one = ConstantInt::get(1, &m);
    auto min = ConstantInt::get(std::numeric_limits<int>::min(), &m);
    auto max = ConstantInt::get(std::numeric_limits<int>::max(), &m);
    auto add = IBinaryInst::create_add(max, one, bb);
    auto sub = IBinaryInst::create_sub(min, one, bb);
    auto mul = IBinaryInst::create_mul(max, max, bb);
    auto div = IBinaryInst::create_sdiv(one, zero, bb);
    auto overflow = IBinaryInst::create_sdiv(min, ConstantInt::get(-1, &m), bb);
    auto nan = FpToSiInst::create_fptosi_to_i32(ConstantFP::get(std::numeric_limits<float>::quiet_NaN(), &m), bb);
    auto inf = FpToSiInst::create_fptosi_to_i32(ConstantFP::get(std::numeric_limits<float>::infinity(), &m), bb);
    auto limit = FpToSiInst::create_fptosi_to_i32(ConstantFP::get(2147483648.0f, &m), bb);
    auto ret = ReturnInst::create_ret(IBinaryInst::create_sub(ConstantInt::get(9, &m), ConstantInt::get(2, &m), bb), bb);
    run(m, f);
    for (auto inst : {add, sub, mul, div, overflow}) check(inst->get_parent() == bb, "unsafe integer fold deleted instruction");
    for (auto inst : {nan, inf, limit}) check(inst->get_parent() == bb, "invalid float conversion deleted instruction");
    returned(ret, 7);
}

static void signed_zero(Module &m) {
    auto positive = ConstantFP::get(0.0f, &m);
    auto f = function(m, "signed_zero");
    auto bb = BasicBlock::create(&m, "entry", f);
    auto product = FBinaryInst::create_fmul(positive, ConstantFP::get(-1.0f, &m), bb);
    auto quotient = FBinaryInst::create_fdiv(ConstantFP::get(1.0f, &m), product, bb);
    auto cmp = FCmpInst::create_flt(quotient, ConstantFP::get(0.0f, &m), bb);
    auto ret = ReturnInst::create_ret(ZextInst::create_zext_to_i32(cmp, bb), bb);
    run(m, f);
    returned(ret, 1); // 1 / -0 is -infinity, not +infinity.
    auto negative = ConstantFP::get(-0.0f, &m);
    check(positive != negative && !std::signbit(positive->get_value()) && std::signbit(negative->get_value()),
          "floating constant cache merged signed zeros");

    auto phi_f = function(m, "signed_zero_phi", {m.get_int1_type()});
    auto entry = BasicBlock::create(&m, "entry", phi_f), left = BasicBlock::create(&m, "left", phi_f);
    auto right = BasicBlock::create(&m, "right", phi_f), merge = BasicBlock::create(&m, "merge", phi_f);
    BranchInst::create_cond_br(phi_f->get_args().front(), left, right, entry);
    BranchInst::create_br(merge, left);
    BranchInst::create_br(merge, right);
    auto p = PhiInst::create_phi(m.get_float_type(), merge, {positive, negative}, {left, right});
    auto conversion = FpToSiInst::create_fptosi_to_i32(p, merge);
    ReturnInst::create_ret(conversion, merge);
    run(m, phi_f);
    check(conversion->get_operand(0) == p, "PHI merged +0 and -0 using numeric equality");
}

static void phi(Module &m) {
    auto f = function(m, "equal_phi", {m.get_int1_type()});
    auto entry = BasicBlock::create(&m, "entry", f);
    auto left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f);
    auto merge = BasicBlock::create(&m, "merge", f);
    BranchInst::create_cond_br(f->get_args().front(), left, right, entry);
    BranchInst::create_br(merge, left);
    BranchInst::create_br(merge, right);
    auto value = ConstantInt::get(5, &m);
    auto p = PhiInst::create_phi(m.get_int32_type(), merge, {value, value}, {left, right});
    auto ret = ReturnInst::create_ret(IBinaryInst::create_mul(p, ConstantInt::get(4, &m), merge), merge);
    run(m, f);
    returned(ret, 20);
    check(merge->get_num_of_instr() == 1, "identical constant PHI not simplified");
    check(static_cast<BranchInst *>(entry->get_terminator())->is_cond_br(), "unknown condition incorrectly folded");

    auto single = function(m, "single_phi", {m.get_int32_type()});
    auto se = BasicBlock::create(&m, "entry", single);
    auto sx = BasicBlock::create(&m, "exit", single);
    BranchInst::create_br(sx, se);
    auto arg = single->get_args().front();
    auto sr = ReturnInst::create_ret(PhiInst::create_phi(m.get_int32_type(), sx, {arg}, {se}), sx);
    run(m, single);
    check(sr->get_operand(0) == arg, "single-input variable PHI not simplified");
}

static void fixedpoint(Module &m) {
    auto f = function(m, "fixedpoint");
    auto entry = BasicBlock::create(&m, "entry", f);
    // Deliberately place users before definitions in the function's block list.
    auto merge = BasicBlock::create(&m, "merge", f);
    auto live = BasicBlock::create(&m, "live", f);
    auto dead = BasicBlock::create(&m, "dead", f);
    BranchInst::create_cond_br(ICmpInst::create_lt(ConstantInt::get(1, &m), ConstantInt::get(2, &m), entry), live, dead, entry);
    auto value = IBinaryInst::create_add(ConstantInt::get(2, &m), ConstantInt::get(3, &m), live);
    BranchInst::create_br(merge, live);
    BranchInst::create_br(merge, dead);
    auto p = PhiInst::create_phi(m.get_int32_type(), merge, {value, ConstantInt::get(9, &m)}, {live, dead});
    auto ret = ReturnInst::create_ret(IBinaryInst::create_mul(p, ConstantInt::get(4, &m), merge), merge);
    run(m, f);
    returned(ret, 20);
    check(f->get_num_basic_blocks() == 3, "dead arm not deleted");
    check(!static_cast<BranchInst *>(entry->get_terminator())->is_cond_br(), "constant branch not rewritten");
}

static void live_edge(Module &m) {
    auto f = function(m, "live_edge", {m.get_int1_type(), m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f);
    auto a = BasicBlock::create(&m, "a", f), b = BasicBlock::create(&m, "b", f);
    auto merge = BasicBlock::create(&m, "merge", f), other = BasicBlock::create(&m, "other", f);
    BranchInst::create_cond_br(f->get_args().front(), a, b, entry);
    BranchInst::create_cond_br(ConstantInt::get(false, &m), merge, other, a);
    BranchInst::create_br(merge, b);
    auto ret = ReturnInst::create_ret(PhiInst::create_phi(m.get_int32_type(), merge,
        {ConstantInt::get(99, &m), f->get_args().back()}, {a, b}), merge);
    ReturnInst::create_ret(ConstantInt::get(0, &m), other);
    run(m, f);
    check(f->get_num_basic_blocks() == 5, "live block removed with dropped edge");
    check(ret->get_operand(0) == f->get_args().back(), "dropped live edge left a PHI input");
}

static void same_target(Module &m) {
    auto f = function(m, "same_target");
    auto entry = BasicBlock::create(&m, "entry", f), exit = BasicBlock::create(&m, "exit", f);
    BranchInst::create_cond_br(ConstantInt::get(true, &m), exit, exit, entry);
    auto ret = ReturnInst::create_ret(PhiInst::create_phi(m.get_int32_type(), exit,
        {ConstantInt::get(8, &m)}, {entry}), exit);
    run(m, f);
    returned(ret, 8);
    check(entry->get_succ_basic_blocks().size() == 1 && exit->get_pre_basic_blocks().size() == 1,
          "same-target rewrite destroyed the retained CFG edge");
    check(!static_cast<BranchInst *>(entry->get_terminator())->is_cond_br(), "same-target constant branch not rewritten");
}

static void dead_cycle(Module &m) {
    auto f = function(m, "dead_cycle");
    auto entry = BasicBlock::create(&m, "entry", f), exit = BasicBlock::create(&m, "exit", f);
    auto a = BasicBlock::create(&m, "cycle_a", f), b = BasicBlock::create(&m, "cycle_b", f);
    BranchInst::create_cond_br(ConstantInt::get(false, &m), a, exit, entry);
    auto p = PhiInst::create_phi(m.get_int32_type(), a, {ConstantInt::get(1, &m)}, {entry});
    BranchInst::create_br(b, a);
    auto next = IBinaryInst::create_add(p, ConstantInt::get(1, &m), b);
    p->add_phi_pair_operand(next, b);
    BranchInst::create_br(a, b);
    auto ret = ReturnInst::create_ret(ConstantInt::get(7, &m), exit);
    run(m, f);
    returned(ret, 7);
    check(f->get_num_basic_blocks() == 2, "unreachable cycle retained because it has predecessors");
}

static void loop(Module &m) {
    auto f = function(m, "loop", {m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f), header = BasicBlock::create(&m, "header", f);
    auto body = BasicBlock::create(&m, "body", f), exit = BasicBlock::create(&m, "exit", f);
    BranchInst::create_br(header, entry);
    auto p = PhiInst::create_phi(m.get_int32_type(), header, {ConstantInt::get(0, &m)}, {entry});
    BranchInst::create_cond_br(ICmpInst::create_lt(p, f->get_args().front(), header), body, exit, header);
    auto next = IBinaryInst::create_add(p, ConstantInt::get(1, &m), body);
    p->add_phi_pair_operand(next, body);
    BranchInst::create_br(header, body);
    auto ret = ReturnInst::create_ret(p, exit);
    run(m, f);
    check(ret->get_operand(0) == p && p->get_num_operand() == 4, "loop PHI was assumed equal to its initial constant");
    check(static_cast<BranchInst *>(header->get_terminator())->is_cond_br(), "runtime loop condition incorrectly folded");
}

static void memory(Module &m) {
    auto global = GlobalVariable::create("memory_global", &m, m.get_int32_type(), false, ConstantInt::get(0, &m));
    auto f = function(m, "memory");
    auto entry = BasicBlock::create(&m, "entry", f), left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f);
    StoreInst::create_store(ConstantInt::get(5, &m), global, entry);
    auto load = LoadInst::create_load(global, entry);
    BranchInst::create_cond_br(ICmpInst::create_eq(load, ConstantInt::get(5, &m), entry), left, right, entry);
    auto ret = ReturnInst::create_ret(ConstantInt::get(1, &m), left);
    ReturnInst::create_ret(ConstantInt::get(0, &m), right);
    run(m, f);
    returned(ret, 1);
    check(entry->get_num_of_instr() == 2, "same-block global load not forwarded or store lost");
    check(f->get_num_basic_blocks() == 2, "forwarded global did not simplify branch");
}

static void barriers(Module &m) {
    auto global = GlobalVariable::create("barrier_global", &m, m.get_int32_type(), false, ConstantInt::get(0, &m));
    auto callee = function(m, "unknown_call");
    auto f = function(m, "barriers", {m.get_int32_ptr_type(), m.get_int32_type()});
    auto bb = BasicBlock::create(&m, "entry", f);
    StoreInst::create_store(ConstantInt::get(5, &m), global, bb);
    CallInst::create_call(callee, {}, bb);
    auto after_call = LoadInst::create_load(global, bb);
    StoreInst::create_store(ConstantInt::get(5, &m), global, bb);
    StoreInst::create_store(ConstantInt::get(7, &m), f->get_args().front(), bb);
    auto after_alias = LoadInst::create_load(global, bb);
    StoreInst::create_store(ConstantInt::get(5, &m), global, bb);
    StoreInst::create_store(f->get_args().back(), global, bb);
    auto after_unknown = LoadInst::create_load(global, bb);
    ReturnInst::create_ret(IBinaryInst::create_add(IBinaryInst::create_add(after_call, after_alias, bb), after_unknown, bb), bb);
    run(m, f);
    for (auto load : {after_call, after_alias, after_unknown})
        check(load->get_parent() == bb, "global constant survived a clobber barrier");
}

static void cross_block(Module &m) {
    auto global = GlobalVariable::create("cross_global", &m, m.get_int32_type(), false, ConstantInt::get(42, &m));
    auto f = function(m, "cross_block", {m.get_int1_type()});
    auto entry = BasicBlock::create(&m, "entry", f), left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f), merge = BasicBlock::create(&m, "merge", f);
    auto initial = LoadInst::create_load(global, entry);
    BranchInst::create_cond_br(f->get_args().front(), left, right, entry);
    StoreInst::create_store(ConstantInt::get(1, &m), global, left);
    BranchInst::create_br(merge, left);
    StoreInst::create_store(ConstantInt::get(2, &m), global, right);
    BranchInst::create_br(merge, right);
    auto final = LoadInst::create_load(global, merge);
    ReturnInst::create_ret(IBinaryInst::create_add(initial, final, merge), merge);
    run(m, f);
    check(initial->get_parent() == entry, "mutable global initializer assumed at function entry");
    check(final->get_parent() == merge, "global facts leaked across blocks in list order");
}

int main(int argc, char **argv) {
    try {
        Module m;
        std::string group = argc > 1 ? argv[1] : "all";
        unsigned count = 0;
        for (const auto &[name, test] : std::vector<std::pair<std::string, void (*)(Module &)>>{
                 {"scalar", scalar}, {"floating", floating}, {"operators", operators},
                 {"unsafe", unsafe}, {"signed-zero", signed_zero}, {"phi", phi},
                 {"fixedpoint", fixedpoint}, {"live-edge", live_edge}, {"same-target", same_target},
                 {"dead-cycle", dead_cycle}, {"loop", loop}, {"memory", memory},
                 {"barriers", barriers}, {"cross-block", cross_block}}) {
            if (group == "all" || group == name) {
                test(m);
                ++count;
                std::cout << "PASS " << name << '\n';
            }
        }
        check(count != 0, "unknown regression group");
        std::cout << "ConstPropagation regression: " << count << " groups passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ConstPropagation regression failed: " << e.what() << '\n';
        return 1;
    }
}
