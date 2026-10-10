#include "GVN.hpp"
#include "ConstPropagation.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace GVNExpression;

Constant *GVNExpression::ConstFolder::compute(Instruction *inst, Constant *a, Constant *b) {
    ::ConstFolder folder(module_);
    if (a->get_type() != b->get_type()) return nullptr;
    if (auto lhs = dynamic_cast<ConstantInt *>(a)) {
        if (auto rhs = dynamic_cast<ConstantInt *>(b))
            if (lhs->get_type()->is_int32_type()) return folder.compute(inst->get_instr_type(), lhs, rhs);
    }
    if (auto lhs = dynamic_cast<ConstantFP *>(a))
        if (auto rhs = dynamic_cast<ConstantFP *>(b)) return folder.compute(inst->get_instr_type(), lhs, rhs);
    return nullptr;
}

Constant *GVNExpression::ConstFolder::compute(Instruction *inst, Constant *a) {
    ::ConstFolder folder(module_);
    if (auto value = dynamic_cast<ConstantInt *>(a)) {
        if (inst->is_zext() && value->get_type()->is_int1_type() && inst->get_type()->is_int32_type())
            return ConstantInt::get(value->get_value(), module_);
        if (inst->is_si2fp() && value->get_type()->is_int32_type() && inst->get_type()->is_float_type())
            return folder.compute(inst->get_instr_type(), value);
    }
    if (auto value = dynamic_cast<ConstantFP *>(a))
        if (inst->is_fp2si() && inst->get_type()->is_int32_type()) return folder.compute(inst->get_instr_type(), value);
    return nullptr;
}

Expression::Expression(Kind k, std::string t, Instruction::OpID op, size_t id,
                       std::vector<std::string> types, std::vector<size_t> args, std::vector<size_t> incoming)
    : kind(k), type(std::move(t)), opcode(op), identity(id), operand_types(std::move(types)),
      operands(std::move(args)), edges(std::move(incoming)) {}

std::string Expression::print() const {
    std::ostringstream out;
    out << kind << ':' << type << ':' << unsigned(opcode) << ':' << identity;
    for (auto id : operands) out << ':' << id;
    for (auto edge : edges) out << '@' << edge;
    return out.str();
}

bool GVNExpression::operator==(const Expr &a, const Expr &b) {
    return (!a && !b) || (a && b && *a == *b);
}

static size_t constant_bits(Constant *c) {
    if (auto i = dynamic_cast<ConstantInt *>(c)) return uint32_t(i->get_value());
    if (auto f = dynamic_cast<ConstantFP *>(c)) {
        uint32_t bits;
        float value = f->get_value();
        std::memcpy(&bits, &value, sizeof bits);
        return bits;
    }
    return 0;
}

size_t GVN::valueId(Value *v) {
    if (auto it = value_ids_.find(v); it != value_ids_.end()) return it->second;
    size_t id;
    if (auto c = dynamic_cast<Constant *>(v)) {
        // Non-scalar constants are identities; only scalar constants have exact bit keys.
        if (dynamic_cast<ConstantInt *>(c) || dynamic_cast<ConstantFP *>(c)) {
            auto key = c->get_type()->print() + ':' + std::to_string(constant_bits(c));
            auto [it, inserted] = constant_ids_.emplace(key, next_id_);
            if (inserted) ++next_id_;
            id = it->second;
        } else id = next_id_++;
    } else id = next_id_++;
    value_ids_[v] = id;
    return id;
}

GVN::Class GVN::findClass(const partitions &p, Value *v) const {
    if (isTop(p)) return nullptr;
    for (auto cc : p) if (cc->members_.count(v)) return cc;
    return nullptr;
}

size_t GVN::classId(const partitions &p, Value *v) {
    if (auto cc = findClass(p, v)) return cc->index_;
    return valueId(v);
}

static bool commutative(Instruction::OpID op) {
    return op == Instruction::add || op == Instruction::mul || op == Instruction::eq || op == Instruction::ne;
}

static Expr rekey(const Expr &e, std::vector<size_t> args) {
    if (e->kind == Expression::operation && commutative(e->opcode) && args.size() == 2 && args[1] < args[0])
        std::swap(args[0], args[1]);
    return std::make_shared<Expression>(e->kind, e->type, e->opcode, e->identity, e->operand_types, std::move(args), e->edges);
}

