#include "Names.hpp"

std::string Names::get_name(std::string name)
{
    int ed = static_cast<int>(name.size());
    int bg = 0;
    while (bg < ed)
    {
        char ch = name[bg];
        if (ch == '_' || (ch >= '0' && ch <= '9')) bg++;
        else break;
    }
    while (ed > bg)
    {
        char ch = name[ed - 1];
        if (ch == '_' || (ch >= '0' && ch <= '9')) ed--;
        else break;
    }
    if (bg == ed) return get_name();
    std::string name1 = {name.begin() + bg, name.begin() + ed};
    if (name1 == default_prefix_) return get_name();
    auto get = appended_prefix_ + name1;
    auto idx = allocated_[name1]++;
    if (idx == 0) return get;
    return get + std::to_string(idx);
}

std::string Names::get_name()
{
    auto get = appended_prefix_ + default_prefix_;
    return get + std::to_string(++default_prefix_used_count_);
}
