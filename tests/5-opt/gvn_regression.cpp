#include "GVN.hpp"
#include "Dominators.hpp"
#include "CodeGen.hpp"
#include "DeadCode.hpp"

#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

static void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

static Function *function(Module &m, std::vector<Type *> args = {}) {
    return Function::create(m.get_function_type(m.get_int32_type(), args), "main", &m);
}

static void verify(Function *f) {
    f->check_for_block_relation_error();
    Dominators dom(f);
    dom.run();
    std::unordered_set<User *> users;
    std::unordered_set<Value *> values;
    for (auto bb : f->get_basic_blocks()) {
        check(bb->is_terminated(), "unterminated block");
        std::unordered_set<Instruction *> seen;
        for (auto inst : bb->get_instructions()) {
            users.insert(inst);
            values.insert(inst);
            if (auto phi = dynamic_cast<PhiInst *>(inst)) {
                std::set<BasicBlock *> incoming;
                for (auto [value, pred] : phi->get_phi_pairs()) {
                    incoming.insert(pred);
                    check(value->get_type() == phi->get_type(), "PHI type mismatch");
                    if (auto def = dynamic_cast<Instruction *>(value))
                        check(dom.is_dominate(def->get_parent(), pred), "PHI edge dominance broken");
                }
                std::set<BasicBlock *> preds(bb->get_pre_basic_blocks().begin(), bb->get_pre_basic_blocks().end());
                check(incoming == preds && incoming.size() * 2 == phi->get_num_operand(), "PHI edge mismatch");
            }
            for (unsigned i = 0; i < inst->get_num_operand(); ++i) {
                auto value = inst->get_operand(i);
                check(value, "null operand");
                values.insert(value);
                unsigned count = 0;
                for (auto use : value->get_use_list())
                    if (use.val_ == inst && use.arg_no_ == i) ++count;
                check(count == 1, "operand/use-list mismatch");
                auto def = dynamic_cast<Instruction *>(value);
                if (!def || inst->is_phi()) continue;
                if (def->get_parent() == bb) check(seen.count(def), "use before definition");
                else check(dom.is_dominate(def->get_parent(), bb), "operand dominance broken");
            }
            seen.insert(inst);
        }
    }
    for (auto value : values)
        for (auto use : value->get_use_list())
            if (users.count(use.val_))
                check(use.val_->get_operand(use.arg_no_) == value, "stale use-list entry");
}

static void optimize(Module &m, Function *f) {
    GVN pass(&m, false);
    pass.run();
    verify(f);
}

static void expressions(Module &m) {
    GVN pass(&m, false);
    auto boolean = pass.valueExpr({}, ConstantInt::get(true, &m));
    auto integer = pass.valueExpr({}, ConstantInt::get(1, &m));
    check(boolean && integer, "constants need typed expression identities");
    check(!(boolean == integer), "i1 and i32 constants merged");
    auto plus = pass.valueExpr({}, ConstantFP::get(0.0f, &m));
    auto minus = pass.valueExpr({}, ConstantFP::get(-0.0f, &m));
    check(!(plus == minus), "signed zero merged");
    uint32_t payload1 = 0x7fc00001, payload2 = 0x7fc00002;
    float nan1, nan2;
    std::memcpy(&nan1, &payload1, sizeof nan1);
    std::memcpy(&nan2, &payload2, sizeof nan2);
    check(!(pass.valueExpr({}, ConstantFP::get(nan1, &m)) == pass.valueExpr({}, ConstantFP::get(nan2, &m))),
          "NaN payloads merged");
    auto f = function(m, {m.get_int32_type(), m.get_int32_type()});
    auto bb = BasicBlock::create(&m, "entry", f);
    auto a = f->get_args().front(), b = f->get_args().back();
    check(!(pass.valueExpr({}, a) == pass.valueExpr({}, b)), "unknown identities merged");
    auto ab = IBinaryInst::create_sub(a, b, bb);
    auto again = IBinaryInst::create_sub(a, b, bb);
    auto ba = IBinaryInst::create_sub(b, a, bb);
    check(pass.valueExpr({}, ab) == pass.valueExpr({}, again), "equal typed operations differ");
    check(!(pass.valueExpr({}, ab) == pass.valueExpr({}, ba)), "sub operands were commuted");
    ReturnInst::create_ret(ab, bb);
}