void GVN::normalize(partitions &p) {
    if (isTop(p)) return;
    std::map<size_t, size_t> renamed;
    for (auto cc : p) {
        if (cc->members_.empty()) continue;
        size_t label = next_id_;
        for (auto member : cc->members_) label = std::min(label, valueId(member));
        renamed[cc->index_] = label;
    }
    // Detach before changing comparator keys; block-local classes are already private copies.
    std::vector<Class> classes(p.begin(), p.end());
    p.clear();
    for (auto cc : classes) {
        if (cc->members_.empty()) continue;
        cc->index_ = renamed.at(cc->index_);
        std::set<Expr, ExprLess> expressions;
        for (auto e : cc->expressions_) {
            auto args = e->operands;
            bool valid = true;
            for (auto &arg : args) {
                auto it = renamed.find(arg);
                if (it == renamed.end()) { valid = false; break; }
                arg = it->second;
            }
            if (valid) expressions.insert(args == e->operands ? e : rekey(e, std::move(args)));
        }
        cc->expressions_ = std::move(expressions);
        cc->leader_ = nullptr;
        for (auto member : cc->members_)
            if (!cc->leader_ || valueId(member) < valueId(cc->leader_) ||
                (dynamic_cast<Constant *>(member) && !dynamic_cast<Constant *>(cc->leader_)))
                cc->leader_ = member;
        cc->value_expr_ = cc->expressions_.empty() ? nullptr : *cc->expressions_.begin();
        p.insert(cc);
    }
}

void GVN::removeValue(partitions &p, Value *v) {
    if (auto cc = findClass(p, v)) cc->members_.erase(v);
    normalize(p); // drop facts with no remaining name for a killed operand
}

void GVN::addValue(partitions &p, Value *v, Value *equal, Expr e) {
    auto cc = equal ? findClass(p, equal) : nullptr;
    if (!cc && e && e->kind != Expression::opaque) cc = getVN(p, e);
    if (!cc) {
        cc = createCongruenceClass(valueId(equal ? equal : v));
        if (equal) cc->members_.insert(equal);
        p.insert(cc);
    }
    cc->members_.insert(v);
    if (e && e->kind != Expression::opaque) cc->expressions_.insert(e);
    normalize(p);
}

Expr GVN::valueExpr(const partitions &p, Value *v) {
    auto type = v->get_type()->print();
    if (auto c = dynamic_cast<Constant *>(v)) {
        if (dynamic_cast<ConstantInt *>(c) || dynamic_cast<ConstantFP *>(c))
            return std::make_shared<Expression>(Expression::constant, type, Instruction::ret, constant_bits(c));
    }
    auto inst = dynamic_cast<Instruction *>(v);
    bool supported = inst && (inst->isBinary() || inst->is_cmp() || inst->is_fcmp() || inst->is_zext() ||
                              inst->is_si2fp() || inst->is_fp2si() || inst->is_gep());
    if (inst && inst->is_call()) supported = pure_[static_cast<Function *>(inst->get_operand(0))];
    if (!supported) return std::make_shared<Expression>(Expression::opaque, type, Instruction::ret, valueId(v));
    std::vector<size_t> args;
    std::vector<std::string> types;
    unsigned start = inst->is_call() ? 1 : 0;
    for (unsigned i = start; i < inst->get_num_operand(); ++i) {
        args.push_back(classId(p, inst->get_operand(i)));
        types.push_back(inst->get_operand(i)->get_type()->print());
    }
    size_t identity = inst->is_call() ? valueId(inst->get_operand(0)) : 0;
    auto key = std::make_shared<Expression>(Expression::operation, type, inst->get_instr_type(), identity, types, args);
    return rekey(key, std::move(args));
}

GVN::Class GVN::getVN(const partitions &p, Expr e) {
    if (!e || isTop(p)) return nullptr;
    for (auto cc : p)
        if (cc->expressions_.count(e)) return cc;
    return nullptr;
}

