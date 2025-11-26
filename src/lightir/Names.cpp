#include "Names.hpp"

std::string Names::get_name(std::string name)
{
	int ed = static_cast<int>(name.size());
	while (ed > 0)
	{
		char ch = name[ed - 1];
		if ((use_underline_ && ch == '_') || (ch >= '0' && ch <= '9')) ed--;
		else break;
	}
	if (ed == 0) return get_name();
	std::string name1 = {name.begin(), name.begin() + ed};
	if (name1 == default_prefix_) return get_name();
	auto get = appended_prefix_ + name1;
	auto idx = allocated_[name1]++;
	if (idx == 0) return get;
	if (use_underline_) get += "_";
	return get + std::to_string(idx);
}

std::string Names::get_name()
{
	auto get = appended_prefix_ + default_prefix_;
	return get + std::to_string(++default_prefix_used_count_);
}
