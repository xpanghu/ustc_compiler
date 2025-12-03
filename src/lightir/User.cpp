#include "User.hpp"

#include <cassert>

User::~User() { remove_all_operands(); }

void User::set_operand(unsigned i, Value *v) {
    assert(i < operands_.size() && "set_operand out of index");
    if (operands_[i]) { // old operand
        operands_[i]->remove_use(this, i);
    }
    if (v) { // new operand
        v->add_use(this, i);
    }
    operands_[i] = v;
}

void User::add_operand(Value *v) {
    if (v == nullptr) return;
    v->add_use(this, static_cast<unsigned>(operands_.size()));
    operands_.push_back(v);
}

void User::remove_all_operands() {
    for (unsigned i = 0; i != operands_.size(); ++i) {
        if (operands_[i]) {
            operands_[i]->remove_use(this, i);
        }
    }
    operands_.clear();
}

void User::remove_operand(unsigned idx) {
    assert(idx < operands_.size() && "remove_operand out of index");
    // influence on other operands
    for (unsigned i = idx + 1; i < operands_.size(); ++i) {
        if (operands_[i])
        {
            operands_[i]->remove_use(this, i);
            operands_[i]->add_use(this, i - 1);
        }
    }
    // remove the designated operand
    if (operands_[idx])
        operands_[idx]->remove_use(this, idx);
    operands_.erase(operands_.begin() + idx);
}