void GVN::transferFunction(Instruction *inst, partitions &p) {
    // SSA arithmetic/address facts survive memory effects; loads are deliberately opaque.
    if (inst->is_void() || inst->is_phi() || isTop(p)) return;
    removeValue(p, inst);
    for (auto operand : inst->get_operands())
        if (!dynamic_cast<Function *>(operand) && !findClass(p, operand)) addValue(p, operand);
    std::vector<Constant *> constants;
    for (auto operand : inst->get_operands()) {
        Constant *constant = nullptr;
        if (auto cc = findClass(p, operand))
            for (auto member : cc->members_)
                if (auto c = dynamic_cast<Constant *>(member)) { constant = c; break; }
        constants.push_back(constant);
    }
    Constant *folded = nullptr;
    if ((inst->isBinary() || inst->is_cmp() || inst->is_fcmp()) && constants[0] && constants[1])
        folded = folder_->compute(inst, constants[0], constants[1]);
    else if ((inst->is_zext() || inst->is_si2fp() || inst->is_fp2si()) && constants[0])
        folded = folder_->compute(inst, constants[0]);
    if (folded && folded->get_type() == inst->get_type()) {
        if (!findClass(p, folded)) addValue(p, folded, nullptr, valueExpr(p, folded));
        addValue(p, inst, folded, valueExpr(p, inst));
    } else addValue(p, inst, nullptr, valueExpr(p, inst));
}

GVN::Class GVN::intersect(const Class &a, const Class &b) {
    auto cc = createCongruenceClass(next_id_);
    std::set_intersection(a->members_.begin(), a->members_.end(), b->members_.begin(), b->members_.end(),
                          std::inserter(cc->members_, cc->members_.end()));
    if (cc->members_.empty()) return nullptr;
    for (auto v : cc->members_) cc->index_ = std::min(cc->index_, valueId(v));
    return cc;
}

GVN::partitions GVN::join(const partitions &a, const partitions &b, BasicBlock *, BasicBlock *) {
    if (isTop(a)) return clone(b);
    if (isTop(b)) return clone(a);
    partitions result;
    std::map<std::pair<size_t, size_t>, Class> intersections;
    for (auto left : a)
        for (auto right : b)
            if (auto cc = intersect(left, right)) {
                result.insert(cc);
                intersections[{left->index_, right->index_}] = cc;
            }
    for (auto left : a)
        for (auto right : b) {
            auto found = intersections.find({left->index_, right->index_});
            if (found == intersections.end()) continue;
            for (auto x : left->expressions_)
                for (auto y : right->expressions_) {
                    if (x->kind != y->kind || x->type != y->type || x->opcode != y->opcode ||
                        x->identity != y->identity || x->operand_types != y->operand_types ||
                        x->operands.size() != y->operands.size()) continue;
                    // Integer commutative operands may have a different canonical order on each edge.
                    for (unsigned swap = 0; swap < (commutative(x->opcode) && x->operands.size() == 2 ? 2u : 1u); ++swap) {
                        std::vector<size_t> args;
                        for (size_t i = 0; i < x->operands.size(); ++i) {
                            size_t j = swap ? 1 - i : i;
                            auto ref = intersections.find({x->operands[i], y->operands[j]});
                            if (ref == intersections.end()) break;
                            args.push_back(ref->second->index_);
                        }
                        if (args.size() == x->operands.size()) found->second->expressions_.insert(rekey(x, std::move(args)));
                    }
                }
        }
    normalize(result);
    return result;
}

GVN::partitions GVN::projectEdge(BasicBlock *pred, BasicBlock *dest) {
    const auto &before = pout_.at(pred);
    if (isTop(before)) return clone(before);
    auto projected = clone(before);
    std::vector<std::pair<PhiInst *, Class>> assignments;
    for (auto inst : dest->get_instructions()) {
        auto phi = dynamic_cast<PhiInst *>(inst);
        if (!phi) continue;
        Value *incoming = nullptr;
        for (auto [v, bb] : phi->get_phi_pairs()) if (bb == pred) incoming = v;
        if (!incoming) throw std::runtime_error("GVN: missing PHI incoming edge");
        auto source = findClass(before, incoming);
        if (!source) {
            source = createCongruenceClass(valueId(incoming));
            source->members_.insert(incoming);
            projected.insert(std::make_shared<CongruenceClass>(*source));
        }
        assignments.emplace_back(phi, source);
    }
    // Read every source from the old edge first, then assign all destinations simultaneously.
    for (auto [phi, source] : assignments)
        for (auto cc : projected) cc->members_.erase(phi);
    for (auto [phi, source] : assignments)
        for (auto cc : projected)
            if (cc->index_ == source->index_) cc->members_.insert(phi);
    for (auto cc : projected) cc->value_phi_.reset();
    normalize(projected);
    return projected;
}

