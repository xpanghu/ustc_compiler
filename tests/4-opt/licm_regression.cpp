#include "Constant.hpp"
#include "Dominators.hpp"
#include "LICM.hpp"
#include "Module.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

static void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static Function* function(Module& m, const char* name, std::vector<Type*> args = {})
{
    return Function::create(m.get_function_type(m.get_int32_type(), args), name, &m);
}

static void verify(Function* f)
{
    f->check_for_block_relation_error();
    Dominators dom(f);
    dom.run();
    for (auto bb : f->get_basic_blocks())
    {
        std::unordered_set<Instruction*> seen;
        for (auto inst : bb->get_instructions())
        {
            check(inst->get_parent() == bb, "instruction parent mismatch");
            if (inst->is_phi())
            {
                std::unordered_set<BasicBlock*> incoming;
                for (auto pair : static_cast<PhiInst*>(inst)->get_phi_pairs())
                {
                    incoming.insert(pair.second);
                    auto def = dynamic_cast<Instruction*>(pair.first);
                    if (def) check(dom.is_dominate(def->get_parent(), pair.second), "phi dominance broken");
                }
                std::unordered_set<BasicBlock*> predecessors(bb->get_pre_basic_blocks().begin(), bb->get_pre_basic_blocks().end());
                check(incoming == predecessors, "phi predecessors mismatch");
                check(inst->get_num_operand() == predecessors.size() * 2, "duplicate phi predecessor");
            }
            for (unsigned i = 0; i < inst->get_num_operand(); ++i)
            {
                auto operand = inst->get_operand(i);
                bool found = false;
                for (auto use : operand->get_use_list())
                    if (use.val_ == inst && use.arg_no_ == i) found = true;
                check(found, "operand use list mismatch");
                auto def = dynamic_cast<Instruction*>(operand);
                if (!def || inst->is_phi()) continue;
                if (def->get_parent() == bb) check(seen.count(def), "definition moved after its use");
                else check(dom.is_dominate(def->get_parent(), bb), "operand dominance broken");
            }
            seen.insert(inst);
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
        Module m;
        auto i32 = m.get_int32_type();
        auto zero = ConstantInt::get(0, &m);
        auto one = ConstantInt::get(1, &m);
        auto two = ConstantInt::get(2, &m);
        auto ten = ConstantInt::get(10, &m);
        auto stable = GlobalVariable::create("stable", &m, i32, false, one);
        auto modified = GlobalVariable::create("modified", &m, i32, false, zero);
        auto io = function(m, "input");
        auto pure = function(m, "pure", {i32});
        auto pure_entry = BasicBlock::create(&m, "entry", pure);
        ReturnInst::create_ret(IBinaryInst::create_mul(pure->get_args().front(), two, pure_entry), pure_entry);
        auto reader = function(m, "reader");
        auto reader_entry = BasicBlock::create(&m, "entry", reader);
        ReturnInst::create_ret(LoadInst::create_load(stable, reader_entry), reader_entry);
        auto writer = function(m, "writer");
        auto writer_entry = BasicBlock::create(&m, "entry", writer);
        StoreInst::create_store(one, modified, writer_entry);
        ReturnInst::create_ret(one, writer_entry);
        auto changing_reader = function(m, "changing_reader");
        auto changing_entry = BasicBlock::create(&m, "entry", changing_reader);
        ReturnInst::create_ret(LoadInst::create_load(modified, changing_entry), changing_entry);

        // Existing preheader, transitive invariants, calls, and a varying load address.
        auto basic = function(m, "basic", {i32});
        auto n = basic->get_args().front();
        auto entry = BasicBlock::create(&m, "entry", basic);
        auto header = BasicBlock::create(&m, "header", basic);
        auto body = BasicBlock::create(&m, "body", basic);
        auto tail = BasicBlock::create(&m, "tail", basic);
        auto exit = BasicBlock::create(&m, "exit", basic);
        auto array = AllocaInst::create_alloca(m.get_array_type(i32, 10), entry);
        BranchInst::create_br(header, entry);
        auto index = PhiInst::create_phi(i32, header);
        index->add_phi_pair_operand(zero, entry);
        BranchInst::create_cond_br(ICmpInst::create_lt(index, n, header), body, exit, header);
        auto product = IBinaryInst::create_mul(n, two, body);
        auto sum = IBinaryInst::create_add(product, one, body);
        auto read_call = CallInst::create_call(reader, {}, body);
        auto changing_call = CallInst::create_call(changing_reader, {}, body);
        auto write_call = CallInst::create_call(writer, {}, body);
        auto io_call = CallInst::create_call(io, {}, body);
        auto stable_load = LoadInst::create_load(stable, body);
        auto changing_load = LoadInst::create_load(modified, body);
        auto address = GetElementPtrInst::create_gep(array, {zero, index}, body);
        auto varying_load = LoadInst::create_load(address, body);
        auto allocation = AllocaInst::create_alloca(i32, body);
        auto next = IBinaryInst::create_add(index, one, body);
        BranchInst::create_br(tail, body);
        // LoopDetection visits tail before body: this use needs another fixed-point round.
        auto pure_call = CallInst::create_call(pure, {sum}, tail);
        BranchInst::create_br(header, tail);
        index->add_phi_pair_operand(next, tail);
        ReturnInst::create_ret(index, exit);

        // Two outside predecessors, one critical edge, and two backedges.
        auto multi = function(m, "multi", {i32});
        auto me = BasicBlock::create(&m, "entry", multi);
        auto left = BasicBlock::create(&m, "left", multi);
        auto right = BasicBlock::create(&m, "right", multi);
        auto mh = BasicBlock::create(&m, "header", multi);
        auto mb = BasicBlock::create(&m, "body", multi);
        auto latch1 = BasicBlock::create(&m, "latch1", multi);
        auto latch2 = BasicBlock::create(&m, "latch2", multi);
        auto mx = BasicBlock::create(&m, "exit", multi);
        auto condition = ICmpInst::create_gt(multi->get_args().front(), zero, me);
        BranchInst::create_cond_br(condition, left, right, me);
        BranchInst::create_cond_br(condition, mh, mx, left);
        BranchInst::create_br(mh, right);
        auto counter = PhiInst::create_phi(i32, mh, {zero, one}, {left, right});
        BranchInst::create_cond_br(ICmpInst::create_lt(counter, ten, mh), mb, mx, mh);
        auto multi_invariant = IBinaryInst::create_mul(multi->get_args().front(), two, mb);
        BranchInst::create_cond_br(condition, latch1, latch2, mb);
        auto next1 = IBinaryInst::create_add(counter, one, latch1);
        BranchInst::create_br(mh, latch1);
        counter->add_phi_pair_operand(next1, latch1);
        auto next2 = IBinaryInst::create_add(counter, two, latch2);
        BranchInst::create_br(mh, latch2);
        counter->add_phi_pair_operand(next2, latch2);
        ReturnInst::create_ret(zero, mx);

        // A single outside predecessor with another successor must also be split.
        auto guarded = function(m, "guarded", {i32});
        auto ge = BasicBlock::create(&m, "entry", guarded);
        auto gh = BasicBlock::create(&m, "header", guarded);
        auto gx = BasicBlock::create(&m, "exit", guarded);
        auto guard = ICmpInst::create_gt(guarded->get_args().front(), zero, ge);
        BranchInst::create_cond_br(guard, gh, gx, ge);
        auto gi = PhiInst::create_phi(i32, gh, {zero}, {ge});
        auto guarded_invariant = IBinaryInst::create_mul(guarded->get_args().front(), two, gh);
        auto gn = IBinaryInst::create_add(gi, one, gh);
        BranchInst::create_cond_br(ICmpInst::create_lt(gn, ten, gh), gh, gx, gh);
        gi->add_phi_pair_operand(gn, gh);
        ReturnInst::create_ret(zero, gx);

        // Pointer arguments can alias even when their roots differ.
        auto alias = function(m, "alias", {m.get_int32_ptr_type(), m.get_int32_ptr_type()});
        auto ae = BasicBlock::create(&m, "entry", alias);
        auto ah = BasicBlock::create(&m, "header", alias);
        auto ab = BasicBlock::create(&m, "body", alias);
        auto ax = BasicBlock::create(&m, "exit", alias);
        BranchInst::create_br(ah, ae);
        auto ai = PhiInst::create_phi(i32, ah, {zero}, {ae});
        BranchInst::create_cond_br(ICmpInst::create_lt(ai, ten, ah), ab, ax, ah);
        auto alias_load = LoadInst::create_load(alias->get_args().front(), ab);
        auto alias_store = StoreInst::create_store(ai, alias->get_args().back(), ab);
        auto alias_invariant = IBinaryInst::create_add(one, two, ab);
        auto an = IBinaryInst::create_add(ai, one, ab);
        BranchInst::create_br(ah, ab);
        ai->add_phi_pair_operand(an, ab);
        ReturnInst::create_ret(ai, ax);

        // Inner-loop invariant versus invariant of both nested loops.
        auto nested = function(m, "nested", {i32});
        auto ne = BasicBlock::create(&m, "entry", nested);
        auto oh = BasicBlock::create(&m, "outer_header", nested);
        auto ob = BasicBlock::create(&m, "outer_body", nested);
        auto ih = BasicBlock::create(&m, "inner_header", nested);
        auto ib = BasicBlock::create(&m, "inner_body", nested);
        auto ol = BasicBlock::create(&m, "outer_latch", nested);
        auto nx = BasicBlock::create(&m, "exit", nested);
        BranchInst::create_br(oh, ne);
        auto oi = PhiInst::create_phi(i32, oh, {zero}, {ne});
        BranchInst::create_cond_br(ICmpInst::create_lt(oi, ten, oh), ob, nx, oh);
        BranchInst::create_br(ih, ob);
        auto ii = PhiInst::create_phi(i32, ih, {zero}, {ob});
        BranchInst::create_cond_br(ICmpInst::create_lt(ii, ten, ih), ib, ol, ih);
        auto both_invariant = IBinaryInst::create_mul(nested->get_args().front(), two, ib);
        auto inner_invariant = IBinaryInst::create_add(oi, both_invariant, ib);
        auto inn = IBinaryInst::create_add(ii, one, ib);
        BranchInst::create_br(ih, ib);
        ii->add_phi_pair_operand(inn, ib);
        auto on = IBinaryInst::create_add(oi, one, ol);
        BranchInst::create_br(oh, ol);
        oi->add_phi_pair_operand(on, ol);
        ReturnInst::create_ret(oi, nx);

        LoopInvariantCodeMotion pass(&m);
        pass.run();
        check(product->get_parent() == entry && sum->get_parent() == entry && pure_call->get_parent() == entry, "transitive invariant not hoisted");
        check(read_call->get_parent() == entry && stable_load->get_parent() == entry, "readonly invariant not hoisted");
        check(changing_call->get_parent() == body && changing_load->get_parent() == body, "modified memory incorrectly hoisted");
        check(io_call->get_parent() == body && write_call->get_parent() == body, "side effect incorrectly hoisted");
        check(varying_load->get_parent() == body && address->get_parent() == body && allocation->get_parent() == body, "variant address or allocation hoisted");
        check(basic->get_num_basic_blocks() == 5, "existing preheader not reused");
        auto preheader = multi_invariant->get_parent();
        check(preheader != mb && preheader != left && preheader != right, "missing dedicated preheader");
        check(preheader->get_pre_basic_blocks().size() == 2 && preheader->get_succ_basic_blocks().front() == mh, "preheader CFG incorrect");
        check(counter->get_num_operand() == 6 && preheader->get_instructions().front()->is_phi(), "header phi not split");
        check(latch1->get_succ_basic_blocks().front() == mh && latch2->get_succ_basic_blocks().front() == mh, "backedge redirected");
        auto guarded_preheader = guarded_invariant->get_parent();
        check(guarded_preheader != ge && guarded_preheader != gh, "single critical edge not split");
        check(guarded_preheader->get_instructions().size() == 2 && gi->get_num_operand() == 4, "single-entry phi incorrectly split");
        check(alias_load->get_parent() == ab && alias_store->get_parent() == ab && alias_invariant->get_parent() == ae, "alias handling incorrect");
        check(both_invariant->get_parent() == ne && inner_invariant->get_parent() == ob, "nested loop hoisting incorrect");
        for (auto f : m.get_functions()) if (!f->is_declaration()) verify(f);
        if (argc == 2 && std::string(argv[1]) == "--emit-ir") std::cout << m.print();
        else std::cout << "LICM regression: 7 groups passed (dependencies, memory/calls, preheader reuse, phi/CFG, critical edge, aliases, nested loops)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "LICM regression failed: " << error.what() << '\n';
        return 1;
    }
}
