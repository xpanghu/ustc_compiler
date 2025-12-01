#include "Value.hpp"
#include "User.hpp"
#include "util.hpp"


Value::~Value() { replace_all_use_with(nullptr); }

bool Value::set_name(const std::string& name) {
    if (name_.empty()) {
        name_ = name;
        return true;
    }
    return false;
}

void Value::add_use(User *user, unsigned arg_no) {
    if (user == nullptr) return;
    use_list_.emplace_back(user, arg_no);
};

void Value::remove_use(User *user, unsigned arg_no) {
    if (user == nullptr) return;
    auto target_use = Use(user, arg_no);
    use_list_.remove_if([&](const Use &use) { return use == target_use; });
}

void Value::replace_all_use_with(Value *new_val) const
{
    if (this == new_val)
        return;
    while (!use_list_.empty()) {
        auto use = use_list_.begin();
        auto val = use->val_;
        if (val != nullptr) val->set_operand(use->arg_no_, new_val);
    }
}

void Value::replace_use_with_if(Value *new_val,
                                const std::function<bool(Use *)>& should_replace) {
    if (this == new_val)
        return;
    for (auto iter = use_list_.begin(); iter != use_list_.end();) {
        auto &use = *iter++;
        if (use.val_ == nullptr) continue;
        if (not should_replace(&use))
            continue;
        use.val_->set_operand(use.arg_no_, new_val);
    }
}

std::string Value::safe_get_name_or_ptr() const
{
    if (name_.empty()) return ptr_to_str(this);
    return name_;
}