GVN::partitions GVN::joinIncoming(BasicBlock *bb) {
    partitions result{createCongruenceClass(0)};
    for (auto pred : bb->get_pre_basic_blocks())
        if (reachable_.count(pred)) result = join(result, projectEdge(pred, bb));
    // Projected operation keys retain exactly the facts common to all incoming edges.
    // Thus phi(add(al,bl),add(ar,br)) already carries add(VN(a),VN(b)); no recursive trees or PRE.
    if (!isTop(result)) {
        for (auto inst : bb->get_instructions()) {
            auto phi = dynamic_cast<PhiInst *>(inst);
            if (!phi) continue;
            std::map<size_t, size_t> incoming;
            bool complete = true;
            for (auto [v, pred] : phi->get_phi_pairs()) {
                if (!reachable_.count(pred)) continue;
                if (isTop(pout_.at(pred))) { complete = false; break; }
                incoming[block_ids_.at(pred)] = classId(pout_.at(pred), v);
            }
            if (!complete) continue;
            std::vector<size_t> edges, values;
            for (auto [edge, value] : incoming) { edges.push_back(edge); values.push_back(value); }
            if (auto cc = findClass(result, phi))
                cc->value_phi_ = std::make_shared<Expression>(Expression::phi, phi->get_type()->print(),
                                                            Instruction::phi, block_ids_.at(bb),
                                                            std::vector<std::string>{}, values, edges);
        }
    }
    return result;
}

void GVN::clearFunction() {
    pin_.clear(); pout_.clear(); value_ids_.clear(); constant_ids_.clear(); block_ids_.clear();
    positions_.clear(); rpo_.clear(); reachable_.clear(); dominators_.reset(); func_ = nullptr;
}

void GVN::initPerFunction() {
    next_id_ = 1;
    pin_.clear(); pout_.clear(); value_ids_.clear(); constant_ids_.clear(); block_ids_.clear();
    positions_.clear(); rpo_.clear(); reachable_.clear();
    std::function<void(BasicBlock *)> visit = [&](BasicBlock *bb) {
        if (!reachable_.insert(bb).second) return;
        for (auto succ : bb->get_succ_basic_blocks()) visit(succ);
        rpo_.push_back(bb);
    };
    visit(func_->get_entry_block());
    std::reverse(rpo_.begin(), rpo_.end());
    for (auto arg : func_->get_args()) valueId(arg);
    for (auto bb : rpo_) {
        block_ids_[bb] = block_ids_.size() + 1;
        size_t position = 0;
        for (auto inst : bb->get_instructions()) { valueId(inst); positions_[inst] = position++; }
    }
    for (auto bb : rpo_)
        for (auto inst : bb->get_instructions())
            for (auto operand : inst->get_operands()) valueId(operand);
    dominators_ = std::make_unique<Dominators>(func_);
    dominators_->run();
}

void GVN::detectEquivalences() {
    partitions boundary;
    for (auto [v, id] : value_ids_)
        if (!dynamic_cast<Instruction *>(v) && !dynamic_cast<BasicBlock *>(v) && !dynamic_cast<Function *>(v))
            addValue(boundary, v, nullptr, valueExpr({}, v));
    for (auto bb : rpo_) pout_[bb] = {createCongruenceClass(0)};
    std::deque<BasicBlock *> worklist(rpo_.begin(), rpo_.end());
    std::set<BasicBlock *> queued(reachable_);
    while (!worklist.empty()) {
        auto bb = worklist.front(); worklist.pop_front(); queued.erase(bb);
        auto in = bb == func_->get_entry_block() ? clone(boundary) : joinIncoming(bb);
        pin_[bb] = clone(in);
        auto out = std::move(in);
        for (auto inst : bb->get_instructions()) transferFunction(inst, out);
        if (!(out == pout_[bb])) {
            pout_[bb] = std::move(out);
            for (auto succ : bb->get_succ_basic_blocks())
                if (reachable_.count(succ) && queued.insert(succ).second) worklist.push_back(succ);
        }
    }
}

