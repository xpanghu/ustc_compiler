#include "LoongArchISel.hpp"
#include "Module.hpp"
#include "Constant.hpp"
#include "Instruction.hpp"
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace machine {
namespace {
using O = MachineOperand;
ValueKind value_kind(Type *t) {
    if (t->is_int1_type())
        return ValueKind::I1;
    if (t->is_int32_type())
        return ValueKind::I32;
    if (t->is_float_type())
        return ValueKind::F32;
    if (t->is_pointer_type())
        return ValueKind::Ptr64;
    throw std::runtime_error("MIR unsupported value type " + t->print());
}
uint64_t type_size(Type *t) {
    if (t->is_array_type()) {
        auto *a = static_cast<ArrayType *>(t);
        auto n = type_size(a->get_element_type());
        if (a->get_num_of_elements() && n > uint64_t(INT64_MAX) / a->get_num_of_elements())
            throw std::runtime_error("MIR object size overflow");
        return n * a->get_num_of_elements();
    }
    switch (value_kind(t)) {
    case ValueKind::I1:
        return 1;
    case ValueKind::I32:
    case ValueKind::F32:
        return 4;
    case ValueKind::Ptr64:
        return 8;
    }
    throw std::runtime_error("invalid type");
}
unsigned type_align(Type *t) {
    while (t->is_array_type())
        t = t->get_array_element_type();
    return unsigned(type_size(t));
}
void check_signature(FunctionType *t, const std::string &name) {
    unsigned g = 0, f = 0;
    for (unsigned k = 0; k < t->get_num_of_args(); ++k) {
        if (value_kind(t->get_param_type(k)) == ValueKind::F32)
            ++f;
        else
            ++g;
    }
    if (g > 8 || f > 8)
        throw std::runtime_error("MIR unsupported ABI signature for " + name +
                                 ": at most eight integer/pointer and eight float arguments");
    if (!t->get_return_type()->is_void_type())
        value_kind(t->get_return_type());
}
uint32_t fp_bits(ConstantFP *c) {
    uint32_t b;
    float x = c->get_value();
    std::memcpy(&b, &x, sizeof b);
    return b;
}
void initializer(Constant *c, Type *t, std::vector<uint8_t> &bytes) {
    if (!c || dynamic_cast<ConstantZero *>(c)) {
        bytes.insert(bytes.end(), type_size(t), 0);
        return;
    }
    if (auto *a = dynamic_cast<ConstantArray *>(c)) {
        auto *at = dynamic_cast<ArrayType *>(t);
        if (!at || unsigned(a->get_size_of_array()) != at->get_num_of_elements())
            throw std::runtime_error("MIR malformed array initializer");
        for (int k = 0; k < a->get_size_of_array(); ++k)
            initializer(a->get_element_value(k), at->get_element_type(), bytes);
        return;
    }
    uint64_t bits;
    if (auto *i = dynamic_cast<ConstantInt *>(c))
        bits = static_cast<uint32_t>(i->get_value());
    else if (auto *f = dynamic_cast<ConstantFP *>(c))
        bits = fp_bits(f);
    else
        throw std::runtime_error("MIR unsupported global initializer");
    for (unsigned k = 0; k < type_size(t); ++k)
        bytes.push_back(uint8_t(bits >> (k * 8)));
}
bool signed12(int64_t value) { return value >= -2048 && value <= 2047; }
std::optional<int64_t> constant_gep_offset(Instruction *gep) {
    Type *layer = gep->get_operand(0)->get_type()->get_pointer_element_type();
    __int128 offset = 0;
    bool all_constant = true;
    for (unsigned k = 1; k < gep->get_num_operand(); ++k) {
        if (k > 1) {
            if (!layer->is_array_type())
                throw std::runtime_error("MIR GEP indexes non-array layer");
            layer = layer->get_array_element_type();
        }
        if (!gep->get_operand(k)->get_type()->is_int32_type())
            throw std::runtime_error("MIR GEP requires i32 indices");
        if (auto *constant = dynamic_cast<ConstantInt *>(gep->get_operand(k))) {
            offset += static_cast<__int128>(constant->get_value()) * type_size(layer);
            if (offset < INT64_MIN || offset > INT64_MAX)
                throw std::runtime_error("MIR GEP constant offset overflow");
        } else
            all_constant = false;
    }
    return all_constant ? std::optional<int64_t>(static_cast<int64_t>(offset)) : std::nullopt;
}
struct SelectionPlan {
    Value *immediate_base = nullptr;
    int64_t immediate = 0;
    ICmpInst *comparison = nullptr;
    Value *branch_condition = nullptr;
    Value *address_base = nullptr;
    int64_t address_offset = 0;
};
struct BlockPlans {
    std::map<Instruction *, SelectionPlan> consumers;
    std::set<Instruction *> covered;
};
BlockPlans plan_block(BasicBlock *block, const Options &options) {
    BlockPlans result;
    if (options.selection != SelectionMode::Greedy)
        return result;
    auto local_single_use = [&](Instruction *instruction) {
        return instruction && instruction->get_parent() == block &&
               instruction->get_use_list().size() == 1 && !result.covered.count(instruction);
    };
    auto clear_span = [&](Instruction *producer, Instruction *consumer) {
        bool after = false;
        for (auto *instruction : block->get_instructions()) {
            if (instruction == consumer)
                return after;
            if (instruction == producer) {
                after = true;
                continue;
            }
            if (after &&
                (instruction->is_load() || instruction->is_store() || instruction->is_call()))
                return false;
        }
        return false;
    };
    // Match consumers first, but commit coverage only after the full shape is legal.
    for (auto cursor = block->get_instructions().rbegin();
         cursor != block->get_instructions().rend(); ++cursor) {
        auto *instruction = *cursor;
        if (result.covered.count(instruction))
            continue;
        SelectionPlan plan;
        if (!options.lab4_metrics && instruction->is_br() && instruction->get_num_operand() == 3) {
            Value *condition = instruction->get_operand(0);
            std::vector<Instruction *> chain;
            while (auto *node = dynamic_cast<Instruction *>(condition)) {
                if (!local_single_use(node))
                    break;
                if (node->is_zext() && node->get_type()->is_int32_type() &&
                    node->get_operand(0)->get_type()->is_int1_type()) {
                    chain.push_back(node);
                    condition = node->get_operand(0);
                    continue;
                }
                if (node->get_instr_type() == Instruction::ne) {
                    Value *boolean = nullptr;
                    for (unsigned k = 0; k < 2; ++k)
                        if (auto *zero = dynamic_cast<ConstantInt *>(node->get_operand(k)))
                            if (zero->get_value() == 0) {
                                auto *other = node->get_operand(1 - k);
                                auto *wide = dynamic_cast<ZextInst *>(other);
                                if (other->get_type()->is_int1_type() ||
                                    (wide && wide->get_type()->is_int32_type() &&
                                     wide->get_operand(0)->get_type()->is_int1_type()))
                                    boolean = other;
                            }
                    if (boolean) {
                        chain.push_back(node);
                        condition = boolean;
                        continue;
                    }
                }
                if (auto *comparison = dynamic_cast<ICmpInst *>(node)) {
                    if (comparison->get_operand(0)->get_type()->is_int32_type() &&
                        comparison->get_operand(1)->get_type()->is_int32_type() &&
                        clear_span(comparison, instruction)) {
                        chain.push_back(comparison);
                        plan.comparison = comparison;
                        result.covered.insert(chain.begin(), chain.end());
                    }
                }
                break;
            }
            // A boolean producer need not itself be an integer compare. Keep its
            // computation (including loads/FCC materialization) and remove only
            // the single-use conversions and comparisons against zero.
            auto *root = dynamic_cast<Instruction *>(condition);
            if (!plan.comparison && !chain.empty() && condition->get_type()->is_int1_type() &&
                (!root || root->get_parent() == block) && clear_span(chain.back(), instruction)) {
                plan.branch_condition = condition;
                result.covered.insert(chain.begin(), chain.end());
            }
        }
        if (!options.lab4_metrics && (instruction->is_load() || instruction->is_store())) {
            auto *gep = dynamic_cast<GetElementPtrInst *>(
                instruction->get_operand(instruction->is_load() ? 0 : 1));
            if (local_single_use(gep) && clear_span(gep, instruction)) {
                auto offset = constant_gep_offset(gep);
                if (offset && signed12(*offset)) {
                    plan.address_base = gep->get_operand(0);
                    plan.address_offset = *offset;
                    result.covered.insert(gep);
                }
            }
        }
        if ((instruction->is_add() || instruction->is_sub()) &&
            instruction->get_type()->is_int32_type()) {
            unsigned index = 1;
            auto *constant = dynamic_cast<ConstantInt *>(instruction->get_operand(index));
            if (!constant && instruction->is_add()) {
                index = 0;
                constant = dynamic_cast<ConstantInt *>(instruction->get_operand(index));
            }
            if (constant && constant->get_type()->is_int32_type()) {
                int64_t immediate = instruction->is_sub()
                                        ? -static_cast<int64_t>(constant->get_value())
                                        : constant->get_value();
                if (signed12(immediate)) {
                    plan.immediate_base = instruction->get_operand(1 - index);
                    plan.immediate = immediate;
                }
            }
        }
        if (plan.immediate_base || plan.comparison || plan.branch_condition || plan.address_base)
            result.consumers.emplace(instruction, plan);
    }
    return result;
}
} // namespace
MachineModule select(Module &module, const Options &options) {
    MachineModule result;
    uint64_t alloca_bytes = 0;
    for (auto *f : module.get_functions()) {
        check_signature(f->get_function_type(), f->get_name());
        if (options.lab4_metrics)
            for (auto *b : f->get_basic_blocks())
                for (auto *i : b->get_instructions())
                    if (auto *a = dynamic_cast<AllocaInst *>(i)) {
                        auto n = type_size(a->get_alloca_type());
                        if (n > uint64_t(INT32_MAX) - alloca_bytes)
                            throw std::runtime_error("MIR metrics alloca total exceeds i32");
                        alloca_bytes += n;
                    }
    }
    for (auto *g : module.get_global_variable()) {
        auto *t = g->get_type()->get_pointer_element_type();
        MachineGlobal out;
        out.name = g->get_name();
        out.size = type_size(t);
        out.alignment = type_align(t);
        out.is_constant = g->is_const();
        initializer(g->get_init(), t, out.initializer);
        result.globals.push_back(std::move(out));
    }
    for (auto *ir : module.get_functions()) {
        if (ir->is_declaration())
            continue;
        MachineFunction f;
        f.name = ir->get_name();
        if (!ir->get_return_type()->is_void_type())
            f.return_type = value_kind(ir->get_return_type());
        std::map<const Value *, VReg> values;
        std::map<BasicBlock *, BlockId> blocks;
        for (auto *a : ir->get_args()) {
            auto t = value_kind(a->get_type());
            f.parameter_types.push_back(t);
            values[a] = f.new_vreg(t);
        }
        for (auto *b : ir->get_basic_blocks()) {
            BlockId id = f.blocks.size();
            blocks[b] = id;
            f.blocks.push_back({id, b->get_name(), {}, {}, {}});
            for (auto *i : b->get_instructions())
                if (!i->is_void())
                    values[i] = f.new_vreg(value_kind(i->get_type()));
        }
        auto emit = [](std::vector<MachineInstr> &out, Opcode op, std::vector<O> args,
                       const Value *source = nullptr) {
            out.push_back({op, std::move(args), source});
        };
        auto constant = [&](std::vector<MachineInstr> &out, int64_t n,
                            ValueKind t = ValueKind::I32) {
            auto v = f.new_vreg(t);
            emit(out, Opcode::LOAD_CONSTANT, {O::vreg(v, RegRole::Def), O::immediate(n)});
            return O::vreg(v);
        };
        std::function<O(Value *, std::vector<MachineInstr> &)> use =
            [&](Value *v, std::vector<MachineInstr> &out) -> O {
            auto found = values.find(v);
            if (found != values.end())
                return O::vreg(found->second);
            if (auto *c = dynamic_cast<ConstantInt *>(v)) {
                if (v->get_type()->is_int1_type() && c->get_value() != 0 && c->get_value() != 1)
                    throw std::runtime_error("MIR non-canonical i1 constant");
                return constant(out, c->get_value(), value_kind(v->get_type()));
            }
            if (auto *c = dynamic_cast<ConstantFP *>(v)) {
                auto bits = constant(out, static_cast<int32_t>(fp_bits(c)));
                auto r = f.new_vreg(ValueKind::F32);
                emit(out, Opcode::MOVGR2FR_W, {O::vreg(r, RegRole::Def), bits});
                return O::vreg(r);
            }
            if (dynamic_cast<ConstantZero *>(v)) {
                auto t = value_kind(v->get_type());
                auto zero = constant(out, 0, t == ValueKind::F32 ? ValueKind::I32 : t);
                if (t != ValueKind::F32)
                    return zero;
                auto r = f.new_vreg(t);
                emit(out, Opcode::MOVGR2FR_W, {O::vreg(r, RegRole::Def), zero});
                return O::vreg(r);
            }
            if (auto *g = dynamic_cast<GlobalVariable *>(v)) {
                auto r = f.new_vreg(ValueKind::Ptr64);
                emit(out, Opcode::SYMBOL_ADDR, {O::vreg(r, RegRole::Def), O::sym(g->get_name())});
                return O::vreg(r);
            }
            throw std::runtime_error("MIR " + f.name + ": unsupported operand " + v->get_name());
        };
        auto metric = [&](std::vector<MachineInstr> &out, int category, int weight,
                          const Value *src = nullptr) {
            auto c = constant(out, category), w = constant(out, weight);
            emit(out, Opcode::HIGH_CALL, {O::sym("add_lab4_flag"), c, w}, src);
        };
        unsigned arg = 0;
        for (auto *a : ir->get_args())
            emit(f.blocks.front().instructions, Opcode::ARG,
                 {O::vreg(values.at(a), RegRole::Def), O::immediate(arg++)}, a);
        if (options.lab4_metrics && f.name == "main")
            metric(f.blocks.front().instructions, 0, int(alloca_bytes));
        for (auto *b : ir->get_basic_blocks()) {
            auto &out = f.block(blocks.at(b)).instructions;
            auto plans = plan_block(b, options);
            for (auto *i : b->get_instructions()) {
                if (plans.covered.count(i))
                    continue;
                auto found_plan = plans.consumers.find(i);
                SelectionPlan plan =
                    found_plan == plans.consumers.end() ? SelectionPlan{} : found_plan->second;
                Opcode selected;
                auto op = i->get_instr_type();
                O dst;
                if (!i->is_void())
                    dst = O::vreg(values.at(i), RegRole::Def);
                auto at = [&](unsigned k) { return use(i->get_operand(k), out); };
                if (plan.immediate_base) {
                    auto base = use(plan.immediate_base, out);
                    emit(out, Opcode::ADDI_W, {dst, base, O::immediate(plan.immediate)}, i);
                } else if (i->isBinary()) {
                    switch (op) {
                    case Instruction::add:
                        selected = Opcode::ADD_W;
                        break;
                    case Instruction::sub:
                        selected = Opcode::SUB_W;
                        break;
                    case Instruction::mul:
                        selected = Opcode::MUL_W;
                        break;
                    case Instruction::sdiv:
                        selected = Opcode::DIV_W;
                        break;
                    case Instruction::fadd:
                        selected = Opcode::FADD_S;
                        break;
                    case Instruction::fsub:
                        selected = Opcode::FSUB_S;
                        break;
                    case Instruction::fmul:
                        selected = Opcode::FMUL_S;
                        break;
                    case Instruction::fdiv:
                        selected = Opcode::FDIV_S;
                        break;
                    default:
                        throw std::runtime_error("MIR unknown binary");
                    }
                    auto a = at(0), c = at(1);
                    emit(out, selected, {dst, a, c}, i);
                } else if (i->is_cmp()) {
                    auto a = at(0), c = at(1);
                    if (op == Instruction::gt || op == Instruction::le)
                        std::swap(a, c);
                    if (op == Instruction::eq || op == Instruction::ne) {
                        auto t = f.new_vreg(ValueKind::I32);
                        emit(out, Opcode::XOR, {O::vreg(t, RegRole::Def), a, c}, i);
                        if (op == Instruction::eq)
                            emit(out, Opcode::SLTUI, {dst, O::vreg(t), O::immediate(1)}, i);
                        else
                            emit(out, Opcode::SLTU, {dst, O::preg(gpr(0)), O::vreg(t)}, i);
                    } else if (op == Instruction::ge || op == Instruction::le) {
                        auto t = f.new_vreg(ValueKind::I1);
                        emit(out, Opcode::SLT, {O::vreg(t, RegRole::Def), a, c}, i);
                        emit(out, Opcode::XORI, {dst, O::vreg(t), O::immediate(1)}, i);
                    } else
                        emit(out, Opcode::SLT, {dst, a, c}, i);
                } else if (i->is_fcmp()) {
                    auto a = at(0), c = at(1);
                    if (op == Instruction::fge || op == Instruction::fgt)
                        std::swap(a, c);
                    selected =
                        (op == Instruction::fge || op == Instruction::fle)   ? Opcode::FCMP_SULE_S
                        : (op == Instruction::fgt || op == Instruction::flt) ? Opcode::FCMP_SULT_S
                        : op == Instruction::feq                             ? Opcode::FCMP_SUEQ_S
                                                                             : Opcode::FCMP_SUNE_S;
                    emit(out, selected, {O::preg(fcc(), RegRole::Def), a, c}, i);
                    emit(out, Opcode::MOVCF2GR, {dst, O::preg(fcc())}, i);
                } else if (i->is_zext()) {
                    if (!i->get_operand(0)->get_type()->is_int1_type() ||
                        !i->get_type()->is_int32_type())
                        throw std::runtime_error("MIR only supports zext i1 to i32");
                    auto a = at(0);
                    emit(out, Opcode::COPY, {dst, a}, i);
                } else if (i->is_si2fp()) {
                    if (!i->get_operand(0)->get_type()->is_int32_type() ||
                        !i->get_type()->is_float_type())
                        throw std::runtime_error("MIR only supports sitofp i32 to float");
                    auto a = at(0);
                    auto t = f.new_vreg(ValueKind::F32);
                    emit(out, Opcode::MOVGR2FR_W, {O::vreg(t, RegRole::Def), a}, i);
                    emit(out, Opcode::FFINT_S_W, {dst, O::vreg(t)}, i);
                } else if (i->is_fp2si()) {
                    if (!i->get_operand(0)->get_type()->is_float_type() ||
                        !i->get_type()->is_int32_type())
                        throw std::runtime_error("MIR only supports fptosi float to i32");
                    auto a = at(0);
                    auto t = f.new_vreg(ValueKind::F32);
                    emit(out, Opcode::FTINTRZ_W_S, {O::vreg(t, RegRole::Def), a}, i);
                    emit(out, Opcode::MOVFR2GR_S, {dst, O::vreg(t)}, i);
                } else if (auto *a = dynamic_cast<AllocaInst *>(i)) {
                    if (b != ir->get_entry_block())
                        throw std::runtime_error("MIR " + f.name +
                                                 ": alloca is only supported in the entry block "
                                                 "(block " +
                                                 b->get_name() + ")");
                    auto *t = a->get_alloca_type();
                    unsigned fi = f.frame_objects.size();
                    f.frame_objects.push_back(
                        {ValueKind::Ptr64, type_size(t), type_align(t), FramePurpose::Alloca, 0});
                    emit(out, Opcode::FRAME_ADDR, {dst, O::frame(fi)}, i);
                } else if (i->is_load() || i->is_store()) {
                    bool load = i->is_load();
                    Value *v = load ? i : i->get_operand(0);
                    auto kind = value_kind(v->get_type());
                    auto value = load ? dst : at(0);
                    auto address =
                        plan.address_base ? use(plan.address_base, out) : at(load ? 0 : 1);
                    int64_t memory_offset = plan.address_base ? plan.address_offset : 0;
                    selected = kind == ValueKind::F32     ? (load ? Opcode::FLD_S : Opcode::FST_S)
                               : kind == ValueKind::Ptr64 ? (load ? Opcode::LD_D : Opcode::ST_D)
                               : kind == ValueKind::I1    ? (load ? Opcode::LD_B_U : Opcode::ST_B)
                                                          : (load ? Opcode::LD_W : Opcode::ST_W);
                    if (load && kind == ValueKind::I1) {
                        auto raw = f.new_vreg(ValueKind::I32);
                        emit(out, Opcode::LD_B_U,
                             {O::vreg(raw, RegRole::Def), address, O::immediate(memory_offset)}, i);
                        emit(out, Opcode::ANDI, {dst, O::vreg(raw), O::immediate(1)}, i);
                    } else
                        emit(out, selected, {value, address, O::immediate(memory_offset)}, i);
                } else if (i->is_gep()) {
                    auto base = at(0);
                    Type *t = i->get_operand(0)->get_type()->get_pointer_element_type();
                    constant_gep_offset(i);

                    for (unsigned k = 1; k < i->get_num_operand(); ++k) {
                        if (k > 1) {
                            if (!t->is_array_type())
                                throw std::runtime_error("MIR GEP indexes non-array layer");
                            t = t->get_array_element_type();
                        }
                        auto index = at(k);
                        if (!i->get_operand(k)->get_type()->is_int32_type())
                            throw std::runtime_error("MIR GEP requires i32 indices");
                        auto stride =
                            constant(out, static_cast<int64_t>(type_size(t)), ValueKind::Ptr64);
                        auto off = f.new_vreg(ValueKind::Ptr64);
                        emit(out, Opcode::MUL_D, {O::vreg(off, RegRole::Def), index, stride}, i);
                        auto next = k + 1 == i->get_num_operand()
                                        ? dst
                                        : O::vreg(f.new_vreg(ValueKind::Ptr64), RegRole::Def);
                        emit(out, Opcode::ADD_D, {next, base, O::vreg(off)}, i);
                        base = O::vreg(next.id);
                    }
                    if (i->get_num_operand() == 1)
                        emit(out, Opcode::COPY, {dst, base}, i);
                } else if (i->is_call()) {
                    auto *callee = dynamic_cast<Function *>(i->get_operand(0));
                    if (!callee)
                        throw std::runtime_error("MIR indirect calls unsupported");
                    auto *ft = callee->get_function_type();
                    if (i->get_num_operand() != ft->get_num_of_args() + 1)
                        throw std::runtime_error("MIR wrong call argument count");
                    std::vector<O> args;
                    if (!i->is_void())
                        args.push_back(dst);
                    args.push_back(O::sym(callee->get_name()));
                    for (unsigned k = 1; k < i->get_num_operand(); ++k) {
                        if (i->get_operand(k)->get_type() != ft->get_param_type(k - 1))
                            throw std::runtime_error("MIR call argument type mismatch");
                        args.push_back(at(k));
                    }
                    emit(out, Opcode::HIGH_CALL, std::move(args), i);
                } else if (i->is_phi())
                    emit(out, Opcode::PHI, {dst}, i);
                else if (i->is_br()) {
                    if (i->get_num_operand() == 3) {
                        auto target =
                            O::block(blocks.at(static_cast<BasicBlock *>(i->get_operand(1))));
                        if (plan.comparison) {
                            auto *comparison = plan.comparison;
                            auto predicate = comparison->get_instr_type();
                            auto lhs = use(comparison->get_operand(0), out),
                                 rhs = use(comparison->get_operand(1), out);
                            if (predicate == Instruction::gt || predicate == Instruction::le)
                                std::swap(lhs, rhs);
                            Opcode branch =
                                (predicate == Instruction::lt || predicate == Instruction::gt)
                                    ? Opcode::BLT
                                : (predicate == Instruction::le || predicate == Instruction::ge)
                                    ? Opcode::BGE
                                : predicate == Instruction::eq ? Opcode::BEQ
                                                               : Opcode::BNE;
                            emit(out, branch, {lhs, rhs, target}, i);
                        } else {
                            auto cond =
                                plan.branch_condition ? use(plan.branch_condition, out) : at(0);
                            emit(out, Opcode::BNEZ, {cond, target}, i);
                        }
                        emit(out, Opcode::B,
                             {O::block(blocks.at(static_cast<BasicBlock *>(i->get_operand(2))))},
                             i);
                    } else
                        emit(out, Opcode::B,
                             {O::block(blocks.at(static_cast<BasicBlock *>(i->get_operand(0))))},
                             i);
                } else if (i->is_ret()) {
                    std::vector<O> args;
                    if (i->get_num_operand())
                        args.push_back(at(0));
                    emit(out, Opcode::RETURN, std::move(args), i);
                } else
                    throw std::runtime_error("MIR " + f.name + ": unsupported LightIR opcode " +
                                             std::to_string(op));
                if (options.lab4_metrics) {
                    int weight = 0;
                    if (i->is_mul() || i->is_fmul() || i->is_gep())
                        weight = 1;
                    if (i->is_div() || i->is_fdiv())
                        weight = 4;
                    if (i->is_load()) {
                        auto *a = dynamic_cast<AllocaInst *>(i->get_operand(0));
                        if (!a || a->get_alloca_type()->is_array_type())
                            weight = 3;
                    }
                    if (weight)
                        metric(out, 1, weight, i);
                }
            }
        }
        // PHI constants are defined on their incoming edge, never in the join block.
        for (auto *b : ir->get_basic_blocks())
            for (auto *i : b->get_instructions())
                if (auto *p = dynamic_cast<PhiInst *>(i)) {
                    std::vector<O> operands = {O::vreg(values.at(i), RegRole::Def)};
                    for (auto pair : p->get_phi_pairs()) {
                        std::vector<MachineInstr> constants;
                        auto v = use(pair.first, constants);
                        auto &pred = f.block(blocks.at(pair.second)).instructions;
                        auto at = std::find_if(pred.begin(), pred.end(), [](const MachineInstr &x) {
                            return descriptor(x.opcode).terminator;
                        });
                        pred.insert(at, constants.begin(), constants.end());
                        operands.push_back(v);
                        operands.push_back(O::block(blocks.at(pair.second)));
                    }
                    for (auto &mi : f.block(blocks.at(b)).instructions)
                        if (mi.source == i && mi.opcode == Opcode::PHI) {
                            mi.operands = std::move(operands);
                            break;
                        }
                }
        rebuild_cfg(f);
        result.functions.push_back(std::move(f));
    }
    return result;
}
} // namespace machine
