#include "BasicBlock.hpp"

#include "Function.hpp"
#include "IRprinter.hpp"
#include "Module.hpp"
#include "util.hpp"

#include <cassert>

#include "Names.hpp"

BasicBlock::BasicBlock(const Module* m, const std::string& name = "",
    Function* parent = nullptr)
    : Value(m->get_label_type(),
        parent == nullptr ? GLOBAL_BASICBLOCK_NAMES_.get_name(name) : parent->names4blocks_.get_name(name))
    , parent_(parent) {
    assert(parent && "currently parent should not be nullptr");
    parent_->add_basic_block(this);
}

Module* BasicBlock::get_module() const { return get_parent()->get_parent(); }
void BasicBlock::erase_from_parent() { this->get_parent()->remove(this); }

bool BasicBlock::is_terminated() const {
    if (instr_list_.empty()) {
        return false;
    }
    switch (instr_list_.back()->get_instr_type()) {
    case Instruction::ret:
    case Instruction::br:
        return true;
    default:
        return false;
    }
}

Instruction* BasicBlock::get_terminator() const
{
    assert(is_terminated() &&
        "Trying to get terminator from an bb which is not terminated");
    return instr_list_.back();
}

void BasicBlock::add_instruction(Instruction* instr) {
    if (instr->is_alloca() || instr->is_phi())
    {
        auto it = instr_list_.begin();
        for (; it != instr_list_.end() && ((*it)->is_alloca() || (*it)->is_phi()); ++it) {}
        instr_list_.emplace(it, instr);
        return;
    }
    assert(not is_terminated() && "Inserting instruction to terminated bb");
    instr_list_.push_back(instr);
}

void BasicBlock::add_instr_begin(Instruction* instr)
{
    if (instr->is_alloca() || instr->is_phi())
        instr_list_.push_front(instr);
    else
    {
        auto it = instr_list_.begin();
        for (; it != instr_list_.end() && ((*it)->is_alloca() || (*it)->is_phi()); ++it) {}
        instr_list_.emplace(it, instr);
    }
}

void BasicBlock::add_instr_before_terminator(Instruction* instr) {
    if (instr->is_alloca() || instr->is_phi())
    {
        auto it = instr_list_.begin();
        for (; it != instr_list_.end() && ((*it)->is_alloca() || (*it)->is_phi()); ++it) {}
        instr_list_.emplace(it, instr);
        return;
    }
    if (!is_terminated()) instr_list_.emplace_back(instr);
    else instr_list_.insert(std::prev(instr_list_.end()), instr);
}

void BasicBlock::erase_instrs(const std::set<Instruction*>& instr)
{
    std::list<Instruction*> ok;
    for (auto i : instr_list_)
    {
        if (!instr.count(i)) ok.emplace_back(i);
    }
    instr_list_ = std::move(ok);
    for (auto i : instr) delete i;
}

void BasicBlock::remove_instrs(const std::set<Instruction*>& instr)
{
    std::list<Instruction*> ok;
    for (auto i : instr_list_)
    {
        if (!instr.count(i)) ok.emplace_back(i);
    }
    instr_list_ = std::move(ok);
}

std::string BasicBlock::print() {
    std::string bb_ir;
    bb_ir += this->get_name();
    bb_ir += ":";
    // print prebb
    if (!this->get_pre_basic_blocks().empty()) {
        bb_ir += "                                                ; preds = ";
    }
    for (auto bb : this->get_pre_basic_blocks()) {
        if (bb != *this->get_pre_basic_blocks().begin()) {
            bb_ir += ", ";
        }
        bb_ir += print_as_op(bb, false);
    }

    // print prebb
    if (!this->get_parent()) {
        bb_ir += "\n";
        bb_ir += "; Error: Block without parent!";
    }
    bb_ir += "\n";
    for (auto instr : this->get_instructions()) {
        bb_ir += "  ";
        bb_ir += instr->print();
        bb_ir += "\n";
    }

    return bb_ir;
}

std::string BasicBlock::safe_print() const
{
    auto name = get_name();
    if (name.empty()) name = "b" + ptr_to_str(this);
    return name + " " + std::to_string(instr_list_.size()) + "inst pre " + std::to_string(pre_bbs_.size()) + "b suc " + std::to_string(succ_bbs_.size()) + "b";
}

BasicBlock::~BasicBlock()
{
    for (auto inst : instr_list_) delete inst;
}

BasicBlock* BasicBlock::get_entry_block_of_same_function() const
{
    assert(parent_ != nullptr && "bb have no parent function");
    return parent_->get_entry_block();
}

Names GLOBAL_BASICBLOCK_NAMES_{ "label", "_" };