bool GVN::available(Value *value, Instruction *user, unsigned operand) const {
    if (!value || value == user) return false;
    auto where = user->get_parent();
    if (user->is_phi()) {
        if (operand % 2) return false;
        where = static_cast<BasicBlock *>(user->get_operand(operand + 1));
    }
    if (auto arg = dynamic_cast<Argument *>(value)) return arg->get_parent() == user->get_function();
    if (dynamic_cast<Constant *>(value) || dynamic_cast<GlobalVariable *>(value)) return true;
    auto def = dynamic_cast<Instruction *>(value);
    if (!def || def->get_function() != user->get_function()) return false;
    if (def->get_parent() == where) {
        auto end = user->is_phi() ? where->get_terminator() : user;
        if (positions_.count(def) && positions_.count(end)) return positions_.at(def) < positions_.at(end);
        // Also supports the public synthetic-partition test entry before IDs have been built.
        for (auto inst : where->get_instructions()) {
            if (inst == end) return false;
            if (inst == def) return true;
        }
        return false;
    }
    if (!dominators_ || !reachable_.count(where) || !reachable_.count(def->get_parent())) return false;
    return dominators_->is_dominate(def->get_parent(), where);
}

Value *GVN::representative(const Class &cc, Value *old, Instruction *user, unsigned operand) {
    Value *best = available(old, user, operand) ? old : nullptr;
    for (auto value : cc->members_) {
        if (value->get_type() != old->get_type() || !available(value, user, operand)) continue;
        if (!best || (dynamic_cast<Constant *>(value) && !dynamic_cast<Constant *>(best)) ||
            (bool(dynamic_cast<Constant *>(value)) == bool(dynamic_cast<Constant *>(best)) && valueId(value) < valueId(best)))
            best = value;
    }
    return best ? best : old;
}

void GVN::replace_cc_members(const std::map<BasicBlock *, partitions> &pout) {
    struct Edit { Instruction *user; unsigned operand; Value *value; };
    std::vector<Edit> edits;
    for (auto &[bb, p] : pout) {
        if (isTop(p)) continue;
        for (auto cc : p)
            for (auto member : cc->members_)
                for (auto use : member->get_use_list()) {
                    auto user = dynamic_cast<Instruction *>(use.val_);
                    if (!user) continue;
                    bool local = user->is_phi() ? use.arg_no_ % 2 == 0 && user->get_operand(use.arg_no_ + 1) == bb : user->get_parent() == bb;
                    if (!local) continue;
                    // Treat an explicitly supplied leader as the first stable hint.
                    if (cc->leader_) valueId(cc->leader_);
                    auto value = representative(cc, member, user, use.arg_no_);
                    if (value != member) edits.push_back({user, use.arg_no_, value});
                }
    }
    for (auto edit : edits) edit.user->set_operand(edit.operand, edit.value);
}

void GVN::replaceUses() {
    struct Edit { Instruction *user; unsigned operand; Value *value; };
    std::vector<Edit> edits;
    for (auto bb : rpo_) {
        auto state = clone(pin_.at(bb));
        for (auto inst : bb->get_instructions()) {
            for (unsigned i = 0; i < inst->get_num_operand(); ++i) {
                if (inst->is_phi() && (i % 2 || !reachable_.count(static_cast<BasicBlock *>(inst->get_operand(i + 1))))) continue;
                auto old = inst->get_operand(i);
                const auto &facts = inst->is_phi() ? pout_.at(static_cast<BasicBlock *>(inst->get_operand(i + 1))) : state;
                if (auto cc = findClass(facts, old)) {
                    auto value = representative(cc, old, inst, i);
                    if (value != old) edits.push_back({inst, i, value});
                }
            }
            transferFunction(inst, state);
        }
    }
    // No definitions are deleted or moved. Every planned representative remains available.
    for (auto edit : edits)
        if (available(edit.value, edit.user, edit.operand)) edit.user->set_operand(edit.operand, edit.value);
}

static std::string quote(const std::string &s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"';
    return out.str();
}