static void partitions(Module &m) {
    auto a = ConstantInt::get(1, &m), b = ConstantInt::get(2, &m);
    auto x = GVN::createCongruenceClass(1), y = GVN::createCongruenceClass(20);
    x->members_ = y->members_ = {a, b};
    x->leader_ = a;
    y->leader_ = b;
    check(GVN::partitions{x} == GVN::partitions{y}, "display numbers/leaders must not determine equality");
    auto copy = GVN::clone({x});
    (*copy.begin())->members_.erase(a);
    check(x->members_.size() == 2, "clone modified original snapshot");
    x->value_phi_ = std::make_shared<GVNExpression::Expression>(GVNExpression::Expression::phi, "i32", Instruction::phi,
                                                              3, std::vector<std::string>{}, std::vector<size_t>{1, 2}, std::vector<size_t>{1, 2});
    y->value_phi_ = std::make_shared<GVNExpression::Expression>(GVNExpression::Expression::phi, "i32", Instruction::phi,
                                                              3, std::vector<std::string>{}, std::vector<size_t>{2, 1}, std::vector<size_t>{1, 2});
    check(!(GVN::partitions{x} == GVN::partitions{y}), "changed PHI edge facts were ignored");
    GVNExpression::Expr empty;
    check(empty == empty, "absent expression must be reflexively equal");
}

static void scalar(Module &m) {
    auto f = function(m, {m.get_int32_type(), m.get_int32_type(), m.get_int32_type()});
    auto bb = BasicBlock::create(&m, "entry", f);
    auto it = f->get_args().begin();
    auto a = *it++, b = *it++, c = *it;
    auto p = IBinaryInst::create_add(a, b, bb, "p");
    auto q = IBinaryInst::create_add(b, a, bb, "q");
    auto x = IBinaryInst::create_mul(p, c, bb, "x");
    auto y = IBinaryInst::create_mul(q, c, bb, "y");
    auto sum = IBinaryInst::create_add(x, y, bb);
    ReturnInst::create_ret(sum, bb);
    optimize(m, f);
    check(sum->get_operand(0) == sum->get_operand(1), "scalar chain CSE missing");
    check(y->get_operand(0) == p, "commutative add CSE missing");
    auto before = f->print();
    optimize(m, f);
    check(before == f->print(), "GVN is not idempotent");
}

static void availability(Module &m) {
    auto f = function(m, {m.get_int32_type()});
    auto output = Function::create(FunctionType::get(m.get_void_type(), {m.get_int32_type()}), "output", &m);
    auto bb = BasicBlock::create(&m, "entry", f);
    auto a = f->get_args().front();
    auto one = ConstantInt::get(1, &m);
    auto early = IBinaryInst::create_add(a, one, bb, "early");
    auto use = CallInst::create_call(output, {early}, bb);
    auto late = IBinaryInst::create_add(a, one, bb, "late");
    auto ret = ReturnInst::create_ret(late, bb);
    auto cc = GVN::createCongruenceClass(1);
    cc->leader_ = late;
    cc->members_ = {early, late};
    GVN(&m, false).replace_cc_members({{bb, {cc}}});
    check(use->get_operand(1) == early, "late leader replaced earlier use");
    optimize(m, f);
    check(ret->get_operand(0) == early, "earlier representative was not used");
}

static void join(Module &m) {
    auto f = function(m, {m.get_int32_type(), m.get_int32_type()});
    auto it = f->get_args().begin();
    auto x = *it++, y = *it;
    auto a = GVN::createCongruenceClass(1), b = GVN::createCongruenceClass(2);
    a->members_ = {x, y, ConstantInt::get(1, &m)};
    b->members_ = {x, y, ConstantInt::get(2, &m)};
    GVN pass(&m, false);
    auto merged = pass.join({a}, {b}, nullptr, nullptr);
    check(merged.size() == 1 && (*merged.begin())->members_ == std::set<Value *>{x, y}, "meet must intersect members");
    check(pass.join({GVN::createCongruenceClass(0)}, {a}, nullptr, nullptr) == GVN::partitions{a}, "TOP must be meet identity");
}