std::string GVN::dumpFunction() const {
    std::ostringstream out;
    out << "{\"function\":" << quote(func_->get_name()) << ",\"pout\":{";
    bool first_bb = true;
    for (auto bb : rpo_) {
        if (!first_bb) out << ',';
        first_bb = false;
        out << quote(bb->get_name()) << ":[";
        bool first_cc = true;
        for (auto cc : pout_.at(bb)) {
            if (!first_cc) out << ',';
            first_cc = false;
            std::vector<Value *> members(cc->members_.begin(), cc->members_.end());
            std::sort(members.begin(), members.end(), [&](Value *a, Value *b) { return value_ids_.at(a) < value_ids_.at(b); });
            out << '[';
            bool first_member = true;
            for (auto member : members) {
                if (!first_member) out << ',';
                first_member = false;
                if (auto integer = dynamic_cast<ConstantInt *>(member)) out << integer->print();
                else if (auto fp = dynamic_cast<ConstantFP *>(member)) {
                    std::ostringstream bits;
                    bits << "float:0x" << std::hex << std::setw(8) << std::setfill('0') << constant_bits(fp);
                    out << quote(bits.str());
                } else if (auto c = dynamic_cast<Constant *>(member)) out << quote(c->get_type()->print() + ':' + c->print());
                else out << quote((dynamic_cast<GlobalVariable *>(member) || dynamic_cast<Function *>(member) ? "@" : "%") + member->get_name());
            }
            out << ']';
        }
        out << ']';
    }
    out << "}}";
    return out.str();
}

void GVN::run() {
    clearFunction(); pure_.clear();
    folder_ = std::make_unique<GVNExpression::ConstFolder>(m_);
    // Analyze the unchanged module and keep only purity booleans during rewriting.
    {
        FuncInfo info(m_);
        info.run();
        for (auto f : m_->get_functions()) pure_[f] = info.is_pure(f);
    }
    // FuncInfo is not alias analysis: unknown pointer origins must not imply purity.
    for (auto f : m_->get_functions())
        for (auto bb : f->get_basic_blocks())
            for (auto inst : bb->get_instructions()) {
                Value *ptr = inst->is_load() ? inst->get_operand(0) : inst->is_store() ? inst->get_operand(1) : nullptr;
                while (auto gep = dynamic_cast<GetElementPtrInst *>(ptr)) ptr = gep->get_operand(0);
                if (ptr && !dynamic_cast<AllocaInst *>(ptr)) pure_[f] = false;
            }
    bool changed;
    do {
        changed = false;
        for (auto f : m_->get_functions())
            if (pure_[f])
                for (auto bb : f->get_basic_blocks())
                    for (auto inst : bb->get_instructions())
                        if (inst->is_call() && !pure_[static_cast<Function *>(inst->get_operand(0))]) { pure_[f] = false; changed = true; }
    } while (changed);
    m_->set_print_name();
    std::ofstream json;
    if (dump_json_) {
        json.open("gvn.json", std::ios::trunc);
        if (!json) throw std::runtime_error("GVN: cannot open gvn.json for writing");
        json << '[';
    }
    bool first = true;
    for (auto f : m_->get_functions()) {
        if (f->is_declaration()) continue;
        func_ = f;
        initPerFunction();
        detectEquivalences();
        if (dump_json_) {
            if (!first) json << ',';
            first = false;
            json << dumpFunction();
        }
        replaceUses();
        clearFunction();
    }
    pure_.clear(); folder_.reset();
    if (dump_json_) {
        json << "]\n";
        json.close();
        if (!json) throw std::runtime_error("GVN: failed writing gvn.json");
    }
}

GVN::partitions GVN::clone(const partitions &p) {
    partitions result;
    for (auto cc : p) result.insert(std::make_shared<CongruenceClass>(*cc));
    return result;
}

bool CongruenceClass::operator==(const CongruenceClass &other) const {
    return members_ == other.members_ && value_expr_ == other.value_expr_ && value_phi_ == other.value_phi_ && expressions_ == other.expressions_;
}

bool operator==(const GVN::partitions &a, const GVN::partitions &b) {
    if (GVN::isTop(a) || GVN::isTop(b)) return GVN::isTop(a) && GVN::isTop(b);
    if (a.size() != b.size()) return false;
    for (auto cc : a) {
        bool found = false;
        for (auto other : b) if (*cc == *other) { found = true; break; }
        if (!found) return false;
    }
    return true;
}