static void loops(Module &m) {
    auto f = function(m, {m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f);
    auto header = BasicBlock::create(&m, "header", f);
    auto body = BasicBlock::create(&m, "body", f);
    auto exit = BasicBlock::create(&m, "exit", f);
    auto n = f->get_args().front();
    auto zero = ConstantInt::get(0, &m), one = ConstantInt::get(1, &m);
    BranchInst::create_br(header, entry);
    auto i = PhiInst::create_phi(m.get_int32_type(), header, {}, {}, "i");
    auto x = PhiInst::create_phi(m.get_int32_type(), header, {}, {}, "x");
    auto y = PhiInst::create_phi(m.get_int32_type(), header, {}, {}, "y");
    i->add_phi_pair_operand(zero, entry);
    x->add_phi_pair_operand(one, entry);
    y->add_phi_pair_operand(ConstantInt::get(2, &m), entry);
    BranchInst::create_cond_br(ICmpInst::create_lt(i, n, header), body, exit, header);
    auto next = IBinaryInst::create_add(i, one, body, "next");
    auto duplicate = IBinaryInst::create_add(i, one, body, "duplicate");
    auto sink = IBinaryInst::create_add(next, duplicate, body);
    auto output = Function::create(FunctionType::get(m.get_void_type(), {m.get_int32_type()}), "output", &m);
    CallInst::create_call(output, {sink}, body);
    BranchInst::create_br(header, body);
    i->add_phi_pair_operand(next, body);
    x->add_phi_pair_operand(y, body);
    y->add_phi_pair_operand(x, body);
    auto result = IBinaryInst::create_add(i, IBinaryInst::create_sub(x, y, exit), exit);
    ReturnInst::create_ret(result, exit);
    optimize(m, f);
    check(sink->get_operand(0) == sink->get_operand(1), "loop-local CSE missing");
    check(dynamic_cast<Constant *>(result->get_operand(0)) == nullptr, "induction variable became initial constant");
    check(x->get_operand(2) == y && y->get_operand(2) == x, "PHI assignments were not parallel");
    auto before = f->print();
    optimize(m, f);
    check(before == f->print(), "loop GVN is not idempotent");
}

static void phi_of_ops(Module &m, bool mismatch = false, bool reorder = false) {
    auto f = function(m, {m.get_int32_type(), m.get_int32_type(), m.get_int32_type(), m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f);
    auto left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f);
    auto merge = BasicBlock::create(&m, "merge", f);
    auto it = f->get_args().begin();
    auto al = *it++, bl = *it++, ar = *it++, br = *it;
    BranchInst::create_cond_br(ICmpInst::create_ne(al, ConstantInt::get(0, &m), entry), left, right, entry);
    auto cl = IBinaryInst::create_add(al, bl, left, "cl");
    BranchInst::create_br(merge, left);
    auto cr = mismatch ? IBinaryInst::create_sub(ar, br, right, "cr") : IBinaryInst::create_add(ar, br, right, "cr");
    BranchInst::create_br(merge, right);
    auto a = PhiInst::create_phi(m.get_int32_type(), merge, {al, ar}, {left, right}, "a");
    auto b = PhiInst::create_phi(m.get_int32_type(), merge, reorder ? std::vector<Value *>{br, bl} : std::vector<Value *>{bl, br},
                                reorder ? std::vector<BasicBlock *>{right, left} : std::vector<BasicBlock *>{left, right}, "b");
    auto c = PhiInst::create_phi(m.get_int32_type(), merge, {cl, cr}, {left, right}, "c");
    auto d = IBinaryInst::create_add(a, b, merge, "d");
    auto result = IBinaryInst::create_add(c, d, merge);
    ReturnInst::create_ret(result, merge);
    if (reorder) {
        f->get_basic_blocks().remove(right);
        f->get_basic_blocks().push_back(right);
    }
    optimize(m, f);
    check((result->get_operand(0) == result->get_operand(1)) == !mismatch, "PHI-of-ops proof incorrect");
}

static void effects(Module &m) {
    auto pure = Function::create(FunctionType::get(m.get_int32_type(), {m.get_int32_type()}), "pure", &m);
    auto pb = BasicBlock::create(&m, "entry", pure);
    ReturnInst::create_ret(IBinaryInst::create_add(pure->get_args().front(), ConstantInt::get(1, &m), pb), pb);
    auto global = GlobalVariable::create("g", &m, m.get_int32_type(), false, ConstantInt::get(0, &m));
    auto read = Function::create(FunctionType::get(m.get_int32_type(), {}), "read", &m);
    auto rb = BasicBlock::create(&m, "entry", read);
    ReturnInst::create_ret(LoadInst::create_load(global, rb), rb);
    auto input = Function::create(FunctionType::get(m.get_int32_type(), {}), "input", &m);
    auto f = function(m, {m.get_int32_type()});
    auto bb = BasicBlock::create(&m, "entry", f);
    auto arg = f->get_args().front();
    auto first = CallInst::create_call(pure, {arg}, bb);
    auto second = CallInst::create_call(pure, {arg}, bb);
    auto sum = IBinaryInst::create_add(first, second, bb);
    auto in1 = CallInst::create_call(input, {}, bb), in2 = CallInst::create_call(input, {}, bb);
    auto inputs = IBinaryInst::create_add(in1, in2, bb);
    auto r1 = CallInst::create_call(read, {}, bb);
    StoreInst::create_store(arg, global, bb);
    auto r2 = CallInst::create_call(read, {}, bb);
    auto reads = IBinaryInst::create_add(r1, r2, bb);
    auto array = AllocaInst::create_alloca(m.get_array_type(m.get_int32_type(), 2), bb);
    auto other = AllocaInst::create_alloca(m.get_array_type(m.get_int32_type(), 2), bb);
    auto zero = ConstantInt::get(0, &m);
    auto p = GetElementPtrInst::create_gep(array, {zero, arg}, bb);
    auto q = GetElementPtrInst::create_gep(array, {zero, arg}, bb);
    auto distinct = GetElementPtrInst::create_gep(other, {zero, arg}, bb);
    auto load1 = LoadInst::create_load(p, bb);
    auto shared_store = StoreInst::create_store(arg, q, bb);
    auto load2 = LoadInst::create_load(p, bb);
    auto loads = IBinaryInst::create_add(load1, load2, bb);
    auto separate_store = StoreInst::create_store(arg, distinct, bb);
    auto result = IBinaryInst::create_add(sum, IBinaryInst::create_add(inputs, reads, bb), bb);
    ReturnInst::create_ret(IBinaryInst::create_add(result, loads, bb), bb);
    optimize(m, f);
    check(sum->get_operand(0) == sum->get_operand(1), "pure calls were not reused");
    check(inputs->get_operand(0) != inputs->get_operand(1), "impure calls merged");
    check(reads->get_operand(0) != reads->get_operand(1), "global-reading calls merged");
    check(loads->get_operand(0) != loads->get_operand(1), "loads across store merged");
    check(shared_store->get_operand(1) == p, "GEP was not reused");
    check(separate_store->get_operand(1) != p, "distinct allocations merged");
}

static void three_predecessors(Module &m) {
    auto f = function(m, {m.get_int32_type(), m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f);
    auto split = BasicBlock::create(&m, "split", f);
    auto left = BasicBlock::create(&m, "left", f), middle = BasicBlock::create(&m, "middle", f);
    auto right = BasicBlock::create(&m, "right", f), merge = BasicBlock::create(&m, "merge", f);
    auto a = f->get_args().front(), b = f->get_args().back();
    auto cond = ICmpInst::create_ne(a, ConstantInt::get(0, &m), entry);
    BranchInst::create_cond_br(cond, left, split, entry);
    BranchInst::create_cond_br(cond, middle, right, split);
    auto l = IBinaryInst::create_add(a, b, left);
    BranchInst::create_br(merge, left);
    auto mid = IBinaryInst::create_add(a, b, middle);
    BranchInst::create_br(merge, middle);
    auto r = IBinaryInst::create_sub(a, b, right);
    BranchInst::create_br(merge, right);
    auto c = PhiInst::create_phi(m.get_int32_type(), merge, {l, mid, r}, {left, middle, right});
    auto d = IBinaryInst::create_add(a, b, merge);
    auto result = IBinaryInst::create_add(c, d, merge);
    ReturnInst::create_ret(result, merge);
    optimize(m, f);
    check(result->get_operand(0) != result->get_operand(1), "only two of three incoming edges proved equality");
    check(d->get_operand(0) == a && d->get_operand(1) == b, "branch-local value escaped its edge");
}

static void multi_backedge(Module &m) {
    auto f = function(m, {m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f), header = BasicBlock::create(&m, "header", f);
    auto left = BasicBlock::create(&m, "left", f), right = BasicBlock::create(&m, "right", f);
    auto choice = BasicBlock::create(&m, "choice", f), exit = BasicBlock::create(&m, "exit", f);
    auto n = f->get_args().front();
    auto zero = ConstantInt::get(0, &m), one = ConstantInt::get(1, &m);
    BranchInst::create_br(header, entry);
    auto i = PhiInst::create_phi(m.get_int32_type(), header);
    auto same = PhiInst::create_phi(m.get_int32_type(), header);
    i->add_phi_pair_operand(zero, entry);
    same->add_phi_pair_operand(zero, entry);
    BranchInst::create_cond_br(ICmpInst::create_lt(i, n, header), choice, exit, header);
    BranchInst::create_cond_br(ICmpInst::create_eq(i, one, choice), left, right, choice);
    auto l = IBinaryInst::create_add(i, one, left), r = IBinaryInst::create_add(i, ConstantInt::get(2, &m), right);
    BranchInst::create_br(header, left);
    BranchInst::create_br(header, right);
    i->add_phi_pair_operand(l, left); i->add_phi_pair_operand(r, right);
    same->add_phi_pair_operand(l, left); same->add_phi_pair_operand(r, right);
    auto result = IBinaryInst::create_sub(i, same, exit);
    ReturnInst::create_ret(result, exit);
    optimize(m, f);
    check(result->get_operand(0) == result->get_operand(1), "equivalent PHIs with multiple backedges missed");
    check(dynamic_cast<Constant *>(result->get_operand(0)) == nullptr, "multi-backedge induction folded to initializer");
    auto before = f->print();
    optimize(m, f);
    check(before == f->print(), "multi-backedge analysis failed to stabilize");
}

static void folded_phi_ops(Module &m) {
    auto f = function(m, {m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f), left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f), merge = BasicBlock::create(&m, "merge", f);
    auto one = ConstantInt::get(1, &m), two = ConstantInt::get(2, &m);
    BranchInst::create_cond_br(ICmpInst::create_eq(f->get_args().front(), one, entry), left, right, entry);
    auto l = IBinaryInst::create_add(one, two, left), r = IBinaryInst::create_add(two, two, right);
    BranchInst::create_br(merge, left); BranchInst::create_br(merge, right);
    auto a = PhiInst::create_phi(m.get_int32_type(), merge, {one, two}, {left, right});
    auto c = PhiInst::create_phi(m.get_int32_type(), merge, {l, r}, {left, right});
    auto d = IBinaryInst::create_add(a, two, merge);
    auto result = IBinaryInst::create_add(c, d, merge);
    ReturnInst::create_ret(result, merge);
    optimize(m, f);
    check(result->get_operand(0) == result->get_operand(1), "folding discarded PHI-of-ops facts");
}

static void unreachable_edge(Module &m) {
    auto f = function(m, {m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f), merge = BasicBlock::create(&m, "merge", f);
    auto dead = BasicBlock::create(&m, "dead", f);
    auto arg = f->get_args().front();
    BranchInst::create_br(merge, entry);
    BranchInst::create_br(merge, dead);
    auto phi = PhiInst::create_phi(m.get_int32_type(), merge, {arg, ConstantInt::get(99, &m)}, {entry, dead});
    auto result = IBinaryInst::create_add(phi, ConstantInt::get(1, &m), merge);
    ReturnInst::create_ret(result, merge);
    GVN(&m, false).run();
    check(result->get_operand(0) == arg, "unreachable predecessor prevented trivial PHI proof");
    check(phi->get_operand(2) == ConstantInt::get(99, &m), "unreachable incoming value was rewritten");
}

static void distinct_joins(Module &m) {
    auto f = function(m, {m.get_int32_type(), m.get_int32_type(), m.get_int32_type()});
    auto entry = BasicBlock::create(&m, "entry", f), left = BasicBlock::create(&m, "left", f);
    auto right = BasicBlock::create(&m, "right", f), first = BasicBlock::create(&m, "first", f);
    auto left2 = BasicBlock::create(&m, "left2", f), right2 = BasicBlock::create(&m, "right2", f);
    auto second = BasicBlock::create(&m, "second", f);
    auto it = f->get_args().begin();
    auto a = *it++, b = *it++, c = *it;
    auto zero = ConstantInt::get(0, &m);
    BranchInst::create_cond_br(ICmpInst::create_ne(a, zero, entry), left, right, entry);
    BranchInst::create_br(first, left); BranchInst::create_br(first, right);
    auto x = PhiInst::create_phi(m.get_int32_type(), first, {a, b}, {left, right});
    BranchInst::create_cond_br(ICmpInst::create_ne(c, zero, first), left2, right2, first);
    BranchInst::create_br(second, left2); BranchInst::create_br(second, right2);
    auto y = PhiInst::create_phi(m.get_int32_type(), second, {a, b}, {left2, right2});
    auto result = IBinaryInst::create_sub(x, y, second);
    ReturnInst::create_ret(result, second);
    optimize(m, f);
    check(result->get_operand(0) != result->get_operand(1), "PHIs from independent joins merged");
}

static void unknown_pointer(Module &m) {
    auto callee = Function::create(FunctionType::get(m.get_int32_type(), {m.get_int32_type()}), "indirect_read", &m);
    auto cb = BasicBlock::create(&m, "entry", callee);
    auto data = AllocaInst::create_alloca(m.get_int32_type(), cb);
    auto slot = AllocaInst::create_alloca(m.get_int32_ptr_type(), cb);
    StoreInst::create_store(callee->get_args().front(), data, cb);
    StoreInst::create_store(data, slot, cb);
    auto ptr = LoadInst::create_load(slot, cb);
    ReturnInst::create_ret(LoadInst::create_load(ptr, cb), cb);
    FuncInfo info(&m); info.run();
    check(info.is_pure(callee), "test must expose the pointer-origin gap in FuncInfo");
    auto f = function(m, {m.get_int32_type()});
    auto bb = BasicBlock::create(&m, "entry", f);
    auto a = CallInst::create_call(callee, {f->get_args().front()}, bb);
    auto b = CallInst::create_call(callee, {f->get_args().front()}, bb);
    auto result = IBinaryInst::create_add(a, b, bb);
    ReturnInst::create_ret(result, bb);
    optimize(m, f);
    check(result->get_operand(0) != result->get_operand(1), "unknown pointer-origin call was treated as proven pure");
}

static void conversions(Module &m) {
    auto f = function(m);
    auto bb = BasicBlock::create(&m, "entry", f);
    GVNExpression::ConstFolder folder(&m);
    auto boolean = ConstantInt::get(true, &m);
    auto signed_i1 = SiToFpInst::create_sitofp(boolean, bb);
    check(folder.compute(signed_i1, boolean) == nullptr, "signed i1 conversion must not fold as +1");
    auto fp = ConstantFP::get(1.0f, &m);
    auto narrow = FpToSiInst::create_fptosi(fp, m.get_int1_type(), bb);
    check(folder.compute(narrow, fp) == nullptr, "fptosi i1 requires different range checks");
    auto wide = ZextInst::create_zext_to_i32(boolean, bb);
    auto value = dynamic_cast<ConstantInt *>(folder.compute(wide, boolean));
    check(value && value->get_type()->is_int32_type() && value->get_value() == 1, "zext i1 to i32 fold missing");
    ReturnInst::create_ret(wide, bb);
}

static void dump(Module &m) {
    struct Directory {
        std::filesystem::path previous = std::filesystem::current_path();
        std::filesystem::path temporary = std::filesystem::temp_directory_path() /
            ("jianmu-gvn-dump-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Directory() { std::filesystem::create_directory(temporary); std::filesystem::current_path(temporary); }
        ~Directory() { std::filesystem::current_path(previous); std::filesystem::remove_all(temporary); }
    } directory;
    auto f = Function::create(FunctionType::get(m.get_int32_type(), {m.get_float_type()}), "name\"\\\n", &m);
    auto bb = BasicBlock::create(&m, "block\"\n", f);
    auto sum = FBinaryInst::create_fadd(f->get_args().front(), ConstantFP::get(-0.0f, &m), bb, "sum\"");
    auto ret = FpToSiInst::create_fptosi_to_i32(sum, bb);
    ReturnInst::create_ret(ret, bb);
    GVN(&m, true).run();
    std::ifstream file("gvn.json");
    std::ostringstream buffer; buffer << file.rdbuf();
    auto text = buffer.str();
    check(text.find("name\\\"\\\\\\u000a") != std::string::npos, "JSON name escaping failed");
    check(text.find("float:0x80000000") != std::string::npos, "JSON lost float sign bit");
    check(text.find("sum\\\"") != std::string::npos, "JSON member escaping failed");
}

static void emit_phi_codegen(Module &m) {
    auto f = function(m);
    auto output = Function::create(FunctionType::get(m.get_void_type(), {m.get_int32_type()}), "output", &m);
    auto output_float = Function::create(FunctionType::get(m.get_void_type(), {m.get_float_type()}), "outputFloat", &m);
    auto entry = BasicBlock::create(&m, "entry", f), loop = BasicBlock::create(&m, "loop", f);
    auto done = BasicBlock::create(&m, "done", f);
    auto zero = ConstantInt::get(0, &m), one = ConstantInt::get(1, &m), two = ConstantInt::get(2, &m);
    BranchInst::create_br(loop, entry);
    auto i = PhiInst::create_phi(m.get_int32_type(), loop, {}, {}, "i");
    auto a = PhiInst::create_phi(m.get_int32_type(), loop, {}, {}, "a");
    auto b = PhiInst::create_phi(m.get_int32_type(), loop, {}, {}, "b");
    auto x = PhiInst::create_phi(m.get_float_type(), loop, {}, {}, "x");
    auto y = PhiInst::create_phi(m.get_float_type(), loop, {}, {}, "y");
    i->add_phi_pair_operand(zero, entry);
    a->add_phi_pair_operand(one, entry); b->add_phi_pair_operand(two, entry);
    x->add_phi_pair_operand(ConstantFP::get(1.5f, &m), entry);
    y->add_phi_pair_operand(ConstantFP::get(2.5f, &m), entry);
    auto next = IBinaryInst::create_add(i, one, loop, "next");
    i->add_phi_pair_operand(next, loop);
    a->add_phi_pair_operand(b, loop); b->add_phi_pair_operand(a, loop);
    x->add_phi_pair_operand(y, loop); y->add_phi_pair_operand(x, loop);
    BranchInst::create_cond_br(ICmpInst::create_lt(i, ConstantInt::get(3, &m), loop), loop, done, loop);
    CallInst::create_call(output, {i}, done);
    CallInst::create_call(output, {a}, done); CallInst::create_call(output, {b}, done);
    CallInst::create_call(output_float, {x}, done); CallInst::create_call(output_float, {y}, done);
    ReturnInst::create_ret(zero, done);
    verify(f);
    for (std::string mode : {"raw", "gvn"}) {
        if (mode == "gvn") { GVN(&m, false).run(); DeadCode(&m, false).run(); verify(f); }
        std::ofstream ir(mode + ".ll"); ir << m.print();
        CodeGen codegen(&m); codegen.run();
        std::ofstream assembly(mode + ".s"); assembly << codegen.print();
        check(ir.good() && assembly.good(), "cannot write PHI codegen fixture");
    }
}

int main(int argc, char **argv) {
    try {
        std::string group = argc > 1 ? argv[1] : "all";
        if (group == "emit-phi-codegen") { Module m; emit_phi_codegen(m); return 0; }
        unsigned count = 0;
        // The existing constant cache keys by Module address; keep fixtures alive to avoid address reuse.
        std::vector<std::unique_ptr<Module>> modules;
        for (auto [name, test] : std::vector<std::pair<std::string, void (*)(Module &)>>{
                 {"expressions", expressions}, {"partitions", partitions}, {"scalar", scalar}, {"availability", availability},
                 {"join", join}, {"loops", loops}, {"effects", effects}, {"three-predecessors", three_predecessors},
                 {"multi-backedge", multi_backedge}, {"folded-phi-ops", folded_phi_ops}, {"dump", dump}, {"unreachable", unreachable_edge}, {"distinct-joins", distinct_joins},
                 {"unknown-pointer", unknown_pointer}, {"conversions", conversions},
                 {"phi-of-ops", [](Module &m) { phi_of_ops(m); }},
                 {"phi-mismatch", [](Module &m) { phi_of_ops(m, true); }},
                 {"phi-reorder", [](Module &m) { phi_of_ops(m, false, true); }}}) {
            if (group == "all" || group == name) { modules.push_back(std::make_unique<Module>()); test(*modules.back()); ++count; }
        }
        check(count != 0, "unknown regression group");
        std::cout << "GVN regression: " << count << " groups passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "GVN regression failed: " << e.what() << '\n';
        return 1;
    }
}
